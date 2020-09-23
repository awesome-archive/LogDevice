/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "logdevice/common/buffered_writer/BufferedWriterSingleLog.h"

#include <chrono>
#include <lz4.h>
#include <lz4hc.h>
#include <zstd.h>

#include <folly/IntrusiveList.h>
#include <folly/Memory.h>
#include <folly/Overload.h>
#include <folly/Random.h>
#include <folly/ScopeGuard.h>
#include <folly/Varint.h>

#include "logdevice/common/AppendRequest.h"
#include "logdevice/common/PayloadGroupCodec.h"
#include "logdevice/common/Processor.h"
#include "logdevice/common/SimpleEnumMap.h"
#include "logdevice/common/Timer.h"
#include "logdevice/common/Worker.h"
#include "logdevice/common/buffered_writer/BufferedWriteCodec.h"
#include "logdevice/common/buffered_writer/BufferedWriteDecoderImpl.h"
#include "logdevice/common/buffered_writer/BufferedWriterImpl.h"
#include "logdevice/common/buffered_writer/BufferedWriterShard.h"
#include "logdevice/common/debug.h"
#include "logdevice/common/stats/Stats.h"

namespace facebook { namespace logdevice {

using namespace std::literals::chrono_literals;

const SimpleEnumMap<BufferedWriterSingleLog::Batch::State, std::string>&
BufferedWriterSingleLog::Batch::names() {
  static SimpleEnumMap<BufferedWriterSingleLog::Batch::State, std::string>
      s_names({{State::BUILDING, "BUILDING"},
               {State::CONSTRUCTING_BLOB, "CONSTRUCTING_BLOB"},
               {State::READY_TO_SEND, "READY_TO_SEND"},
               {State::INFLIGHT, "INFLIGHT"},
               {State::RETRY_PENDING, "RETRY_PENDING"},
               {State::FINISHED, "FINISHED"}});

  return s_names;
}

using batch_flags_t = BufferedWriteDecoderImpl::flags_t;
using Compression = BufferedWriter::Options::Compression;
using Flags = BufferedWriteDecoderImpl::Flags;

BufferedWriterSingleLog::BufferedWriterSingleLog(BufferedWriterShard* parent,
                                                 logid_t log_id,
                                                 GetLogOptionsFunc get_options)
    : parent_(parent),
      log_id_(log_id),
      get_log_options_(std::move(get_options)),
      options_(get_log_options_(log_id_)) {}

BufferedWriterSingleLog::~BufferedWriterSingleLog() {
  for (std::unique_ptr<Batch>& batch : *batches_) {
    if (batch->state != Batch::State::FINISHED) {
      invokeCallbacks(*batch, E::SHUTDOWN, DataRecord(), NodeID());
      finishBatch(*batch);
    }
  }

  dropBlockedAppends(E::SHUTDOWN, NodeID());
}

void BufferedWriterSingleLog::append(AppendChunk chunk) {
  int rv = appendImpl(chunk, /* defer_client_size_trigger */ false);
  if (rv != 0) {
    // Buffer this chunk; when the inflight batch finishes we will re-call
    // appendImpl().
    blocked_appends_->push_back(std::move(chunk));
    blocked_appends_.observe();
    // The new append can be flushed; need to inform parent
    flushableMayHaveChanged();
    ld_check(is_flushable_);
  }
}

int BufferedWriterSingleLog::appendImpl(AppendChunk& chunk,
                                        bool defer_client_size_trigger) {
  // Calculate how many bytes (approx.) these records will take up in the memory
  size_t payload_memory_bytes_added = 0;
  for (const BufferedWriter::Append& append : chunk) {
    const auto& payload = append.payload;
    payload_memory_bytes_added +=
        BufferedWriterPayloadMeter::memorySize(payload);
  }
  const size_t max_payload_size = Worker::settings().max_payload_size;

  if (haveBuildingBatch()) {
    auto& batch = batches_->back();
    // Calculate how many bytes these records will take up in the blob
    for (const BufferedWriter::Append& append : chunk) {
      std::visit(folly::overload(
                     [&](const std::string& payload) {
                       auto iobuf = folly::IOBuf::wrapBufferAsValue(
                           payload.data(), payload.size());
                       batch->blob_size_estimator.append(iobuf);
                     },
                     [&](const PayloadGroup& payload_group) {
                       batch->blob_size_estimator.append(payload_group);
                     }),
                 append.payload);
    }
    const size_t new_blob_bytes_total =
        batch->blob_size_estimator.calculateSize(checksumBits());
    if (new_blob_bytes_total > max_payload_size) {
      // These records would take us over the payload size limit. Flush the
      // already buffered records first, then we will create a new batch for
      // these records.
      StatsHolder* stats{parent_->parent_->processor()->stats_};
      STAT_INCR(stats, buffered_writer_max_payload_flush);
      flushBuildingBatch();
      ld_check(!haveBuildingBatch());
    } else {
      batch->blob_bytes_total = new_blob_bytes_total;
      batch->blob_format = batch->blob_size_estimator.getFormat();
    }
  }

  // If there is no batch in the BUILDING state, create one now.
  if (!haveBuildingBatch()) {
    if (options_.mode == BufferedWriter::Options::Mode::ONE_AT_A_TIME &&
        !batches_->empty()) {
      // In the one-at-a-time mode, if there is already a batch inflight, we
      // must wait for it to finish before creating a new batch.
      return -1;
    }

    // Refresh log options once per batch.
    options_ = get_log_options_(log_id_);

    auto batch = std::make_unique<Batch>(next_batch_num_++);

    // Calculate how many bytes these records will take up in the blob
    for (const BufferedWriter::Append& append : chunk) {
      std::visit(folly::overload(
                     [&](const std::string& payload) {
                       auto iobuf = folly::IOBuf::wrapBufferAsValue(
                           payload.data(), payload.size());
                       batch->blob_size_estimator.append(iobuf);
                     },
                     [&](const PayloadGroup& payload_group) {
                       batch->blob_size_estimator.append(payload_group);
                     }),
                 append.payload);
    }
    batch->blob_bytes_total =
        batch->blob_size_estimator.calculateSize(checksumBits());
    batch->blob_format = batch->blob_size_estimator.getFormat();
    batches_->push_back(std::move(batch));
    // Intentionally setting state after pushing to make sure is_flushable_
    // becomes true *during* the setBatchState() call
    setBatchState(*batches_->back(), Batch::State::BUILDING);
    ld_check(is_flushable_);

    batches_.observe();
    activateTimeTrigger();
  }
  Batch& batch = *batches_->back();
  // Add these appends to the BUILDING batch
  batch.payload_memory_bytes_total += payload_memory_bytes_added;
  ld_check(batch.blob_bytes_total <= MAX_PAYLOAD_SIZE_INTERNAL);
  for (auto& append : chunk) {
    auto& payload = append.payload;
    BufferedWriter::AppendCallback::Context& context = append.context;
    batch.appends.emplace_back(std::move(context), std::move(payload));

    if (append.attrs.optional_keys.find(KeyType::FINDKEY) !=
        append.attrs.optional_keys.end()) {
      const std::string& key = append.attrs.optional_keys[KeyType::FINDKEY];
      // The batch of records will have the smallest custom key from the ones
      // provided by the client for each record.
      if (batch.attrs.optional_keys.find(KeyType::FINDKEY) ==
              batch.attrs.optional_keys.end() ||
          key < batch.attrs.optional_keys[KeyType::FINDKEY]) {
        batch.attrs.optional_keys[KeyType::FINDKEY] = key;
      }
    }
    if (append.attrs.counters.has_value()) {
      const auto& new_counters = append.attrs.counters.value();
      if (!batch.attrs.counters.has_value()) {
        batch.attrs.counters.emplace();
      }
      auto& curr_counters = batch.attrs.counters.value();
      for (auto counter : new_counters) {
        auto it = curr_counters.find(counter.first);
        if (it == curr_counters.end()) {
          curr_counters.emplace(counter);
          continue;
        }
        it->second += counter.second;
      }
    }
  }

  // Check if we hit the size trigger and should flush
  flushMeMaybe(defer_client_size_trigger);
  return 0;
}

void BufferedWriterSingleLog::flushMeMaybe(bool defer_client_size_trigger) {
  ld_check(haveBuildingBatch());
  const Batch& batch = *batches_->back();
  Worker* w = Worker::onThisThread();
  const size_t max_payload_size = w->immutable_settings_->max_payload_size;

  // If we're at the LogDevice hard limit on payload size, flush
  if (batch.blob_bytes_total >= max_payload_size) {
    ld_check(batch.blob_bytes_total <= MAX_PAYLOAD_SIZE_INTERNAL);
    STAT_INCR(w->getStats(), buffered_writer_max_payload_flush);
    flushBuildingBatch();
    return;
  }

  // If client set `Options::size_trigger', check if the sum of payload bytes
  // buffered exceeds it
  if (!defer_client_size_trigger && options_.size_trigger >= 0 &&
      batch.payload_memory_bytes_total >= options_.size_trigger) {
    STAT_INCR(w->getStats(), buffered_writer_size_trigger_flush);
    flushBuildingBatch();
    return;
  }
}

void BufferedWriterSingleLog::flushBuildingBatch() {
  ld_check(is_flushable_);
  ld_check(haveBuildingBatch());
  sendBatch(*batches_->back());
}

void BufferedWriterSingleLog::flushAll() {
  ld_check(is_flushable_);

  if (haveBuildingBatch()) {
    flushBuildingBatch();
  }

  // In the ONE_AT_A_TIME mode, there may be appends blocked because there
  // is a batch currently inflight.  But this flushAll() call is supposed to
  // flush them.  Defer the flush; record the count so that these appends
  // get flushed as soon as the currently inflight batch comes back.
  blocked_appends_flush_deferred_count_ = blocked_appends_->size();

  flushableMayHaveChanged();
  ld_check(!is_flushable_);
}

bool BufferedWriterSingleLog::calculateIsFlushable() const {
  ld_check_le(blocked_appends_flush_deferred_count_, blocked_appends_->size());
  return haveBuildingBatch() ||
      blocked_appends_flush_deferred_count_ < blocked_appends_->size();
}

bool BufferedWriterSingleLog::haveBuildingBatch() const {
  return !batches_->empty() &&
      batches_->back()->state == Batch::State::BUILDING;
}

void BufferedWriterSingleLog::flushableMayHaveChanged() {
  bool new_flushable = calculateIsFlushable();
  if (new_flushable == is_flushable_) {
    return;
  }

  is_flushable_ = new_flushable;
  parent_->setFlushable(*this, is_flushable_);
  if (is_flushable_) {
    // If we're flushable for enough time continuously, do the flush.
    // Note that this applies both to BUILDING batch and to blocked_appends_.
    activateTimeTrigger();
  } else if (time_trigger_timer_) {
    time_trigger_timer_->cancel();
  }
}

struct AppendRequestCallbackImpl {
  void operator()(Status st, const DataRecord& record, NodeID redirect) {
    // Check that the BufferedWriter still exists; might have been destroyed
    // since the append went out
    Worker* w = Worker::onThisThread();
    auto it = w->active_buffered_writers_.find(writer_id_);
    if (it == w->active_buffered_writers_.end()) {
      return;
    }
    parent_->onAppendReply(batch_, st, record, redirect);
  }

  buffered_writer_id_t writer_id_;
  BufferedWriterSingleLog* parent_;
  BufferedWriterSingleLog::Batch& batch_;
};

void BufferedWriterSingleLog::sendBatch(Batch& batch) {
  if (batch.state == Batch::State::BUILDING) {
    ld_check_eq(batch.blob.length(), 0);

    setBatchState(batch, Batch::State::CONSTRUCTING_BLOB);
    construct_blob(
        batch, checksumBits(), options_.compression, options_.destroy_payloads);
  } else {
    // This is a retry, so we must have already sent it, so we can skip the
    // purgatory of READY_TO_SEND.
    ld_check(batch.state == Batch::State::RETRY_PENDING);
    appendBatch(batch);
  }
}

void BufferedWriterSingleLog::readyToSend(Batch& batch) {
  ld_check(batch.state == Batch::State::CONSTRUCTING_BLOB);

  StatsHolder* stats{parent_->parent_->processor()->stats_};

  // Collect before&after byte counters to give clients an
  // idea of the compression ratio
  STAT_ADD(stats, buffered_writer_bytes_in, batch.payload_memory_bytes_total);
  STAT_ADD(stats, buffered_writer_bytes_batched, batch.blob.length());

  setBatchState(batch, Batch::State::READY_TO_SEND);

  bool sent_at_least_one{false};
  while (!batches_->empty()) {
    size_t index = next_batch_to_send_ - batches_->front()->num;
    if (index >= batches_->size()) {
      break;
    }

    if ((*batches_)[index]->state != Batch::State::READY_TO_SEND) {
      if (!sent_at_least_one) {
        RATELIMIT_WARNING(
            1s,
            1,
            "On log %s, %lu batches are behind next_batch_to_send_ "
            "(%lu), which is in state %s",
            toString(log_id_).c_str(),
            batches_->size() - index,
            next_batch_to_send_,
            Batch::names()[(*batches_)[index]->state].c_str());
      }

      break;
    }

    next_batch_to_send_++;
    appendBatch(*(*batches_)[index]);
    sent_at_least_one = true;
  }
}

void BufferedWriterSingleLog::appendBatch(Batch& batch) {
  ld_check(batch.state == Batch::State::READY_TO_SEND ||
           batch.state == Batch::State::RETRY_PENDING);
  ld_check(batch.blob.length() != 0);
  ld_check(!batch.retry_timer || !batch.retry_timer->isActive());

  if (batch.total_size_freed > 0) {
    parent_->parent_->appendSink()->onBytesFreedByWorker(
        batch.total_size_freed);
    // Now that we've recorded that they're freed, reset the counter so we don't
    // double count by double calling onBytesFreedByWorker(), e.g. if we retry.
    batch.total_size_freed = 0;
  }

  setBatchState(batch, Batch::State::INFLIGHT);

  // Call into BufferedWriter::appendBuffered() which in production is just a
  // proxy for ClientImpl::appendBuffered() or
  // SequencerBatching::appendBuffered() but in tests may be overridden to fail
  std::pair<Status, NodeID> rv = parent_->parent_->appendSink()->appendBuffered(
      log_id_,
      batch.appends,
      std::move(batch.attrs),
      PayloadHolder(batch.blob.cloneAsValue()),
      AppendRequestCallbackImpl{parent_->id_, this, batch},
      Worker::onThisThread()->idx_, // need the callback on this same Worker
      checksumBits());
  if (rv.first != E::OK) {
    // Simulate failure reply
    onAppendReply(batch, rv.first, DataRecord(log_id_, Payload()), rv.second);
  }
}

void BufferedWriterSingleLog::onAppendReply(Batch& batch,
                                            Status status,
                                            const DataRecord& dr_batch,
                                            NodeID redirect) {
  ld_spew("status = %s", error_name(status));

  ld_check(batch.state == Batch::State::INFLIGHT);

  if (status != E::OK && scheduleRetry(batch, status, dr_batch) == 0) {
    // Scheduled retry, nothing else to do
    return;
  }

  if (status == E::OK) {
    WORKER_STAT_INCR(buffered_writer_batches_succeeded);
  } else {
    WORKER_STAT_INCR(buffered_writer_batches_failed);
  }

  invokeCallbacks(batch, status, dr_batch, redirect);
  finishBatch(batch);
  reap();

  if (options_.mode == BufferedWriter::Options::Mode::ONE_AT_A_TIME) {
    if (status == E::OK) {
      // Now that some batch finished successfully, reissue any appends that
      // were blocked in ONE_AT_A_TIME mode while the batch was inflight.
      unblockAppends();
    } else {
      // Batch failed (exhausted all retries), also fail any blocked appends to
      // preserve ordering. This is only a best-effort measure; it's inherently
      // racy since more appends may be in flight (e.g. queued
      // BufferedAppendRequest, or user append() calls from another thread).
      dropBlockedAppends(status, redirect);
    }
  }
}

void BufferedWriterSingleLog::quiesce() {
  // Stop all timers.
  if (time_trigger_timer_) {
    time_trigger_timer_->cancel();
  }

  for (std::unique_ptr<Batch>& batch : *batches_) {
    if (batch->retry_timer) {
      batch->retry_timer->cancel();
    }
  }
}

void BufferedWriterSingleLog::reap() {
  while (!batches_->empty() &&
         batches_->front()->state == Batch::State::FINISHED) {
    batches_->pop_front();
  }
  batches_.compact();
}

void BufferedWriterSingleLog::unblockAppends() {
  bool flush_at_end = false;

  while (!blocked_appends_->empty()) {
    // If there are more blocked appends after this one, defer the client size
    // trigger so that we fit as many as possible (still subject to the max
    // payload size limit) in the next batch.
    bool defer_client_size_trigger = blocked_appends_->size() > 1;
    int rv = appendImpl(blocked_appends_->front(), defer_client_size_trigger);
    if (rv != 0) {
      // Blocked.  Must have just flushed a new batch; keep the chunk in
      // `blocked_appends_'.
      break;
    }
    // Chunk contents were moved out of the queue, pop.
    blocked_appends_->pop_front();

    if (blocked_appends_flush_deferred_count_ > 0) {
      // There was a flush() call while this append was blocked.  That flush
      // had to be deferred because there was a batch already inflight and the
      // flush would have created a new one (but we are in ONE_AT_A_TIME
      // mode).
      flush_at_end = true;
      --blocked_appends_flush_deferred_count_;
    }
  }
  blocked_appends_.compact();

  // Despite `flush_at_end == true`, the log may not be flushable if the last
  // call to `appendImpl()` above has flushed the last batch
  if (flush_at_end && haveBuildingBatch()) {
    flushBuildingBatch();
  } else {
    flushableMayHaveChanged();
  }
}

void BufferedWriterSingleLog::dropBlockedAppends(Status status,
                                                 NodeID redirect) {
  BufferedWriterImpl::AppendCallbackInternal* cb =
      parent_->parent_->getCallback();
  int64_t payload_mem_bytes = 0;
  for (auto& chunk : *blocked_appends_) {
    BufferedWriter::AppendCallback::ContextSet context_set;
    for (auto& append : chunk) {
      auto& payload = append.payload;
      payload_mem_bytes += BufferedWriterPayloadMeter::memorySize(payload);
      BufferedWriter::AppendCallback::Context& context = append.context;
      context_set.emplace_back(std::move(context), std::move(payload));
    }
    WORKER_STAT_ADD(
        buffered_append_failed_dropped_behind_failed_batch, chunk.size());
    cb->onFailureInternal(log_id_, std::move(context_set), status, redirect);
  }
  blocked_appends_->clear();
  blocked_appends_.compact();
  blocked_appends_flush_deferred_count_ = 0;
  // Return the memory budget
  parent_->parent_->releaseMemory(payload_mem_bytes);

  flushableMayHaveChanged();
}

void BufferedWriterSingleLog::activateTimeTrigger() {
  if (options_.time_trigger.count() < 0) {
    return;
  }

  if (!time_trigger_timer_) {
    time_trigger_timer_ = std::make_unique<Timer>([this] {
      StatsHolder* stats{parent_->parent_->processor()->stats_};
      STAT_INCR(stats, buffered_writer_time_trigger_flush);
      flushAll();
    });
  }
  if (!time_trigger_timer_->isActive()) {
    time_trigger_timer_->activate(options_.time_trigger);
  }
}

int BufferedWriterSingleLog::scheduleRetry(Batch& batch,
                                           Status status,
                                           const DataRecord& /*dr_batch*/) {
  if (options_.retry_count >= 0 && batch.retry_count >= options_.retry_count) {
    return -1;
  }

  // Invoke retry callback to let the upper layer (application typically) know
  // about the failure (if interested) and check if it wants to block the
  // retry.
  BufferedWriterImpl::AppendCallbackInternal* cb =
      parent_->parent_->getCallback();
  if (cb->onRetry(log_id_, batch.appends, status) !=
      BufferedWriter::AppendCallback::RetryDecision::ALLOW) {
    // Client blocked the retry; bail out.  The caller will invoke the failure
    // callback shortly.
    return -1;
  }

  // Initialize `retry_timer' if this is the first retry
  if (!batch.retry_timer) {
    ld_check(options_.retry_initial_delay.count() >= 0);
    std::chrono::milliseconds max_delay = options_.retry_max_delay.count() >= 0
        ? options_.retry_max_delay
        : std::chrono::milliseconds::max();
    max_delay = std::max(max_delay, options_.retry_initial_delay);

    batch.retry_timer = std::make_unique<ExponentialBackoffTimer>(
        [this, &batch]() { sendBatch(batch); },
        options_.retry_initial_delay,
        max_delay);

    batch.retry_timer->randomize();
  }

  setBatchState(batch, Batch::State::RETRY_PENDING);
  ++batch.retry_count;
  ld_spew(
      "scheduling retry in %ld ms", batch.retry_timer->getNextDelay().count());
  WORKER_STAT_INCR(buffered_writer_retries);
  batch.retry_timer->activate();
  return 0;
}

void BufferedWriterSingleLog::invokeCallbacks(Batch& batch,
                                              Status status,
                                              const DataRecord& dr_batch,
                                              NodeID redirect) {
  ld_check(batch.state != Batch::State::FINISHED);

  BufferedWriterImpl::AppendCallbackInternal* cb =
      parent_->parent_->getCallback();

  if (status == E::OK) {
    WORKER_STAT_ADD(buffered_append_success, batch.appends.size());

    cb->onSuccess(log_id_, std::move(batch.appends), dr_batch.attrs);
  } else {
    if (status == E::SHUTDOWN) {
      WORKER_STAT_ADD(buffered_append_failed_shutdown, batch.appends.size());
    } else {
      WORKER_STAT_ADD(
          buffered_append_failed_actual_append, batch.appends.size());
    }

    cb->onFailureInternal(log_id_, std::move(batch.appends), status, redirect);
  }
}

void BufferedWriterSingleLog::finishBatch(Batch& batch) {
  setBatchState(batch, Batch::State::FINISHED);
  // Make sure payloads are deallocated; invokeCallbacks() ought to have moved
  // them back to the application
  ld_check(batch.appends.empty());
  batch.appends.shrink_to_fit();
  // Free/unlink the buffer.
  batch.blob = folly::IOBuf();
  // Return the memory budget
  parent_->parent_->releaseMemory(batch.payload_memory_bytes_total);
}

void BufferedWriterSingleLog::setBatchState(Batch& batch, Batch::State state) {
  batch.state = state;
  flushableMayHaveChanged();
}

int BufferedWriterSingleLog::checksumBits() const {
  return parent_->parent_->shouldPrependChecksum()
      ? Worker::settings().checksum_bits
      : 0;
}

namespace {
template <typename PayloadsEncoder>
void encode_batch(BufferedWriterSingleLog::Batch& batch,
                  int checksum_bits,
                  Compression compression,
                  int zstd_level,
                  bool destroy_payloads) {
  ld_check(batch.total_size_freed == 0);

  BufferedWriteCodec::Encoder<PayloadsEncoder> encoder(
      checksum_bits, batch.appends.size(), batch.blob_bytes_total);
  for (auto& append : batch.appends) {
    if (destroy_payloads) {
      batch.total_size_freed +=
          BufferedWriterPayloadMeter::memorySize(append.second);

      std::visit(
          folly::overload(
              [&](std::string& payload) {
                // Payload should be destroyed, so pass ownership to the encoder
                std::string* client_payload =
                    new std::string(std::move(payload));
                folly::IOBuf iobuf = folly::IOBuf(
                    folly::IOBuf::TAKE_OWNERSHIP,
                    client_payload->data(),
                    client_payload->size(),
                    +[](void* /* buf */, void* userData) {
                      delete reinterpret_cast<std::string*>(userData);
                    },
                    /* userData */ reinterpret_cast<void*>(client_payload));
                encoder.append(std::move(iobuf));

                // Can't do payload.clear() because that usually doesn't free
                // memory.
                payload.clear();
                payload.shrink_to_fit();
              },
              [&](PayloadGroup& payload_group) {
                encoder.append(payload_group);

                payload_group.clear();
              }),
          append.second);
    } else {
      std::visit(folly::overload(
                     [&](const std::string& client_payload) {
                       // It's safe to wrap buffer, since payloads are preserved
                       // in batch.
                       encoder.append(folly::IOBuf::wrapBufferAsValue(
                           client_payload.data(), client_payload.size()));
                     },
                     [&](const PayloadGroup& payload_group) {
                       encoder.append(payload_group);
                     }),
                 append.second);
    }
  }
  folly::IOBufQueue encoded;
  encoder.encode(encoded, compression, zstd_level);
  batch.blob = encoded.moveAsValue();
}
} // namespace

void BufferedWriterSingleLog::Impl::construct_compressed_blob(
    BufferedWriterSingleLog::Batch& batch,
    int checksum_bits,
    Compression compression,
    int zstd_level,
    bool destroy_payloads) {
  switch (batch.blob_format) {
    case BufferedWriteCodec::Format::SINGLE_PAYLOADS: {
      encode_batch<BufferedWriteSinglePayloadsCodec::Encoder>(
          batch, checksum_bits, compression, zstd_level, destroy_payloads);
      break;
    }
    case BufferedWriteCodec::Format::PAYLOAD_GROUPS: {
      encode_batch<PayloadGroupCodec::Encoder>(
          batch, checksum_bits, compression, zstd_level, destroy_payloads);
      break;
    }
  }
}

void BufferedWriterSingleLog::Impl::construct_blob_long_running(
    BufferedWriterSingleLog::Batch& batch,
    int checksum_bits,
    Compression compression,
    const int zstd_level,
    bool destroy_payloads) {
  ld_check(batch.state == Batch::State::CONSTRUCTING_BLOB);

  construct_compressed_blob(
      batch, checksum_bits, compression, zstd_level, destroy_payloads);
}

void BufferedWriterSingleLog::construct_blob(
    BufferedWriterSingleLog::Batch& batch,
    int checksum_bits,
    Compression compression,
    bool destroy_payloads) {
  ld_check(batch.state == Batch::State::CONSTRUCTING_BLOB);

  if (parent_->parent_->isShuttingDown()) {
    // Our destructor will call callbacks with E::SHUTDOWN.
    return;
  }

  const int zstd_level = Worker::settings().buffered_writer_zstd_level;

  // We need to call construct_blob_long_running(), then callback().  If the
  // batch is large, we send it to a background thread so that this thread can
  // process more requests.  However, if it's small, we just do that inline
  // since the queueing & switching overhead would cost more than we save.

  if (batch.blob_bytes_total <
      Worker::settings().buffered_writer_bg_thread_bytes_threshold) {
    Impl::construct_blob_long_running(
        batch, checksum_bits, compression, zstd_level, destroy_payloads);
    readyToSend(batch);
  } else {
    ProcessorProxy* processor_proxy = parent_->parent_->processorProxy();
    ld_spew("Enqueueing batch %lu for log %s to background thread.  Batches "
            "outstanding for this log: %lu Background tasks: %lu",
            batch.num,
            toString(log_id_).c_str(),
            batches_->size(),
            parent_->parent_->recentNumBackground());

    processor_proxy->processor()->enqueueToBackgroundBlocking(
        [&batch,
         checksum_bits,
         destroy_payloads,
         processor_proxy,
         trigger = parent_->parent_->getBackgroundTaskCountHolder(),
         thread_affinity = Worker::onThisThread()->idx_.val(),
         compression,
         zstd_level,
         this]() mutable {
          BufferedWriterSingleLog::Impl::construct_blob_long_running(
              batch, checksum_bits, compression, zstd_level, destroy_payloads);
          std::unique_ptr<Request> request =
              std::make_unique<ContinueBlobSendRequest>(
                  this, batch, thread_affinity);
          // Since this runs on the background thread, be careful not to access
          // non-constant fields of "this", unless you know what you're doing.
          ld_spew("Done constructing batch %lu for log %s, posting back to "
                  "worker.  Background tasks: %lu",
                  batch.num,
                  toString(log_id_).c_str(),
                  parent_->parent_->recentNumBackground());

          int rc = processor_proxy->postWithRetrying(request);
          if (rc != 0) {
            ld_error("Processor::postWithRetrying() failed: %d", rc);
          }
        });
  }
}

}} // namespace facebook::logdevice
