/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <sstream>

#include "CallbackOStream.h"
#include "HeapProfilerDomainAgent.h"

#include <hermes/cdp/MessageConverters.h>
#include <hermes/cdp/RemoteObjectConverters.h>
#include <jsi/instrumentation.h>

namespace facebook {
namespace hermes {
namespace cdp {

HeapProfilerDomainAgent::HeapProfilerDomainAgent(
    int32_t executionContextID,
    HermesRuntime &runtime,
    SynchronizedOutboundCallback messageCallback,
    std::shared_ptr<RemoteObjectsTable> objTable)
    : DomainAgent(executionContextID, messageCallback, objTable),
      runtime_(runtime) {}

HeapProfilerDomainAgent::~HeapProfilerDomainAgent() {
  if (trackingHeapObjectStackTraces_) {
    runtime_.instrumentation().stopTrackingHeapObjectStackTraces();
  }
}

/// Handles HeapProfiler.takeHeapSnapshot request
void HeapProfilerDomainAgent::takeHeapSnapshot(
    const m::heapProfiler::TakeHeapSnapshotRequest &req) {
  sendSnapshot(req.id, req.reportProgress && *req.reportProgress);
}

void HeapProfilerDomainAgent::sendSnapshot(int reqId, bool reportProgress) {
  if (reportProgress) {
    // A progress notification with finished = true indicates the
    // snapshot has been captured and is ready to be sent.  Our
    // implementation streams the snapshot as it is being captured,
    // so we must send this notification first.
    m::heapProfiler::ReportHeapSnapshotProgressNotification note;
    note.done = 1;
    note.total = 1;
    note.finished = true;
    sendNotificationToClient(note);
  }

  // The CallbackOStream buffers data and invokes the callback whenever
  // the chunk size is reached. It can also invoke the callback once more
  // upon destruction, emitting the final partially-filled chunk. Make sure
  // the stream goes out of scope and the final chunk is emitted before
  // sending the OK response.
  {
    // Size picked to match V8:
    // https://github.com/v8/v8/blob/45a5a44dd4397af6fdaee623f72999c8490cd8e3/src/inspector/v8-heap-profiler-agent-impl.cc#L93
    CallbackOStream cos(
        /* sz */ 100 << 10, [this](std::string s) {
          m::heapProfiler::AddHeapSnapshotChunkNotification note;
          note.chunk = std::move(s);
          sendNotificationToClient(note);
          return true;
        });

    runtime_.instrumentation().createSnapshotToStream(cos);
  }
  sendResponseToClient(m::makeOkResponse(reqId));
}

void HeapProfilerDomainAgent::getObjectByHeapObjectId(
    const m::heapProfiler::GetObjectByHeapObjectIdRequest &req) {
  uint64_t objID = atoi(req.objectId.c_str());
  jsi::Value val = runtime_.getObjectForID(objID);
  if (val.isNull()) {
    sendResponseToClient(m::makeErrorResponse(
        req.id, m::ErrorCode::ServerError, "Unknown object"));
    return;
  }

  std::string group = req.objectGroup.value_or("");
  m::runtime::RemoteObject remoteObj = m::runtime::makeRemoteObject(
      runtime_, val, *objTable_, group, false, false);
  if (remoteObj.type.empty()) {
    sendResponseToClient(m::makeErrorResponse(
        req.id, m::ErrorCode::ServerError, "Remote object is not available"));
    return;
  }

  m::heapProfiler::GetObjectByHeapObjectIdResponse resp;
  resp.id = req.id;
  resp.result = std::move(remoteObj);
  sendResponseToClient(resp);
}

void HeapProfilerDomainAgent::getHeapObjectId(
    const m::heapProfiler::GetHeapObjectIdRequest &req) {
  uint64_t snapshotID = 0;
  if (const jsi::Value *valuePtr = objTable_->getValue(req.objectId)) {
    snapshotID = runtime_.getUniqueID(*valuePtr);
  }

  if (snapshotID) {
    m::heapProfiler::GetHeapObjectIdResponse resp;
    resp.id = req.id;
    // std::to_string is not available on Android, use a std::ostream
    // instead.
    std::ostringstream stream;
    stream << snapshotID;
    resp.heapSnapshotObjectId = stream.str();
    sendResponseToClient(resp);
  } else {
    sendResponseToClient(m::makeErrorResponse(
        req.id, m::ErrorCode::ServerError, "Object is not available"));
  }
}

void HeapProfilerDomainAgent::collectGarbage(
    const m::heapProfiler::CollectGarbageRequest &req) {
  runtime_.instrumentation().collectGarbage("inspector");
  sendResponseToClient(m::makeOkResponse(req.id));
}

void HeapProfilerDomainAgent::startTrackingHeapObjects(
    const m::heapProfiler::StartTrackingHeapObjectsRequest &req) {
  if (trackingHeapObjectStackTraces_) {
    sendResponseToClient(m::makeErrorResponse(
        req.id, m::ErrorCode::InvalidRequest, "Already tracking heap objects"));
    return;
  }

  // Update state before registering the callback, as it may be invoked
  // immediately.
  trackingHeapObjectStackTraces_ = true;
  sendResponseToClient(m::makeOkResponse(req.id));

  // Register for heap object stack trace callbacks.
  // NOTE: As with most profiling/tracing operations, the runtime only supports
  // a single tracking session at a time, so this does not support multiple CDP
  // agents capturing this trace simultaneously.
  runtime_.instrumentation().startTrackingHeapObjectStackTraces(
      [this](
          uint64_t lastSeenObjectId,
          std::chrono::microseconds timestamp,
          std::vector<jsi::Instrumentation::HeapStatsUpdate> stats) {
        // Send the last object ID notification first.
        m::heapProfiler::LastSeenObjectIdNotification note;
        note.lastSeenObjectId = lastSeenObjectId;
        // The protocol uses milliseconds with a fraction for
        // microseconds.
        note.timestamp = static_cast<double>(timestamp.count()) / 1000;
        sendNotificationToClient(note);

        m::heapProfiler::HeapStatsUpdateNotification heapStatsNote;
        // Flatten the HeapStatsUpdate list.
        heapStatsNote.statsUpdate.reserve(stats.size() * 3);
        for (const jsi::Instrumentation::HeapStatsUpdate &fragment : stats) {
          // Each triplet is the fragment number, the total count of
          // objects for the fragment, and the total size of objects
          // for the fragment.
          heapStatsNote.statsUpdate.push_back(
              static_cast<int>(std::get<0>(fragment)));
          heapStatsNote.statsUpdate.push_back(
              static_cast<int>(std::get<1>(fragment)));
          heapStatsNote.statsUpdate.push_back(
              static_cast<int>(std::get<2>(fragment)));
        }
        assert(
            heapStatsNote.statsUpdate.size() == stats.size() * 3 &&
            "Should be exactly 3x the stats vector");
        // TODO: Chunk this if there are too many fragments to
        // update. Unlikely to be a problem in practice unless
        // there's a huge amount of allocation and freeing.
        sendNotificationToClient(heapStatsNote);
      });
}

void HeapProfilerDomainAgent::stopTrackingHeapObjects(
    const m::heapProfiler::StopTrackingHeapObjectsRequest &req) {
  if (!trackingHeapObjectStackTraces_) {
    sendResponseToClient(m::makeErrorResponse(
        req.id, m::ErrorCode::InvalidRequest, "Not tracking heap objects"));
    return;
  }

  runtime_.instrumentation().stopTrackingHeapObjectStackTraces();
  trackingHeapObjectStackTraces_ = false;
  sendSnapshot(req.id, req.reportProgress && *req.reportProgress);
}

} // namespace cdp
} // namespace hermes
} // namespace facebook