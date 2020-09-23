/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include <folly/Optional.h>

#include "logdevice/admin/AdminCommandAPIHandler.h"
#include "logdevice/admin/CheckImpactHandler.h"
#include "logdevice/admin/ClusterMembershipAPIHandler.h"
#include "logdevice/admin/MaintenanceAPIHandler.h"
#include "logdevice/admin/NodesConfigAPIHandler.h"
#include "logdevice/admin/NodesStateAPIHandler.h"
#include "logdevice/admin/if/gen-cpp2/AdminAPI.h"
#include "logdevice/common/NodeID.h"
#include "logdevice/common/WorkerType.h"
#include "logdevice/common/types_internal.h"
#include "logdevice/server/locallogstore/ShardedRocksDBLocalLogStore.h"
#include "logdevice/server/thrift/LogDeviceThriftHandler.h"

namespace facebook { namespace logdevice {
class SafetyChecker;
/**
 * This is the Admin API Handler class, here we expect to see all callback
 * functions for the admin thrift interface to be implemented.
 *
 * You can use the synchronous version or the Future-based version based on
 * your preference and how long the operation might take. It's always
 * preferred to used the future_Fn flavor if you are performing an operation
 * on a background worker.
 */
class AdminAPIHandler : public LogDeviceThriftHandler,
                        public NodesConfigAPIHandler,
                        public NodesStateAPIHandler,
                        public CheckImpactHandler,
                        public MaintenanceAPIHandler,
                        public ClusterMembershipAPIHandler,
                        public AdminCommandAPIHandler {
 public:
  AdminAPIHandler(
      const std::string& service_name,
      Processor* processor,
      std::shared_ptr<SettingsUpdater> settings_updater,
      UpdateableSettings<ServerSettings> updateable_server_settings,
      UpdateableSettings<AdminServerSettings> updateable_admin_server_settings,
      StatsHolder* stats_holder);

  facebook::fb303::cpp2::fb_status getStatus() override;

  // *** LogTree-related APIs
  void getLogTreeInfo(thrift::LogTreeInfo&) override;
  void getReplicationInfo(thrift::ReplicationInfo&) override;

  // Sync version since this response can be constructed quite fast
  void getSettings(thrift::SettingsResponse& response,
                   std::unique_ptr<thrift::SettingsRequest> request) override;

  // Set a temporary override setting
  folly::SemiFuture<folly::Unit> semifuture_applySettingOverride(
      std::unique_ptr<thrift::ApplySettingOverrideRequest> request) override;

  // Unset a temporary override setting
  folly::SemiFuture<folly::Unit> semifuture_removeSettingOverride(
      std::unique_ptr<thrift::RemoveSettingOverrideRequest> request) override;

  // Take a snapshot of the LogTree running on this server.
  folly::SemiFuture<folly::Unit>
  semifuture_takeLogTreeSnapshot(thrift::unsigned64 min_version) override;

  // Take a snapshot of the Maintenance State running on this server.
  folly::SemiFuture<folly::Unit> semifuture_takeMaintenanceLogSnapshot(
      thrift::unsigned64 min_version) override;

  void getLogGroupThroughput(
      thrift::LogGroupThroughputResponse& response,
      std::unique_ptr<thrift::LogGroupThroughputRequest> request) override;

  void getLogGroupCustomCounters(
      thrift::LogGroupCustomCountersResponse& response,
      std::unique_ptr<thrift::LogGroupCustomCountersRequest> request) override;

  void dumpServerConfigJson(std::string& response) override;
  void getClusterName(std::string& response) override;
};
}} // namespace facebook::logdevice
