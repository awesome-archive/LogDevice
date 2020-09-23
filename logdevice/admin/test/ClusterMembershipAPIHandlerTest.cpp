/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logdevice/admin/ClusterMembershipAPIHandler.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "logdevice/admin/AdminAPIUtils.h"
#include "logdevice/admin/if/gen-cpp2/AdminAPI.h"
#include "logdevice/admin/if/gen-cpp2/cluster_membership_constants.h"
#include "logdevice/common/Processor.h"
#include "logdevice/common/configuration/nodes/NodesConfiguration.h"
#include "logdevice/common/configuration/nodes/NodesConfigurationManagerFactory.h"
#include "logdevice/common/membership/gen-cpp2/Membership_types.h"
#include "logdevice/common/test/TestUtil.h"
#include "logdevice/test/utils/AdminAPITestUtils.h"
#include "logdevice/test/utils/IntegrationTestBase.h"
#include "logdevice/test/utils/IntegrationTestUtils.h"
#include "logdevice/test/utils/SelfRegisteredCluster.h"

using namespace ::testing;
using namespace apache::thrift;
using namespace facebook::logdevice;

using ClientNetworkPriority = thrift::ClientNetworkPriority;

class ClusterMemebershipAPIIntegrationTest : public IntegrationTestBase {
 protected:
  void SetUp() override {
    IntegrationTestBase::SetUp();

    cluster_ =
        IntegrationTestUtils::ClusterFactory()
            .useHashBasedSequencerAssignment()
            .enableSelfInitiatedRebuilding("10s")
            .setNodesConfigurationSourceOfTruth(
                IntegrationTestUtils::NodesConfigurationSourceOfTruth::NCM)
            .setParam("--event-log-grace-period", "1ms")
            .setParam("--gossip-enabled", "true")
            .setParam("--gossip-interval", "50ms")
            .setParam("--disable-event-log-trimming", "true")
            .setParam("--enable-nodes-configuration-manager",
                      "true",
                      IntegrationTestUtils::ParamScope::ALL)
            .setParam("--nodes-configuration-manager-store-polling-interval",
                      "1s",
                      IntegrationTestUtils::ParamScope::ALL)
            .setParam("--nodes-configuration-manager-intermediary-shard-state-"
                      "timeout",
                      "2s")
            .useStandaloneAdminServer(true)
            .setMetaDataLogsConfig(createMetaDataLogsConfig({2, 3}, 2))
            .deferStart()
            .create(4);
  }

  thrift::RemoveNodesRequest
  buildRemoveNodesRequest(std::vector<int32_t> idxs) {
    thrift::RemoveNodesRequest req;
    for (auto idx : idxs) {
      thrift::NodesFilter filter;
      filter.set_node(mkNodeID(node_index_t(idx)));
      req.node_filters_ref()->push_back(std::move(filter));
    }
    return req;
  }

  thrift::AddNodesRequest buildAddNodesRequest(std::vector<int32_t> idxs) {
    ld_assert(idxs.size() < 25);
    for (int32_t idx : idxs) {
      ld_assert(idx < 150);
    }

    auto make_address = [](int addr, int port) {
      thrift::SocketAddress ret;
      ret.set_address(folly::sformat("127.0.0.{}", addr));
      ret.set_port(port);
      return ret;
    };

    thrift::AddNodesRequest req;
    for (int32_t idx : idxs) {
      thrift::NodeConfig cfg;

      cfg.set_node_index(idx);
      cfg.set_name(folly::sformat("server-{}", idx));
      cfg.set_data_address(make_address(0 + idx, 1000 + idx));

      {
        thrift::Addresses other_addresses;
        other_addresses.set_gossip(make_address(25 + idx, 2000 + idx));
        other_addresses.set_ssl(make_address(50 + idx, 3000 + idx));
        other_addresses.set_admin(make_address(75 + idx, 4000 + idx));
        other_addresses.set_server_to_server(
            make_address(100 + idx, 5000 + idx));
        other_addresses.set_server_thrift_api(
            make_address(125 + idx, 6000 + idx));
        other_addresses.set_client_thrift_api(
            make_address(150 + idx, 7000 + idx));
        other_addresses.set_addresses_per_priority(
            {{ClientNetworkPriority::LOW, make_address(0 + idx, 8000 + idx)},
             {ClientNetworkPriority::MEDIUM, make_address(0 + idx, 9000 + idx)},
             {ClientNetworkPriority::HIGH,
              make_address(0 + idx, 10000 + idx)}});
        cfg.set_other_addresses(other_addresses);
      }

      cfg.set_roles({thrift::Role::SEQUENCER, thrift::Role::STORAGE});
      cfg.set_tags({{"tag1", "value1"}, {"tag2", "value2"}});

      {
        thrift::SequencerConfig seq_cfg;
        seq_cfg.set_weight(idx);
        cfg.set_sequencer(seq_cfg);
      }

      {
        thrift::StorageConfig storage_cfg;
        storage_cfg.set_weight(idx);
        storage_cfg.set_num_shards(2);
        cfg.set_storage(storage_cfg);
      }

      cfg.set_location(folly::sformat("PRN.PRN.PRN.PRN.{}", idx));

      thrift::AddSingleNodeRequest single;
      single.set_new_config(std::move(cfg));
      req.new_node_requests_ref()->push_back(std::move(single));
    }
    return req;
  }

  bool disableAndWait(std::vector<thrift::ShardID> shards,
                      std::vector<thrift::NodeID> sequencers) {
    cluster_->getAdminServer()->waitUntilFullyLoaded();
    auto admin_client = cluster_->getAdminServer()->createAdminClient();

    thrift::MaintenanceDefinition request;
    request.set_user("bunny");
    request.set_shard_target_state(thrift::ShardOperationalState::DRAINED);
    request.set_sequencer_nodes(sequencers);
    request.set_sequencer_target_state(thrift::SequencingState::DISABLED);
    request.set_shards(shards);
    request.set_skip_safety_checks(true);
    thrift::MaintenanceDefinitionResponse resp;
    admin_client->sync_applyMaintenance(resp, request);
    return wait_until("Maintenance manager disables the node", [&]() {
             thrift::MaintenancesFilter filter;
             filter.set_user("bunny");
             admin_client->sync_getMaintenances(resp, filter);
             return std::all_of(resp.get_maintenances().begin(),
                                resp.get_maintenances().end(),
                                [](const auto& m) {
                                  return *m.progress_ref() ==
                                      thrift::MaintenanceProgress::COMPLETED;
                                });
           }) == 0;
  }

  bool waitUntilMaintenanceManagerHasNCVersion(
      std::unique_ptr<thrift::AdminAPIAsyncClient>& admin_client,
      uint64_t version) {
    return wait_until(
               folly::sformat(
                   "Maintenance Managers's NC has version >= {}", version)
                   .c_str(),
               [&]() {
                 thrift::NodesStateRequest state_req;
                 state_req.set_filter(thrift::NodesFilter{});

                 thrift::NodesStateResponse nc;
                 admin_client->sync_getNodesState(nc, state_req);
                 return *nc.version_ref() >= version;
               }) == 0;
  }

 public:
  std::unique_ptr<IntegrationTestUtils::Cluster> cluster_;
};

TEST_F(ClusterMemebershipAPIIntegrationTest, TestRemoveAliveNodes) {
  cluster_->updateNodeAttributes(
      node_index_t(1), configuration::StorageState::DISABLED, 1, false);
  ASSERT_EQ(0, cluster_->start({0, 1, 2, 3}));
  cluster_->getAdminServer()->waitUntilFullyLoaded();
  auto admin_client = cluster_->getAdminServer()->createAdminClient();
  disableAndWait({mkShardID(1, -1)}, {mkNodeID(1)});

  try {
    thrift::RemoveNodesResponse resp;
    admin_client->sync_removeNodes(resp, buildRemoveNodesRequest({1}));
    FAIL() << "RemoveNodes call should fail, but it didn't";
  } catch (const thrift::ClusterMembershipOperationFailed& exception) {
    ASSERT_EQ(1, exception.failed_nodes_ref()->size());
    auto failed_node = exception.failed_nodes_ref()[0];
    EXPECT_EQ(1, failed_node.node_id_ref()->node_index_ref().value_unchecked());
    EXPECT_EQ(thrift::ClusterMembershipFailureReason::NOT_DEAD,
              *failed_node.reason_ref());
  }
}

TEST_F(ClusterMemebershipAPIIntegrationTest, TestRemoveProvisioningNodes) {
  ASSERT_EQ(0, cluster_->start({0, 1, 2, 3}));
  auto admin_client = cluster_->getAdminServer()->createAdminClient();

  {
    // Add two nodes with 2 shards each. They will get added as PROVISIONING.
    thrift::AddNodesResponse resp;
    admin_client->sync_addNodes(resp, buildAddNodesRequest({100, 101}));
    ASSERT_EQ(2, resp.added_nodes_ref()->size());
    waitUntilMaintenanceManagerHasNCVersion(
        admin_client, *resp.new_nodes_configuration_version_ref());
  }

  ASSERT_TRUE(wait_until_service_state(
      *admin_client, {100, 101}, thrift::ServiceState::DEAD));
  thrift::RemoveNodesResponse resp;
  admin_client->sync_removeNodes(resp, buildRemoveNodesRequest({100, 101}));
  EXPECT_EQ(2, resp.removed_nodes_ref()->size());
}

TEST_F(ClusterMemebershipAPIIntegrationTest, TestApplyDrainOnProvisioning) {
  ASSERT_EQ(0, cluster_->start({0, 1, 2, 3}));
  cluster_->getAdminServer()->waitUntilFullyLoaded();
  auto admin_client = cluster_->getAdminServer()->createAdminClient();

  {
    // Add two nodes with 2 shards each. They will get added as PROVISIONING.
    thrift::AddNodesResponse resp;
    admin_client->sync_addNodes(resp, buildAddNodesRequest({100, 101}));
    ASSERT_EQ(2, resp.added_nodes_ref()->size());

    waitUntilMaintenanceManagerHasNCVersion(
        admin_client, *resp.new_nodes_configuration_version_ref());
  }

  // We didn't provision the shared, let's apply a DRAINED maintenance
  // immediately.
  disableAndWait(
      {mkShardID(100, -1), mkShardID(101, -1)}, {mkNodeID(100), mkNodeID(101)});
}

TEST_F(ClusterMemebershipAPIIntegrationTest, TestRemoveNonExistentNode) {
  ASSERT_EQ(0, cluster_->start({0, 1, 2, 3}));
  cluster_->getAdminServer()->waitUntilFullyLoaded();
  auto admin_client = cluster_->getAdminServer()->createAdminClient();

  thrift::RemoveNodesResponse resp;
  admin_client->sync_removeNodes(resp, buildRemoveNodesRequest({10}));
  EXPECT_EQ(0, resp.removed_nodes_ref()->size());
}

TEST_F(ClusterMemebershipAPIIntegrationTest, TestRemoveEnabledNodes) {
  ASSERT_EQ(0, cluster_->start({0, 2, 3}));
  cluster_->getAdminServer()->waitUntilFullyLoaded();
  auto admin_client = cluster_->getAdminServer()->createAdminClient();
  ASSERT_TRUE(
      wait_until_service_state(*admin_client, {1}, thrift::ServiceState::DEAD));

  try {
    thrift::RemoveNodesResponse resp;
    admin_client->sync_removeNodes(resp, buildRemoveNodesRequest({1}));
    FAIL() << "RemoveNodes call should fail, but it didn't";
  } catch (const thrift::ClusterMembershipOperationFailed& exception) {
    ASSERT_EQ(1, exception.failed_nodes_ref()->size());
    auto failed_node = exception.failed_nodes_ref()[0];
    EXPECT_EQ(1, failed_node.node_id_ref()->node_index_ref().value_unchecked());
    EXPECT_EQ(thrift::ClusterMembershipFailureReason::NOT_DISABLED,
              *failed_node.reason_ref());
  }
}

TEST_F(ClusterMemebershipAPIIntegrationTest, TestRemoveNodeSuccess) {
  cluster_->updateNodeAttributes(
      node_index_t(1), configuration::StorageState::DISABLED, 1, false);
  ASSERT_EQ(0, cluster_->start({0, 2, 3}));
  cluster_->getAdminServer()->waitUntilFullyLoaded();
  auto admin_client = cluster_->getAdminServer()->createAdminClient();
  disableAndWait({mkShardID(1, -1)}, {mkNodeID(1)});

  ASSERT_TRUE(
      wait_until_service_state(*admin_client, {1}, thrift::ServiceState::DEAD));
  thrift::RemoveNodesResponse resp;
  admin_client->sync_removeNodes(resp, buildRemoveNodesRequest({1}));
  EXPECT_EQ(1, resp.removed_nodes_ref()->size());
  EXPECT_EQ(1, resp.removed_nodes_ref()[0].node_index_ref().value_unchecked());

  waitUntilMaintenanceManagerHasNCVersion(
      admin_client, *resp.new_nodes_configuration_version_ref());

  thrift::NodesConfigResponse nodes_config;
  admin_client->sync_getNodesConfig(nodes_config, thrift::NodesFilter{});
  EXPECT_EQ(3, nodes_config.nodes_ref()->size());
}

MATCHER_P2(NodeConfigEq, expected_idx, req, "") {
  return expected_idx == *arg.node_index_ref() &&
      *req.name_ref() == *arg.name_ref() &&
      *req.data_address_ref() == *arg.data_address_ref() &&
      req.other_addresses_ref() == arg.other_addresses_ref() &&
      req.location_ref() == arg.location_ref() &&
      *req.roles_ref() == *arg.roles_ref() &&
      *req.tags_ref() == *arg.tags_ref() &&
      req.sequencer_ref() == arg.sequencer_ref() &&
      req.storage_ref() == arg.storage_ref();
};

MATCHER_P2(SequencerStateEq, expected_idx, req, "") {
  return expected_idx == *arg.node_index_ref() && arg.sequencer_state_ref() &&
      arg.sequencer_state_ref().value().get_state() == req;
};

TEST_F(ClusterMemebershipAPIIntegrationTest, TestAddNodeSuccess) {
  ASSERT_EQ(0, cluster_->start({0, 1, 2, 3}));
  cluster_->waitUntilAllAvailable();
  cluster_->getAdminServer()->waitUntilFullyLoaded();
  auto admin_client = cluster_->getAdminServer()->createAdminClient();

  thrift::AddNodesRequest req = buildAddNodesRequest({10, 50});
  // Let the admin server allocate the NodeID for the second node for us
  req.new_node_requests_ref()[1].new_config_ref()->set_node_index(
      thrift::cluster_membership_constants::ANY_NODE_IDX());
  thrift::AddNodesResponse resp;

  admin_client->sync_addNodes(resp, req);
  EXPECT_EQ(2, resp.added_nodes_ref()->size());
  EXPECT_THAT(
      *resp.added_nodes_ref(),
      UnorderedElementsAre(
          NodeConfigEq(10, *req.new_node_requests_ref()[0].new_config_ref()),
          NodeConfigEq(4, *req.new_node_requests_ref()[1].new_config_ref())));

  waitUntilMaintenanceManagerHasNCVersion(
      admin_client, *resp.new_nodes_configuration_version_ref());

  thrift::NodesConfigResponse nodes_config;
  admin_client->sync_getNodesConfig(nodes_config, thrift::NodesFilter{});
  EXPECT_EQ(6, nodes_config.nodes_ref()->size());
  EXPECT_THAT(*nodes_config.nodes_ref(),
              AllOf(Contains(NodeConfigEq(
                        10, *req.new_node_requests_ref()[0].new_config_ref())),
                    Contains(NodeConfigEq(
                        4, *req.new_node_requests_ref()[1].new_config_ref()))));

  thrift::NodesStateResponse nodes_state;
  admin_client->sync_getNodesState(nodes_state, thrift::NodesStateRequest{});
  EXPECT_EQ(6, nodes_state.states_ref()->size());
  EXPECT_THAT(
      *nodes_state.states_ref(),
      AllOf(Contains(SequencerStateEq(10, thrift::SequencingState::DISABLED)),
            Contains(SequencerStateEq(4, thrift::SequencingState::DISABLED))));
}

TEST_F(ClusterMemebershipAPIIntegrationTest, TestAddAlreadyExists) {
  ASSERT_EQ(0, cluster_->start({0, 1, 2, 3}));
  cluster_->getAdminServer()->waitUntilFullyLoaded();
  auto admin_client = cluster_->getAdminServer()->createAdminClient();

  // Get current Admin server version
  thrift::NodesConfigResponse nodes_config;
  admin_client->sync_getNodesConfig(nodes_config, thrift::NodesFilter{});

  thrift::AddNodesRequest req = buildAddNodesRequest({100});
  // Copy the address of an existing node
  *req.new_node_requests_ref()[0].new_config_ref()->data_address_ref() =
      *nodes_config.nodes_ref()[0].data_address_ref();

  try {
    thrift::AddNodesResponse resp;
    admin_client->sync_addNodes(resp, req);
    FAIL() << "AddNodes call should fail, but it didn't";
  } catch (const thrift::ClusterMembershipOperationFailed& exception) {
    ASSERT_EQ(1, exception.failed_nodes_ref()->size());
    auto failed_node = exception.failed_nodes_ref()[0];
    EXPECT_EQ(
        100, failed_node.node_id_ref()->node_index_ref().value_unchecked());
    EXPECT_EQ(thrift::ClusterMembershipFailureReason::ALREADY_EXISTS,
              *failed_node.reason_ref());
  }
}

TEST_F(ClusterMemebershipAPIIntegrationTest, TestInvalidAddNodesRequest) {
  ASSERT_EQ(0, cluster_->start({0, 1, 2, 3}));
  cluster_->getAdminServer()->waitUntilFullyLoaded();
  auto admin_client = cluster_->getAdminServer()->createAdminClient();

  // Get current Admin server version
  thrift::NodesConfigResponse nodes_config;
  admin_client->sync_getNodesConfig(nodes_config, thrift::NodesFilter{});

  thrift::AddNodesRequest req = buildAddNodesRequest({4});
  // Let's reset the storage the storage config
  req.new_node_requests_ref()[0].new_config_ref()->storage_ref().reset();

  try {
    thrift::AddNodesResponse resp;
    admin_client->sync_addNodes(resp, req);
    FAIL() << "AddNodes call should fail, but it didn't";
  } catch (const thrift::ClusterMembershipOperationFailed& exception) {
    ASSERT_EQ(1, exception.failed_nodes_ref()->size());
    auto failed_node = exception.failed_nodes_ref()[0];
    EXPECT_EQ(4, failed_node.node_id_ref()->node_index_ref().value_unchecked());
    EXPECT_EQ(
        thrift::ClusterMembershipFailureReason::INVALID_REQUEST_NODES_CONFIG,
        *failed_node.reason_ref());
  }
}

TEST_F(ClusterMemebershipAPIIntegrationTest, TestUpdateRequest) {
  using Priority = thrift::ClientNetworkPriority;
  ASSERT_EQ(0, cluster_->start({0, 1, 2, 3}));
  cluster_->getAdminServer()->waitUntilFullyLoaded();
  auto admin_client = cluster_->getAdminServer()->createAdminClient();

  thrift::NodesFilter filter;
  filter.set_node(mkNodeID(node_index_t(3)));
  thrift::NodesConfigResponse nodes_config;
  admin_client->sync_getNodesConfig(nodes_config, filter);
  ASSERT_EQ(1, nodes_config.nodes_ref()->size());

  // Update N3
  auto cfg = nodes_config.nodes_ref()[0];
  cfg.set_name("updatedName");
  cfg.data_address_ref()->set_address("/test1");
  cfg.other_addresses_ref()->gossip_ref()->set_address("/test2");
  cfg.other_addresses_ref()->ssl_ref()->set_address("/test3");
  cfg.other_addresses_ref()->server_to_server_ref()->set_address("/test4");
  cfg.other_addresses_ref()->server_thrift_api_ref()->set_address("/test5");
  cfg.other_addresses_ref()->client_thrift_api_ref()->set_address("/test6");
  (*cfg.other_addresses_ref()->addresses_per_priority_ref())[Priority::MEDIUM]
      .set_address("/test7");

  cfg.storage_ref()->set_weight(123);
  cfg.sequencer_ref()->set_weight(122);

  thrift::UpdateSingleNodeRequest updt;
  updt.set_node_to_be_updated(mkNodeID(3));
  updt.set_new_config(cfg);
  thrift::UpdateNodesRequest req;
  req.set_node_requests({std::move(updt)});

  thrift::UpdateNodesResponse uresp;
  admin_client->sync_updateNodes(uresp, req);
  EXPECT_EQ(1, uresp.updated_nodes_ref()->size());
  EXPECT_THAT(
      *uresp.updated_nodes_ref(), UnorderedElementsAre(NodeConfigEq(3, cfg)));

  waitUntilMaintenanceManagerHasNCVersion(
      admin_client, *uresp.new_nodes_configuration_version_ref());

  admin_client->sync_getNodesConfig(nodes_config, filter);
  ASSERT_EQ(1, nodes_config.nodes_ref()->size());
  ASSERT_THAT(nodes_config.nodes_ref()[0], NodeConfigEq(3, cfg));
}

TEST_F(ClusterMemebershipAPIIntegrationTest, TestUpdateFailure) {
  ASSERT_EQ(0, cluster_->start({0, 1, 2, 3}));
  cluster_->getAdminServer()->waitUntilFullyLoaded();
  auto admin_client = cluster_->getAdminServer()->createAdminClient();

  thrift::NodesFilter filter;
  filter.set_node(mkNodeID(node_index_t(3)));
  thrift::NodesConfigResponse nodes_config;
  admin_client->sync_getNodesConfig(nodes_config, filter);
  ASSERT_EQ(1, nodes_config.nodes_ref()->size());

  auto cfg = nodes_config.nodes_ref()[0];
  thrift::UpdateSingleNodeRequest updt;
  updt.set_node_to_be_updated(mkNodeID(3));
  updt.set_new_config(cfg);
  thrift::UpdateNodesRequest _req;
  _req.set_node_requests({std::move(updt)});

  // The constant base for all the updates. Copy it and modify the request.
  const thrift::UpdateNodesRequest request_tpl{std::move(_req)};

  {
    // A mismatch in the node's index should fail.
    auto req = request_tpl;
    req.node_requests_ref()[0].set_node_to_be_updated(mkNodeID(2));

    try {
      thrift::UpdateNodesResponse resp;
      admin_client->sync_updateNodes(resp, req);
      FAIL() << "UpdateNodes call should fail, but it didn't";
    } catch (const thrift::ClusterMembershipOperationFailed& exception) {
      ASSERT_EQ(1, exception.failed_nodes_ref()->size());
      auto failed_node = exception.failed_nodes_ref()[0];
      EXPECT_EQ(
          2, failed_node.node_id_ref()->node_index_ref().value_unchecked());
      EXPECT_EQ(
          thrift::ClusterMembershipFailureReason::INVALID_REQUEST_NODES_CONFIG,
          *failed_node.reason_ref());
    }
  }

  {
    // Trying to update a node that doesn't exist should fail
    auto req = request_tpl;
    req.node_requests_ref()[0].set_node_to_be_updated(mkNodeID(20));

    try {
      thrift::UpdateNodesResponse resp;
      admin_client->sync_updateNodes(resp, req);
      FAIL() << "UpdateNodes call should fail, but it didn't";
    } catch (const thrift::ClusterMembershipOperationFailed& exception) {
      ASSERT_EQ(1, exception.failed_nodes_ref()->size());
      auto failed_node = exception.failed_nodes_ref()[0];
      EXPECT_EQ(
          20, failed_node.node_id_ref()->node_index_ref().value_unchecked());
      EXPECT_EQ(thrift::ClusterMembershipFailureReason::NO_MATCH_IN_CONFIG,
                *failed_node.reason_ref());
    }
  }

  {
    // Trying to update an immutable attribute (e.g location) will fail with an
    // NCM error.
    auto req = request_tpl;
    req.node_requests_ref()[0].new_config_ref()->set_location(
        "FRC.FRC.FRC.FRC.FRC");

    try {
      thrift::UpdateNodesResponse resp;
      admin_client->sync_updateNodes(resp, req);
      FAIL() << "UpdateNodes call should fail, but it didn't";
    } catch (const thrift::NodesConfigurationManagerError& exception) {
      EXPECT_EQ(static_cast<int32_t>(E::INVALID_PARAM),
                exception.error_code_ref().value_unchecked());
    }
  }
}

TEST_F(ClusterMemebershipAPIIntegrationTest, MarkShardsAsProvisionedSuccess) {
  ASSERT_EQ(0, cluster_->start({0, 1, 2, 3}));
  cluster_->getAdminServer()->waitUntilFullyLoaded();
  auto admin_client = cluster_->getAdminServer()->createAdminClient();

  {
    // Add two nodes with 2 shards each. They will get added as PROVISIONING.
    thrift::AddNodesResponse resp;
    admin_client->sync_addNodes(resp, buildAddNodesRequest({100, 101}));
    ASSERT_EQ(2, resp.added_nodes_ref()->size());

    waitUntilMaintenanceManagerHasNCVersion(
        admin_client, *resp.new_nodes_configuration_version_ref());
  }

  // Mark all N100 shards, and only N101:S0 as provisioned
  thrift::MarkShardsAsProvisionedRequest req;
  req.set_shards({mkShardID(100, -1), mkShardID(101, 0)});

  thrift::MarkShardsAsProvisionedResponse resp;
  admin_client->sync_markShardsAsProvisioned(resp, req);
  EXPECT_THAT(resp.get_updated_shards(),
              UnorderedElementsAre(
                  mkShardID(100, 0), mkShardID(100, 1), mkShardID(101, 0)));

  waitUntilMaintenanceManagerHasNCVersion(
      admin_client, *resp.new_nodes_configuration_version_ref());

  auto get_shard_state = [&](const thrift::NodesState& state,
                             thrift::ShardID shard) {
    for (const auto& node : state) {
      if (node.get_config().get_node_index() ==
          *shard.get_node().node_index_ref()) {
        return node.shard_states_ref()
            ->at(shard.get_shard_index())
            .get_storage_state();
      }
    }
    return membership::thrift::StorageState::INVALID;
  };

  thrift::NodesStateRequest state_req;
  state_req.set_filter(thrift::NodesFilter{});

  thrift::NodesStateResponse nc;
  admin_client->sync_getNodesState(nc, state_req);

  // MM could have already started enabling the shards after getting out of the
  // PROVISIONING state. So it's just enough to check that they are not in
  // PROVISIONING anymore.
  EXPECT_NE(membership::thrift::StorageState::PROVISIONING,
            get_shard_state(nc.get_states(), mkShardID(100, 0)));
  EXPECT_NE(membership::thrift::StorageState::PROVISIONING,
            get_shard_state(nc.get_states(), mkShardID(100, 1)));
  EXPECT_NE(membership::thrift::StorageState::PROVISIONING,
            get_shard_state(nc.get_states(), mkShardID(101, 0)));
  EXPECT_EQ(membership::thrift::StorageState::PROVISIONING,
            get_shard_state(nc.get_states(), mkShardID(101, 1)));
}

// Tests that bumping the generation of the stopped node (N1) works
TEST_F(ClusterMemebershipAPIIntegrationTest, BumpNodeGeneration) {
  ASSERT_EQ(0, cluster_->start({0, 2, 3}));
  cluster_->getAdminServer()->waitUntilFullyLoaded();
  auto admin_client = cluster_->getAdminServer()->createAdminClient();

  thrift::NodesFilter filter;
  filter.set_node(mkNodeID(node_index_t(1)));
  thrift::BumpGenerationRequest req;
  req.set_node_filters({std::move(filter)});

  thrift::BumpGenerationResponse resp;
  admin_client->sync_bumpNodeGeneration(resp, std::move(req));

  EXPECT_EQ(
      (std::vector<thrift::NodeID>{mkNodeID(1)}), *resp.bumped_nodes_ref());
  auto new_version = *resp.new_nodes_configuration_version_ref();

  // Admin server doesn't expose an API to check node's generation. Let's read
  // it from from the NC directly.
  auto nc = cluster_->readNodesConfigurationFromStore();
  ASSERT_NE(nullptr, nc);
  ASSERT_EQ(new_version, nc->getVersion().val());

  EXPECT_EQ(1, nc->getNodeStorageAttribute(0)->generation);
  EXPECT_EQ(2, nc->getNodeStorageAttribute(1)->generation);
  EXPECT_EQ(1, nc->getNodeStorageAttribute(2)->generation);
  EXPECT_EQ(1, nc->getNodeStorageAttribute(3)->generation);
}

TEST(ClusterMemebershipAPIIntegrationTest2, BootstrapCluster) {
  auto cluster = IntegrationTestUtils::SelfRegisteredCluster::create();

  std::vector<std::unique_ptr<IntegrationTestUtils::Node>> nodes;
  nodes.push_back(cluster->createSelfRegisteringNode("server_1"));
  nodes.push_back(cluster->createSelfRegisteringNode("server_2"));
  nodes.push_back(cluster->createSelfRegisteringNode("server_3"));

  // Start all the node and wait until they are all provisioned
  for (auto& node : nodes) {
    node->start();
  }
  for (auto& node : nodes) {
    node->waitUntilStarted();
  }
  wait_until("All the shards are marked provisioned", [&]() {
    auto nc = cluster->readNodesConfigurationFromStore();
    ld_check(nc);
    auto mem = nc->getStorageMembership();

    if (mem->getMembershipNodes().size() < nodes.size()) {
      return false;
    }

    auto all_provisioned = true;
    for (auto& node : mem->getMembershipNodes()) {
      for (const auto& [shard, state] : mem->getShardStates(node)) {
        all_provisioned = all_provisioned &&
            state.storage_state == membership::StorageState::NONE;
      }
    }
    return all_provisioned;
  });

  // The cluster is ready to be marked as bootstrapped
  auto admin_client = nodes[0]->createAdminClient();

  // Replicate metadata across three nodes
  thrift::BootstrapClusterRequest req;
  req.set_metadata_replication_property({{thrift::LocationScope::NODE, 3}});
  thrift::BootstrapClusterResponse resp;
  admin_client->sync_bootstrapCluster(resp, std::move(req));

  // Validate the changes that happened to NC
  auto nc = cluster->readNodesConfigurationFromStore();

  auto storage_mem = nc->getStorageMembership();
  auto seq_mem = nc->getSequencerMembership();

  // First check that NC is not bootstrapping anymore.
  EXPECT_FALSE(seq_mem->isBootstrapping());
  EXPECT_FALSE(storage_mem->isBootstrapping());

  // All sequencers should be enabled
  EXPECT_EQ(3, seq_mem->getMembershipNodes().size());
  for (const auto& seq : seq_mem->getMembershipNodes()) {
    auto state = seq_mem->getNodeState(seq);
    EXPECT_TRUE(state->sequencer_enabled);
    EXPECT_EQ(1, state->getConfiguredWeight());
  }

  // All storage should be enabled
  EXPECT_EQ(3, storage_mem->getMembershipNodes().size());
  for (auto& node : storage_mem->getMembershipNodes()) {
    for (const auto& [shard, state] : storage_mem->getShardStates(node)) {
      EXPECT_EQ(membership::StorageState::READ_WRITE, state.storage_state);
    }
  }

  // Metadata replication property should be correctly set
  auto rep_prop = nc->getMetaDataLogsReplication()->getReplicationProperty();
  EXPECT_EQ(ReplicationProperty(3, NodeLocationScope::NODE), rep_prop);

  // Nodeset must spawn the whole cluster
  EXPECT_EQ(
      (std::set<node_index_t>{0, 1, 2}), storage_mem->getMetaDataNodeSet());
}
