/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "logdevice/common/StopReadingRequest.h"

#include "logdevice/common/DataRecordOwnsPayload.h"
#include "logdevice/common/Semaphore.h"
#include "logdevice/common/Worker.h"
#include "logdevice/common/client_read_stream/AllClientReadStreams.h"

namespace facebook { namespace logdevice {

Request::Execution StopReadingRequest::execute() {
  Worker* w = Worker::onThisThread();
  bool found = w->clientReadStreams().erase(stop_handle_.read_stream_id);
  if (!found) {
    RATELIMIT_INFO(std::chrono::seconds(10),
                   2,
                   "Stream %lu not found. Ignoring. This should be rare.",
                   stop_handle_.read_stream_id.val());
  }
  if (callback_) {
    callback_();
  }
  return Execution::COMPLETE;
}

}} // namespace facebook::logdevice
