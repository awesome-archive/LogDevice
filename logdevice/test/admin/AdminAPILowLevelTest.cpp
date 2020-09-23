/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <chrono>

#include <gtest/gtest.h>

#include "logdevice/common/test/TestUtil.h"
#include "logdevice/include/Client.h"
#include "logdevice/include/Record.h"
#include "logdevice/test/utils/AdminAPITestUtils.h"
#include "logdevice/test/utils/AppendThread.h"
#include "logdevice/test/utils/IntegrationTestBase.h"
#include "logdevice/test/utils/IntegrationTestUtils.h"
#include "logdevice/test/utils/ReaderThread.h"

using namespace facebook::logdevice;
using namespace facebook::logdevice::thrift;

class AdminAPILowLevelTest : public IntegrationTestBase {};

TEST_F(AdminAPILowLevelTest, BasicThriftClientCreation) {
  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .useHashBasedSequencerAssignment()
                     .create(3);

  cluster->waitUntilAllAvailable();
  auto admin_client = cluster->getNode(0).createAdminClient();
  ASSERT_NE(nullptr, admin_client);

  auto fbStatus = admin_client->sync_getStatus();
  ASSERT_EQ(facebook::fb303::cpp2::fb_status::ALIVE, fbStatus);
}

TEST_F(AdminAPILowLevelTest, LogTreeReplicationInfo) {
  auto internal_log_attrs = logsconfig::LogAttributes()
                                .with_singleWriter(false)
                                .with_replicationFactor(2)
                                .with_extraCopies(0)
                                .with_syncedCopies(0);
  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .setConfigLogAttributes(internal_log_attrs)
                     .enableLogsConfigManager()
                     .useHashBasedSequencerAssignment()
                     .create(3);

  cluster->waitUntilAllAvailable();
  auto admin_client = cluster->getNode(0).createAdminClient();
  ASSERT_NE(nullptr, admin_client);

  // Let's create some log groups
  std::unique_ptr<ClientSettings> settings(ClientSettings::create());
  settings->set("on-demand-logs-config", "true");
  std::shared_ptr<Client> client =
      cluster->createClient(std::chrono::seconds(60), std::move(settings));

  cluster->waitUntilAllSequencersQuiescent();

  auto lg1 = client->makeLogGroupSync(
      "/log1",
      logid_range_t(logid_t(1), logid_t(100)),
      client::LogAttributes()
          .with_replicateAcross({{NodeLocationScope::RACK, 3}})
          .with_backlogDuration(std::chrono::seconds(60)),
      false);

  ASSERT_TRUE(lg1);
  auto lg2 = client->makeLogGroupSync(
      "/log2",
      logid_range_t(logid_t(200), logid_t(300)),
      client::LogAttributes()
          .with_replicateAcross(
              {{NodeLocationScope::RACK, 2}, {NodeLocationScope::NODE, 3}})
          .with_backlogDuration(std::chrono::seconds(30)),
      false);
  ASSERT_TRUE(lg2);

  auto target_version = std::to_string(lg2->version());

  ReplicationInfo info;
  int give_up = 10;
  do {
    // let's keep trying until the server hits that version
    admin_client->sync_getReplicationInfo(info);
    ld_info("Still waiting for the server to sync to config version '%s'"
            " instead we have received '%s', retries left %i.",
            target_version.c_str(),
            info.get_version().c_str(),
            give_up);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    give_up--;
  } while (info.get_version() != target_version && give_up > 0);

  ASSERT_EQ(target_version, info.get_version());
  ASSERT_EQ(2, info.get_smallest_replication_factor());
  std::map<thrift::LocationScope, int32_t> narrowest_expected{
      {thrift::LocationScope::NODE, 2}};
  ASSERT_EQ(narrowest_expected, info.get_narrowest_replication());
  auto tolerable_failure_domains = info.get_tolerable_failure_domains();
  ASSERT_EQ(
      thrift::LocationScope::NODE, tolerable_failure_domains.get_domain());
  ASSERT_EQ(1, tolerable_failure_domains.get_count());

  LogTreeInfo logtree;
  admin_client->sync_getLogTreeInfo(logtree);
  ASSERT_EQ(target_version, logtree.get_version());
  // 200 normal logs + 6 internal logs
  ASSERT_EQ(206, logtree.get_num_logs());
  ASSERT_EQ(60, logtree.get_max_backlog_seconds());
  ASSERT_TRUE(logtree.get_is_fully_loaded());
}

TEST_F(AdminAPILowLevelTest, TakeLogTreeSnapshot) {
  const int node_count = 3;
  auto internal_log_attrs = logsconfig::LogAttributes()
                                .with_singleWriter(false)
                                .with_replicationFactor(2)
                                .with_extraCopies(0)
                                .with_syncedCopies(0);
  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .setConfigLogAttributes(internal_log_attrs)
                     .enableLogsConfigManager()
                     .useHashBasedSequencerAssignment()
                     .create(node_count);

  cluster->waitUntilAllAvailable();
  auto admin_client = cluster->getNode(0).createAdminClient();
  ASSERT_NE(nullptr, admin_client);

  // Let's create some log groups
  std::unique_ptr<ClientSettings> settings(ClientSettings::create());
  settings->set("on-demand-logs-config", "true");
  std::shared_ptr<Client> client =
      cluster->createClient(std::chrono::seconds(60), std::move(settings));

  cluster->waitUntilAllSequencersQuiescent();

  auto lg1 = client->makeLogGroupSync(
      "/log1",
      logid_range_t(logid_t(1), logid_t(100)),
      client::LogAttributes()
          .with_replicateAcross({{NodeLocationScope::RACK, 3}})
          .with_backlogDuration(std::chrono::seconds(60)),
      false);

  ASSERT_TRUE(lg1);
  auto server_version = std::to_string(lg1->version());

  // Takes a log-tree snapshot regardless of the version. This shouldn't throw
  // exceptions.
  wait_until([&]() {
    LogTreeInfo logtree;
    admin_client->sync_getLogTreeInfo(logtree);
    return logtree.get_is_fully_loaded();
  });
  admin_client->sync_takeLogTreeSnapshot(0);
  auto unrealistic_version = 999999999999999999l;
  ASSERT_THROW(admin_client->sync_takeLogTreeSnapshot(unrealistic_version),
               thrift::StaleVersion);
}

TEST_F(AdminAPILowLevelTest, TakeMaintenanceLogSnapshot) {
  const int node_count = 3;
  auto internal_log_attrs = logsconfig::LogAttributes()
                                .with_singleWriter(false)
                                .with_replicationFactor(2)
                                .with_extraCopies(0)
                                .with_syncedCopies(0);
  auto cluster =
      IntegrationTestUtils::ClusterFactory()
          .setConfigLogAttributes(internal_log_attrs)
          .setParam("--enable-cluster-maintenance-state-machine", "true")
          .setParam("--maintenance-log-snapshotting", "true")
          .enableLogsConfigManager()
          .useHashBasedSequencerAssignment()
          .create(node_count);

  cluster->waitUntilAllAvailable();
  auto admin_client = cluster->getNode(0).createAdminClient();
  ASSERT_NE(nullptr, admin_client);

  cluster->waitUntilAllSequencersQuiescent();

  // Takes a log-tree snapshot regardless of the version. This shouldn't throw
  // exceptions.
  wait_until([&]() {
    try {
      admin_client->sync_takeMaintenanceLogSnapshot(0);
      return true;
    } catch (thrift::NodeNotReady& e) {
      return false;
    }
  });
  auto unrealistic_version = 999999999999999999l;
  ASSERT_THROW(
      admin_client->sync_takeMaintenanceLogSnapshot(unrealistic_version),
      thrift::StaleVersion);
}

TEST_F(AdminAPILowLevelTest, SettingsAPITest) {
  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .setParam("--store-timeout", "1s..12s")
                     .useHashBasedSequencerAssignment()
                     .create(2);
  cluster->waitUntilAllAvailable();
  auto admin_client = cluster->getNode(0).createAdminClient();
  ASSERT_NE(nullptr, admin_client);

  auto fbStatus = admin_client->sync_getStatus();
  ASSERT_EQ(facebook::fb303::cpp2::fb_status::ALIVE, fbStatus);

  thrift::SettingsResponse response;
  admin_client->sync_getSettings(response, thrift::SettingsRequest());

  // For LogsConfig manager, we pass this in the CONFIG
  auto& logsconfig_setting =
      response.settings_ref()["enable-logsconfig-manager"];
  ASSERT_EQ("false", *logsconfig_setting.currentValue_ref());
  ASSERT_EQ("true", *logsconfig_setting.defaultValue_ref());
  ASSERT_EQ("false",
            logsconfig_setting.sources_ref()
                ->find(thrift::SettingSource::CONFIG)
                ->second);
  ASSERT_TRUE(logsconfig_setting.sources_ref()->end() ==
              logsconfig_setting.sources_ref()->find(
                  thrift::SettingSource::ADMIN_OVERRIDE));

  auto& store_timeout_setting = response.settings_ref()["store-timeout"];
  ASSERT_EQ("1s..12s",
            store_timeout_setting.sources_ref()
                ->find(thrift::SettingSource::CONFIG)
                ->second);

  // Check setting filtering
  auto filtered_request = thrift::SettingsRequest();
  std::set<std::string> filtered_settings;
  filtered_settings.insert("rebuilding-local-window");
  filtered_settings.insert("store-timeout");

  thrift::SettingsResponse filtered_response;
  filtered_request.set_settings(std::move(filtered_settings));
  admin_client->sync_getSettings(filtered_response, filtered_request);

  // Check that not all settings are in the response
  ASSERT_TRUE(
      filtered_response.settings_ref()->find("enable-logsconfig-manager") ==
      filtered_response.settings_ref()->end());

  // Check that requested settings are in there
  auto& rebuilding_window_setting =
      filtered_response.settings_ref()["rebuilding-local-window"];
  ASSERT_EQ("60min", *rebuilding_window_setting.currentValue_ref());
  store_timeout_setting = filtered_response.settings_ref()["store-timeout"];
  ASSERT_EQ("1s..12s", *store_timeout_setting.currentValue_ref());
}

TEST_F(AdminAPILowLevelTest, ApplySettingOverrideAPITest) {
  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .setParam("--rebuilding-local-window", "60min")
                     .useHashBasedSequencerAssignment()
                     .create(2);

  cluster->waitUntilAllAvailable();
  auto admin_client = cluster->getNode(0).createAdminClient();
  ASSERT_NE(nullptr, admin_client);

  auto fbStatus = admin_client->sync_getStatus();
  ASSERT_EQ(facebook::fb303::cpp2::fb_status::ALIVE, fbStatus);

  // Apply a temporary override
  auto apply_setting_override_request = thrift::ApplySettingOverrideRequest();
  *apply_setting_override_request.name_ref() = "rebuilding-local-window";
  *apply_setting_override_request.value_ref() = "30min";
  *apply_setting_override_request.ttl_seconds_ref() = 3;
  admin_client->sync_applySettingOverride(apply_setting_override_request);

  thrift::SettingsResponse response;
  // Check that overridden setting is in there
  admin_client->sync_getSettings(response, thrift::SettingsRequest());
  auto& rebuilding_window_setting =
      response.settings_ref()["rebuilding-local-window"];
  ASSERT_EQ("30min", *rebuilding_window_setting.currentValue_ref());

  // Wait for the TTL to expire and the setting to get removed
  auto setting_applied = wait_until(
      [&]() {
        thrift::SettingsResponse response2;
        admin_client->sync_getSettings(response2, thrift::SettingsRequest());
        rebuilding_window_setting =
            response2.settings_ref()["rebuilding-local-window"];
        return *rebuilding_window_setting.currentValue_ref() == "60min";
      },
      std::chrono::steady_clock::now() + std::chrono::seconds(30));
  ASSERT_EQ(0, setting_applied);
}

TEST_F(AdminAPILowLevelTest, ApplySettingOverrideAPITestValidation) {
  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .useHashBasedSequencerAssignment()
                     .create(2);

  cluster->waitUntilAllAvailable();
  auto admin_client = cluster->getNode(0).createAdminClient();
  ASSERT_NE(nullptr, admin_client);

  auto fbStatus = admin_client->sync_getStatus();
  ASSERT_EQ(facebook::fb303::cpp2::fb_status::ALIVE, fbStatus);

  // Zero TTL isn't allowed
  auto request1 = thrift::ApplySettingOverrideRequest();
  *request1.name_ref() = "rebuilding-local-window";
  *request1.value_ref() = "30min";
  *request1.ttl_seconds_ref() = 0;
  ASSERT_THROW(admin_client->sync_applySettingOverride(request1),
               thrift::InvalidRequest);

  // Negative TTL isn't allowed
  auto request2 = thrift::ApplySettingOverrideRequest();
  *request2.name_ref() = "rebuilding-local-window";
  *request2.value_ref() = "30min";
  *request2.ttl_seconds_ref() = -1;
  ASSERT_THROW(admin_client->sync_applySettingOverride(request2),
               thrift::InvalidRequest);

  // Invalid setting name
  auto request3 = thrift::ApplySettingOverrideRequest();
  *request3.name_ref() = "foo";
  *request3.value_ref() = "30min";
  *request3.ttl_seconds_ref() = 1;
  ASSERT_THROW(admin_client->sync_applySettingOverride(request2),
               thrift::InvalidRequest);
}

TEST_F(AdminAPILowLevelTest, RemoveSettingOverrideAPITest) {
  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .setParam("--rebuilding-local-window", "60min")
                     .useHashBasedSequencerAssignment()
                     .create(2);

  cluster->waitUntilAllAvailable();
  auto admin_client = cluster->getNode(0).createAdminClient();
  ASSERT_NE(nullptr, admin_client);

  auto fbStatus = admin_client->sync_getStatus();
  ASSERT_EQ(facebook::fb303::cpp2::fb_status::ALIVE, fbStatus);

  // Apply a temporary override
  auto apply_setting_override_request = thrift::ApplySettingOverrideRequest();
  *apply_setting_override_request.name_ref() = "rebuilding-local-window";
  *apply_setting_override_request.value_ref() = "30min";
  *apply_setting_override_request.ttl_seconds_ref() = 3600;
  admin_client->sync_applySettingOverride(apply_setting_override_request);

  thrift::SettingsResponse response;
  // Check that overridden setting is in there
  admin_client->sync_getSettings(response, thrift::SettingsRequest());
  auto& rebuilding_window_setting =
      response.settings_ref()["rebuilding-local-window"];
  ASSERT_EQ("30min", *rebuilding_window_setting.currentValue_ref());

  // Remove the temporary override
  auto remove_setting_override_request = thrift::RemoveSettingOverrideRequest();
  *remove_setting_override_request.name_ref() = "rebuilding-local-window";
  admin_client->sync_removeSettingOverride(remove_setting_override_request);

  // Wait for the setting to get removed
  auto setting_removed = wait_until(
      [&]() {
        thrift::SettingsResponse response2;
        admin_client->sync_getSettings(response2, thrift::SettingsRequest());
        rebuilding_window_setting =
            response2.settings_ref()["rebuilding-local-window"];
        return *rebuilding_window_setting.currentValue_ref() == "60min";
      },
      std::chrono::steady_clock::now() + std::chrono::seconds(30));
  ASSERT_EQ(0, setting_removed);

  // Remove the temporary override again to ensure it's a noop
  auto remove_setting_override_request2 =
      thrift::RemoveSettingOverrideRequest();
  *remove_setting_override_request2.name_ref() = "rebuilding-local-window";
  admin_client->sync_removeSettingOverride(remove_setting_override_request2);
  thrift::SettingsResponse response3;
  admin_client->sync_getSettings(response3, thrift::SettingsRequest());
  rebuilding_window_setting =
      response3.settings_ref()["rebuilding-local-window"];
  ASSERT_EQ("60min", *rebuilding_window_setting.currentValue_ref());
}

TEST_F(AdminAPILowLevelTest, LogGroupThroughputAPITest) {
  /**
   * Using one node to assure querying to a sequencer rather
   * than a data node.
   */
  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .enableLogsConfigManager()
                     .useHashBasedSequencerAssignment()
                     .create(1);

  cluster->waitUntilAllAvailable();
  auto admin_client = cluster->getNode(0).createAdminClient();
  ASSERT_NE(nullptr, admin_client);

  auto client = cluster->createClient();
  client->makeLogGroupSync("/log1",
                           logid_range_t(logid_t(1), logid_t(10)),
                           client::LogAttributes().with_replicationFactor(1),
                           false);

  client->makeLogGroupSync("/log2",
                           logid_range_t(logid_t(20), logid_t(30)),
                           client::LogAttributes().with_replicationFactor(1),
                           false);

  auto fbStatus = admin_client->sync_getStatus();
  ASSERT_EQ(facebook::fb303::cpp2::fb_status::ALIVE, fbStatus);

  // Start writers
  using namespace facebook::logdevice::IntegrationTestUtils;
  auto append_thread_lg1 = std::make_unique<AppendThread>(client, logid_t(1));
  auto append_thread_lg2 = std::make_unique<AppendThread>(client, logid_t(20));
  append_thread_lg1->start();
  append_thread_lg2->start();

  // Start readers
  std::unique_ptr<ClientSettings> client_settings(ClientSettings::create());
  auto reader_lg1 = std::make_unique<ReaderThread>(
      cluster->createClient(testTimeout(), std::move(client_settings)),
      logid_t(1));
  auto reader_lg2 = std::make_unique<ReaderThread>(
      cluster->createClient(testTimeout(), std::move(client_settings)),
      logid_t(20));
  reader_lg1->start();
  reader_lg2->start();

  wait_until([&]() {
    return reader_lg1->getNumRecordsRead() > 100 &&
        reader_lg2->getNumRecordsRead() > 100;
  });

  ld_info("Syncing reader to tail...");
  reader_lg1->syncToTail();
  reader_lg2->syncToTail();

  // Get throughput for log appends
  thrift::LogGroupThroughputResponse response;
  thrift::LogGroupThroughputRequest request;
  request.set_operation(thrift::LogGroupOperation::APPENDS);
  request.set_time_period({60});
  admin_client->sync_getLogGroupThroughput(response, request);
  ASSERT_TRUE(!response.get_throughput().empty());
  std::set<std::string> log_groups{
      "/log1", "/log2", "/config_log_deltas", "/config_log_snapshots"};
  for (const auto& it : response.get_throughput()) {
    ASSERT_TRUE((bool)log_groups.count(it.first));
    ASSERT_EQ(thrift::LogGroupOperation::APPENDS, it.second.get_operation());
    ASSERT_TRUE(it.second.get_results()[0] > 0);
  }

  // Test log_group_name filtering
  request.set_log_group_name("/log2");
  admin_client->sync_getLogGroupThroughput(response, request);
  ASSERT_TRUE(!response.get_throughput().empty());
  for (const auto& it : response.get_throughput()) {
    ASSERT_EQ("/log2", it.first);
    ASSERT_EQ(thrift::LogGroupOperation::APPENDS, it.second.get_operation());
    ASSERT_TRUE(it.second.get_results()[0] > 0);
  }

  // Get throughput for log reads. Test interval request and filtering.
  request.set_operation(thrift::LogGroupOperation::READS);
  request.set_log_group_name("/log1");
  request.set_time_period({60, 300});
  admin_client->sync_getLogGroupThroughput(response, request);
  ASSERT_TRUE(!response.get_throughput().empty());
  for (const auto& it : response.get_throughput()) {
    ASSERT_EQ("/log1", it.first);
    ASSERT_EQ(thrift::LogGroupOperation::READS, it.second.get_operation());
    for (const auto& result : it.second.get_results()) {
      ASSERT_TRUE(result > 0);
    }
  }

  // Get throughput for log appends_out. Test interval request and filtering.
  request.set_operation(thrift::LogGroupOperation::APPENDS_OUT);
  request.set_log_group_name("/log1");
  request.set_time_period({60, 300});
  admin_client->sync_getLogGroupThroughput(response, request);
  ASSERT_TRUE(!response.get_throughput().empty());
  for (const auto& it : response.get_throughput()) {
    ASSERT_EQ("/log1", it.first);
    ASSERT_EQ(
        thrift::LogGroupOperation::APPENDS_OUT, it.second.get_operation());
    for (const auto& result : it.second.get_results()) {
      ASSERT_TRUE(result > 0);
    }
  }
}

TEST_F(AdminAPILowLevelTest, LogGroupCustomCountersAPITest) {
  /**
   * Using one node to assure querying to a sequencer rather
   * than a data node.
   */
  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .enableLogsConfigManager()
                     .useHashBasedSequencerAssignment()
                     .create(1);

  cluster->waitUntilAllAvailable();
  auto admin_client = cluster->getNode(0).createAdminClient();
  ASSERT_NE(nullptr, admin_client);

  auto client = cluster->createClient();
  client->makeLogGroupSync("/log1",
                           logid_range_t(logid_t(1), logid_t(10)),
                           client::LogAttributes().with_replicationFactor(1),
                           false);
  client->makeLogGroupSync("/log2",
                           logid_range_t(logid_t(21), logid_t(30)),
                           client::LogAttributes().with_replicationFactor(1),
                           false);

  // Start writers
  using namespace facebook::logdevice::IntegrationTestUtils;
  auto append_thread_lg1 = std::make_unique<AppendThread>(client, logid_t(1));
  auto append_thread_lg2 = std::make_unique<AppendThread>(client, logid_t(21));

  AppendAttributes attr = AppendAttributes();
  attr.counters = std::map<uint8_t, int64_t>();
  attr.counters->insert(std::pair<uint8_t, int64_t>(0, 1));
  attr.counters->insert(std::pair<uint8_t, int64_t>(1, 1));
  append_thread_lg1->setAppendAttributes(attr);

  AppendAttributes attr2 = AppendAttributes();
  attr2.counters = std::map<uint8_t, int64_t>();
  attr2.counters->insert(std::pair<uint8_t, int64_t>(5, 1));
  attr2.counters->insert(std::pair<uint8_t, int64_t>(6, 1));
  append_thread_lg2->setAppendAttributes(attr2);

  append_thread_lg1->start();
  append_thread_lg2->start();

  wait_until([&]() {
    return append_thread_lg1->getNumRecordsAppended() > 100 &&
        append_thread_lg2->getNumRecordsAppended() > 100;
  });

  append_thread_lg1->stop();
  append_thread_lg2->stop();

  thrift::LogGroupCustomCountersRequest request;
  thrift::LogGroupCustomCountersResponse response;

  request.set_time_period(300);
  admin_client->sync_getLogGroupCustomCounters(response, request);

  auto counters = response.get_counters();
  ASSERT_TRUE(!counters.empty());

  std::set<std::string> log_groups{"/log1", "/log2", "/config_log_deltas"};
  for (const auto& it : counters) {
    ASSERT_TRUE((bool)log_groups.count(it.first));
  }
  auto keys = counters["/log1"];

  ASSERT_EQ(*keys[0].key_ref(), 0);
  ASSERT_EQ(*keys[1].key_ref(), 1);

  ASSERT_TRUE(*keys[0].val_ref() > 0);
  ASSERT_TRUE(*keys[1].val_ref() > 0);

  thrift::LogGroupCustomCountersRequest log2Request;
  thrift::LogGroupCustomCountersResponse log2Response;

  log2Request.set_log_group_path("/log2");
  admin_client->sync_getLogGroupCustomCounters(log2Response, log2Request);

  auto log2Counters = log2Response.get_counters();
  ASSERT_EQ(log2Counters.size(), 1);

  auto log2Keys = counters["/log2"];
  ASSERT_EQ(*log2Keys[0].key_ref(), 5);
  ASSERT_EQ(*log2Keys[1].key_ref(), 6);

  ASSERT_TRUE(*log2Keys[0].val_ref() > 0);
  ASSERT_TRUE(*log2Keys[1].val_ref() > 0);

  LogGroupCustomCountersRequest keyFilteredReq;
  LogGroupCustomCountersResponse keyFilteredRes;

  keyFilteredReq.set_keys(std::vector<int16_t>{0});
  admin_client->sync_getLogGroupCustomCounters(keyFilteredRes, keyFilteredReq);

  auto keyFilteredCounters = keyFilteredRes.get_counters();
  auto keyFilteredKeys = keyFilteredCounters["/log1"];
  ASSERT_EQ(keyFilteredKeys.size(), 1);
  ASSERT_EQ(*keyFilteredKeys[0].key_ref(), 0);
  ASSERT_TRUE(*keyFilteredKeys[0].val_ref() > 0);
}
