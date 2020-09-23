/**
 * Copyright (c) 2019-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include <folly/io/async/HHWheelTimer.h>

#include "logdevice/common/VersionedConfigStore.h"
#include "logdevice/common/WeakRefHolder.h"
#include "logdevice/include/CheckpointStore.h"
#include "logdevice/include/Err.h"
#include "logdevice/lib/checkpointing/if/gen-cpp2/Checkpoint_types.h"

namespace facebook { namespace logdevice {
/*
 * @file CheckpointStoreImpl implements CheckpointStore. It stores LSNs for logs
 *       using VersionedConfigStore.
 */
class CheckpointStoreImpl : public CheckpointStore {
 public:
  /**
   * @param prefix: the string which will be added at the beginning of every
   *   key.
   */
  explicit CheckpointStoreImpl(std::unique_ptr<VersionedConfigStore> vcs,
                               const std::string& prefix = "");

  void getLSN(const std::string& customer_id,
              logid_t log_id,
              GetCallback cb) const override;

  Status getLSNSync(const std::string& customer_id,
                    logid_t log_id,
                    lsn_t* value_out) const override;

  Status updateLSNSync(const std::string& customer_id,
                       logid_t log_id,
                       lsn_t lsn) override;

  Status updateLSNSync(const std::string& customer_id,
                       const std::map<logid_t, lsn_t>& checkpoints) override;

  void updateLSN(const std::string& customer_id,
                 logid_t log_id,
                 lsn_t lsn,
                 StatusCallback cb) override;

  void updateLSN(const std::string& customer_id,
                 const std::map<logid_t, lsn_t>& checkpoints,
                 StatusCallback cb) override;

  void removeCheckpoints(const std::string& customer_id,
                         const std::vector<logid_t>& checkpoints,
                         StatusCallback cb) override;

  void removeAllCheckpoints(const std::string& customer_id,
                            StatusCallback cb) override;

  Status
  removeCheckpointsSync(const std::string& customer_id,
                        const std::vector<logid_t>& checkpoints) override;

  Status removeAllCheckpointsSync(const std::string& customer_id) override;

  static folly::Optional<CheckpointStore::Version>
      extractVersion(folly::StringPiece);

 private:
  static constexpr auto kRetryDuration = std::chrono::seconds(1);

  void updateCheckpoints(
      const std::string& customer_id,
      folly::Function<void(checkpointing::thrift::Checkpoint&) const>
          modify_checkpoint,
      StatusCallback cb);

  std::string createKey(const std::string& customer_id) const;

  std::unique_ptr<VersionedConfigStore> vcs_;
  std::string prefix_;
  folly::EventBase* event_base_;
  folly::HHWheelTimer::UniquePtr timer_;
  WeakRefHolder<CheckpointStoreImpl> holder_;
};

}} // namespace facebook::logdevice
