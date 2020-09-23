/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include <chrono>
#include <memory>
#include <thread>

#include <gtest/gtest.h>

#include "logdevice/common/configuration/ConfigParser.h"
#include "logdevice/common/test/NodeSetTestUtil.h"
#include "logdevice/include/Client.h"
#include "logdevice/test/utils/IntegrationTestBase.h"
#include "logdevice/test/utils/IntegrationTestUtils.h"

using namespace facebook::logdevice;

class FailureDomainIntegrationTest : public IntegrationTestBase {};

// create cluster nodes with six nodes in the cluster;
// the cluster spans across three regions, and has a total of 6 nodes (1+2+3);
// used in following failure domain related tests
static Configuration::Nodes createFailureDomainNodes() {
  Configuration::Nodes nodes;
  for (int i = 0; i < 6; ++i) {
    auto& node = nodes[i];

    // store data on all nodes
    node.addStorageRole(/*num_shards*/ 2);

    // node 0 running sequencer
    if (i == 0) {
      node.addSequencerRole();
      ld_check_eq(node.sequencer_attributes->weight, 1);
    }

    std::string domain_string;
    if (i < 1) {
      domain_string = "region0.dc1..."; // node 0 is in region 0
    } else if (i < 3) {
      domain_string = "region1.dc1.cl1.ro1.rk1"; // node 1, 2 is in region 1
    } else {
      domain_string = "region2.dc1.cl1.ro1.rk1"; // node 3-5 is in region 2
    }
    NodeLocation location;
    location.fromDomainString(domain_string);
    node.location = location;
  }

  return nodes;
}

TEST_F(FailureDomainIntegrationTest, TolerateRegionFailure) {
  // the test log has replication: 2, and is doing cross-region replication;
  // test the failure case in which the entire third region is down but the
  // cluster is still able to perform writing and reading.
  const logid_t logid(2);
  Configuration::Nodes nodes = createFailureDomainNodes();
  // Set metadata nodeset
  nodes[0].metadata_node = true;
  nodes[1].metadata_node = true;
  nodes[3].metadata_node = true;

  // metadata logs are replicated cross-region as well
  auto nodes_configuration = NodesConfigurationTestUtil::provisionNodes(
      std::move(nodes), ReplicationProperty{{NodeLocationScope::REGION, 2}});

  auto log_attrs = logsconfig::LogAttributes()
                       .with_replicationFactor(2)
                       .with_extraCopies(0)
                       .with_syncedCopies(0)
                       .with_maxWritesInFlight(2048)
                       // cross-region replication
                       .with_syncReplicationScope(NodeLocationScope::REGION);

  Configuration::MetaDataLogsConfig meta_config =
      ServerConfig::MetaDataLogsConfig();
  meta_config.nodeset_selector_type = NodeSetSelectorType::SELECT_ALL;

  auto cluster =
      IntegrationTestUtils::ClusterFactory()
          .setNodes(nodes_configuration)
          .setLogGroupName("mylog")
          .setLogAttributes(log_attrs)
          .setMetaDataLogsConfig(meta_config)
          .eventLogMode(
              IntegrationTestUtils::ClusterFactory::EventLogMode::NONE)
          .setConfigLogAttributes(log_attrs)
          .setMaintenanceLogAttributes(log_attrs)
          .create(nodes_configuration->clusterSize());

  cluster->waitUntilAllSequencersQuiescent();

  std::map<lsn_t, std::string> lsn_map;
  lsn_t first_lsn = LSN_INVALID;
  const size_t NUM_RECORDS_BATCH = 30;

  std::unique_ptr<ClientSettings> client_settings(ClientSettings::create());
  // using a small gap grace period to speed us reading when nodes are down
  ASSERT_EQ(0, client_settings->set("gap-grace-period", "200ms"));

  std::shared_ptr<Client> client =
      cluster->createClient(testTimeout(), std::move(client_settings));

  // write some records and store them in lsn map
  auto write_records = [&] {
    for (int i = 0; i < NUM_RECORDS_BATCH; ++i) {
      std::string data("data" + std::to_string(i));
      lsn_t lsn = client->appendSync(logid, Payload(data.data(), data.size()));
      EXPECT_NE(LSN_INVALID, lsn);
      if (first_lsn == LSN_INVALID) {
        first_lsn = lsn;
      }

      EXPECT_EQ(lsn_map.end(), lsn_map.find(lsn));
      lsn_map[lsn] = data;
    };
  };
  write_records();

  // all nodes in region 2 become not available
  cluster->getNode(3).suspend();
  cluster->getNode(4).suspend();
  cluster->getNode(5).suspend();

  // Restart the sequencer to trigger recovery
  cluster->getNode(0).kill();
  cluster->getNode(0).start();
  cluster->getNode(0).waitUntilStarted();

  // write some more records in the new epoch, writes should succeed
  // since there are still two regions available
  write_records();

  // recovery should finish despite one region (3 nodes) is down
  cluster->waitUntilAllSequencersQuiescent();

  std::vector<std::unique_ptr<DataRecord>> records;
  GapRecord gap;
  const size_t num_records = NUM_RECORDS_BATCH * 2;

  std::unique_ptr<Reader> reader(client->createReader(2));
  reader->setTimeout(std::chrono::seconds(2));
  reader->startReading(logid, first_lsn);

  int nread = 0, total_read = 0;
  do {
    nread = reader->read(num_records, &records, &gap);
    if (nread < 0) {
      ASSERT_EQ(E::GAP, err);
    } else {
      total_read += nread;
    }
  } while (total_read < num_records);
  // reader should be able to read all records
  ASSERT_EQ(num_records, records.size());
  // verify the data read
  auto it = lsn_map.cbegin();
  for (int i = 0; i < num_records; ++i, ++it) {
    const DataRecord& r = *records[i];
    EXPECT_EQ(logid, r.logid);
    EXPECT_NE(lsn_map.cend(), it);
    EXPECT_EQ(it->first, r.attrs.lsn);
    const Payload& p = r.payload;
    EXPECT_NE(nullptr, p.data());
    EXPECT_EQ(it->second.size(), p.size());
    EXPECT_EQ(it->second, p.toString());
  }

  int rv = reader->stopReading(logid);
  EXPECT_EQ(0, rv);
}

// Test ReadHealth with failure domain support end-to-end.
// If records are cross-region replicated, the state of reading should still
// be considered healthy even if all nodes from one single region are down.
TEST_F(FailureDomainIntegrationTest, ReadHealthWithFailureDomain) {
  const logid_t LOG_ID(1);
  Configuration::Nodes nodes = createFailureDomainNodes();
  // Set metadata nodeset
  nodes[0].metadata_node = true;
  nodes[1].metadata_node = true;
  nodes[3].metadata_node = true;

  auto nodes_configuration = NodesConfigurationTestUtil::provisionNodes(
      std::move(nodes), ReplicationProperty{{NodeLocationScope::REGION, 2}});

  auto log_attrs = logsconfig::LogAttributes()
                       .with_replicationFactor(2)
                       .with_extraCopies(0)
                       .with_syncedCopies(0)
                       .with_maxWritesInFlight(256)
                       // cross-region replication
                       .with_syncReplicationScope(NodeLocationScope::REGION);

  Configuration::MetaDataLogsConfig meta_config =
      ServerConfig::MetaDataLogsConfig();
  meta_config.nodeset_selector_type = NodeSetSelectorType::SELECT_ALL;

  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .setNodes(nodes_configuration)
                     .setLogGroupName("mylog")
                     .setLogAttributes(log_attrs)
                     .setMetaDataLogsConfig(meta_config)
                     .create(nodes_configuration->clusterSize());

  std::shared_ptr<Client> client = cluster->createClient();
  std::shared_ptr<const Configuration> config = cluster->getConfig()->get();
  std::unique_ptr<Reader> reader = client->createReader(1);
  std::unique_ptr<AsyncReader> async_reader = client->createAsyncReader();

  // Call should return -1 if we are not reading
  ASSERT_EQ(-1, reader->isConnectionHealthy(LOG_ID));
  ASSERT_EQ(-1, async_reader->isConnectionHealthy(LOG_ID));

  reader->startReading(LOG_ID, LSN_OLDEST);
  async_reader->setRecordCallback(
      [](std::unique_ptr<DataRecord>&) { return true; });
  async_reader->startReading(LOG_ID, LSN_OLDEST);

  ld_info("Waiting for readers to connect and reach healthy state");
  wait_until([&]() { return reader->isConnectionHealthy(LOG_ID) == 1; });
  wait_until([&]() { return async_reader->isConnectionHealthy(LOG_ID) == 1; });

  ld_info("Killing the third region (node 3, 4, 5), should not affect "
          "connection health");
  for (int i = 3; i <= 5; ++i) {
    ld_check(config->getNodesConfiguration()
                 ->getStorageMembership()
                 ->hasShardShouldReadFrom(i));
    cluster->getNode(i).kill();
  }

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  wait_until([&]() { return reader->isConnectionHealthy(LOG_ID) == 1; });
  wait_until([&]() { return async_reader->isConnectionHealthy(LOG_ID) == 1; });

  // resume node 3, 4 but kill node 0, connection should be unhealthy even if
  // the number of healthy nodes increased
  ld_info("resume node 3 and 4 but kill node 0, should negatively "
          "affect connection health since they are two failed nodes from "
          "different region");
  cluster->getNode(3).start();
  cluster->getNode(4).start();
  cluster->getNode(3).waitUntilStarted();
  cluster->getNode(4).waitUntilStarted();
  ld_check(config->getNodesConfiguration()
               ->getStorageMembership()
               ->hasShardShouldReadFrom(0));
  cluster->getNode(0).kill();

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  wait_until([&]() { return reader->isConnectionHealthy(LOG_ID) == 0; });
  wait_until([&]() { return async_reader->isConnectionHealthy(LOG_ID) == 0; });
}

TEST_F(FailureDomainIntegrationTest,
       TolerateRegionFailureWithHashBasedSequencerAssignment) {
  dbg::parseLoglevelOption("debug");
  // the test log has replication: 2, and is doing cross-region replication;
  // test the failure case in which the entire third region is down but the
  // cluster is still able to perform writing and reading.
  const logid_t logid(2);
  Configuration::Nodes nodes = createFailureDomainNodes();
  for (auto& it : nodes) {
    auto& node = it.second;
    node.addSequencerRole();
  }
  // Set metadata nodeset
  nodes[0].metadata_node = true;
  nodes[1].metadata_node = true;
  nodes[3].metadata_node = true;

  // metadata logs are replicated cross-region as well
  // this nodeset, with replication = 2, enforces cross-region replication
  auto nodes_configuration = NodesConfigurationTestUtil::provisionNodes(
      std::move(nodes), ReplicationProperty{{NodeLocationScope::REGION, 2}});

  auto log_attrs = logsconfig::LogAttributes()
                       .with_replicationFactor(2)
                       .with_extraCopies(0)
                       .with_syncedCopies(0)
                       .with_maxWritesInFlight(2048)
                       // cross-region replication
                       .with_syncReplicationScope(NodeLocationScope::REGION)
                       .with_nodeSetSize(3); // 3 regions

  auto cluster =
      IntegrationTestUtils::ClusterFactory()
          .setNodes(nodes_configuration)
          .setLogGroupName("mylog")
          .setLogAttributes(log_attrs)
          // TODO(#8466255): remove.
          .eventLogMode(
              IntegrationTestUtils::ClusterFactory::EventLogMode::NONE)
          .useHashBasedSequencerAssignment(100, "10s")
          .enableMessageErrorInjection()
          .create(nodes_configuration->clusterSize());

  cluster->waitUntilAllStartedAndPropagatedInGossip();

  std::map<lsn_t, std::string> lsn_map;
  lsn_t first_lsn = LSN_INVALID;
  const size_t NUM_RECORDS_BATCH = 30;

  std::unique_ptr<ClientSettings> client_settings(ClientSettings::create());
  // using a small gap grace period to speed up reading when nodes are down
  ASSERT_EQ(0, client_settings->set("gap-grace-period", "200ms"));
  ASSERT_EQ(0, client_settings->set("cluster-state-refresh-interval", "100ms"));

  std::shared_ptr<Client> client =
      cluster->createClient(testTimeout(), std::move(client_settings));

  // write some records and store them in lsn map
  auto write_records = [&](bool expect_error) {
    bool error_encountered = false;
    for (int i = 0; i < NUM_RECORDS_BATCH; ++i) {
      std::string data("data" + std::to_string(i));
      lsn_t lsn = client->appendSync(logid, Payload(data.data(), data.size()));
      if (expect_error && !error_encountered) {
        if (lsn == LSN_INVALID) {
          error_encountered = true;
          // retry once
          lsn = client->appendSync(logid, Payload(data.data(), data.size()));
        }
      }

      EXPECT_NE(LSN_INVALID, lsn);
      if (first_lsn == LSN_INVALID) {
        first_lsn = lsn;
      }

      EXPECT_EQ(lsn_map.end(), lsn_map.find(lsn));
      lsn_map[lsn] = data;
    };
  };
  write_records(false);

  // all nodes in region 2 become not available
  cluster->getNode(3).suspend();
  cluster->getNode(4).suspend();
  cluster->getNode(5).suspend();

  cluster->waitUntilGossip(/* alive */ false, 3);
  cluster->waitUntilGossip(/* alive */ false, 4);
  cluster->waitUntilGossip(/* alive */ false, 5);

  // write some more records in the new epoch, writes should succeed
  // since there are still two regions available
  write_records(true);

  // recovery should finish despite one region (3 nodes) is down
  cluster->waitUntilAllSequencersQuiescent();

  std::vector<std::unique_ptr<DataRecord>> records;
  GapRecord gap;
  const size_t num_records = NUM_RECORDS_BATCH * 2;

  std::unique_ptr<Reader> reader(client->createReader(2));
  reader->setTimeout(std::chrono::seconds(2));
  reader->startReading(logid, first_lsn);

  int nread = 0, total_read = 0;
  do {
    nread = reader->read(num_records, &records, &gap);
    if (nread < 0) {
      ASSERT_EQ(E::GAP, err);
    } else {
      total_read += nread;
    }
  } while (total_read < num_records);
  // reader should be able to read all records
  ASSERT_EQ(num_records, records.size());
  // verify the data read
  auto it = lsn_map.cbegin();
  for (int i = 0; i < num_records; ++i, ++it) {
    const DataRecord& r = *records[i];
    EXPECT_EQ(logid, r.logid);
    EXPECT_NE(lsn_map.cend(), it);
    EXPECT_EQ(it->first, r.attrs.lsn);
    const Payload& p = r.payload;
    EXPECT_NE(nullptr, p.data());
    EXPECT_EQ(it->second.size(), p.size());
    EXPECT_EQ(it->second, p.toString());
  }

  int rv = reader->stopReading(logid);
  EXPECT_EQ(0, rv);
}

// Cluster of 4 racks, replicate to 3. Check that when 2 racks are down there's
// read availability and no write availability.
TEST_F(FailureDomainIntegrationTest, ThreeRackReplication) {
  // 4 racks, with different number of nodes.
  auto nodes = std::make_shared<const NodesConfiguration>();
  std::vector<node_index_t> rack_start = {0};
  NodeSetTestUtil::addNodes(nodes, 4, 1, "region.dc.cl.ro.rk1"); // 0,1,2,3
  rack_start.push_back(nodes->clusterSize());
  NodeSetTestUtil::addNodes(nodes, 3, 1, "region.dc.cl.ro.rk2"); // 4, 5, 6
  rack_start.push_back(nodes->clusterSize());
  NodeSetTestUtil::addNodes(nodes, 2, 1, "region.dc.cl.ro.rk3"); // 7, 8
  rack_start.push_back(nodes->clusterSize());
  NodeSetTestUtil::addNodes(nodes, 2, 1, "region.dc.cl.ro.rk4"); // 9, 10
  rack_start.push_back(nodes->clusterSize());

  // Metadata nodeset is whole cluster.
  nodes =
      nodes->applyUpdate(NodesConfigurationTestUtil::setStorageMembershipUpdate(
          *nodes,
          nodes->getStorageMembership()->getAllShards(),
          folly::none,
          membership::MetaDataStorageState::METADATA));

  // Replicate metadata logs to all 4 racks.
  nodes = nodes->applyUpdate(
      NodesConfigurationTestUtil::setMetadataReplicationPropertyUpdate(
          *nodes, ReplicationProperty{{NodeLocationScope::RACK, 4}}));

  configuration::MetaDataLogsConfig meta_logs_config;
  meta_logs_config.sequencers_write_metadata_logs = true;
  meta_logs_config.sequencers_provision_epoch_store = true;

  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .setNodes(std::move(nodes))
                     .setLogGroupName("test_logs")
                     .setLogAttributes(logsconfig::LogAttributes()
                                           .with_replicateAcross(
                                               {{NodeLocationScope::RACK, 3}})
                                           .with_nodeSetSize(8)
                                           .with_scdEnabled(true))
                     .setMetaDataLogsConfig(meta_logs_config)
                     .setEventLogAttributes(
                         logsconfig::LogAttributes().with_replicateAcross(
                             {{NodeLocationScope::RACK, 4}}))
                     .useHashBasedSequencerAssignment()
                     .setParam("--enable-sticky-copysets", "false")
                     .setParam("--rocksdb-use-copyset-index", "false")
                     .create(0);

  cluster->waitUntilAllStartedAndPropagatedInGossip();

  const std::chrono::seconds client_timeout(3);
  auto client = cluster->createClient(client_timeout);
  const logid_t logid(2);
  std::vector<lsn_t> lsns;

  // Write some records.
  auto write_records = [&](int n, bool expect_success) {
    for (int i = 0; i < n; ++i) {
      lsn_t lsn;
      auto append_start_time = std::chrono::steady_clock::now();
      do {
        lsn = client->appendSync(logid, std::to_string(lsns.size()));

        if (!expect_success) {
          EXPECT_EQ(LSN_INVALID, lsn);
          return;
        }

        if (lsn == LSN_INVALID) {
          // It's possible for append to fail if there's an appender that
          // started while a rack was down and hasn't yet noticed that the
          // rack is up again. Keep trying.
          EXPECT_TRUE(err == E::SEQNOBUFS || err == E::ISOLATED ||
                      err == E::TIMEDOUT)
              << error_name(err);

          if (std::chrono::steady_clock::now() - append_start_time >
              std::chrono::seconds(10)) {
            // This is not supposed to take so long.
            ADD_FAILURE() << "Can't append " << error_name(err);
            return;
          }

          /* sleep override */
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      } while (lsn == LSN_INVALID);

      lsns.push_back(lsn);
    }
  };

  ld_info("Writing with all nodes up.");
  write_records(10, true);

  // Make sure some records have copyset entirely in 2 racks + 1 node which
  // we're going to stop later in the test.
  ld_info("Writing with a forced copyset.");
  cluster->updateSetting("test-do-not-pick-in-copysets", "0,1,2,3,10");
  write_records(5, true);

  ld_info("Writing some more.");
  cluster->unsetSetting("test-do-not-pick-in-copysets");
  write_records(5, true);

  EXPECT_EQ(20, lsns.size());

  // Read them back.
  auto read_records = [&](const std::vector<lsn_t>& lsns,
                          int restarted_at = -1,
                          std::chrono::milliseconds timeout =
                              std::chrono::seconds(20)) {
    auto reader = client->createReader(1);
    reader->setTimeout(timeout);
    reader->startReading(logid, LSN_OLDEST, lsns.back());
    size_t nread = 0;
    size_t expected_payload = 0;
    while (reader->isReadingAny()) {
      std::vector<std::unique_ptr<DataRecord>> recs;
      GapRecord gap;
      auto n = reader->read(1, &recs, &gap);
      if (n == -1) {
        EXPECT_EQ(GapType::BRIDGE, gap.type);
      } else if (n == 0) {
        return false;
      } else {
        if (nread >= lsns.size()) {
          ADD_FAILURE() << nread << ' ' << lsns.size();
        } else if ((int)nread == restarted_at &&
                   recs[0]->attrs.lsn < lsns[nread]) {
          // An annoying subtlety: when we expect a write to time out and it
          // times out, the Appender keeps trying forever. After we bring some
          // nodes back, the Appender succeeds, producing an unexpected record.
          // This branch skips such records.
        } else {
          EXPECT_EQ(lsns[nread], recs[0]->attrs.lsn);
          EXPECT_EQ(
              std::to_string(expected_payload), recs[0]->payload.toString());
          ++nread;
          ++expected_payload;
        }
      }

      // Expect connection to be reported as healthy most of the time.
      // There's a known issue with a short period of reported unhealthiness
      // near epoch bumps; work around that by skipping the check for the first
      // half of the records.
      if (nread > lsns.size() / 2 && nread < lsns.size()) {
        EXPECT_EQ(1, reader->isConnectionHealthy(logid));
      }
    }
    EXPECT_EQ(-1, reader->isConnectionHealthy(logid));
    EXPECT_EQ(E::NOTFOUND, err); // not reading the log anymore
    return true;
  };
  ld_info("Reading with all nodes up.");
  EXPECT_TRUE(read_records(lsns));

  // Stop 2/4 racks.
  ld_info("Shutting down racks 2 and 3.");
  {
    std::vector<node_index_t> nodes(rack_start[3] - rack_start[1]);
    std::iota(nodes.begin(), nodes.end(), rack_start[1]);
    EXPECT_EQ(0, cluster->shutdownNodes(nodes));
  }

  // Read the records again.
  ld_info("Reading without 2/4 racks.");
  EXPECT_TRUE(read_records(lsns));
  int records_before_failed_writes = static_cast<int>(lsns.size());

  // Try writing something. It should fail.
  ld_info("Writing without 2/4 racks (should fail).");
  write_records(50, false);

  // Stop one more node. Note that the rack has 2 nodes and both are in nodeset.
  ld_info("Killing another node.");
  cluster->getNode(rack_start[3]).kill();

  // Reads should fail now.
  ld_info("Reading without 2/4 racks and one node (should fail).");
  EXPECT_FALSE(read_records(lsns, -1, std::chrono::seconds(10)));

  // Bring one rack back.
  ld_info("Starting rack 2.");
  {
    std::vector<node_index_t> nodes(rack_start[2] - rack_start[1]);
    std::iota(nodes.begin(), nodes.end(), rack_start[1]);
    EXPECT_EQ(0, cluster->start(nodes));
  }
  cluster->waitUntilAllStartedAndPropagatedInGossip();

  // Writes and reads should work now.
  ld_info("Writing without a rack and a node.");
  write_records(20, true);
  EXPECT_EQ(40, lsns.size());
  ld_info("Reading without a rack and a node.");
  EXPECT_TRUE(read_records(lsns, records_before_failed_writes));
}
