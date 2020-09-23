/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include <folly/Memory.h>
#include <gtest/gtest.h>

#include "logdevice/common/SequencerLocator.h"
#include "logdevice/common/debug.h"
#include "logdevice/common/test/MockSequencerRouter.h"
#include "logdevice/common/test/NodeSetTestUtil.h"
#include "logdevice/common/test/TestUtil.h"

namespace facebook { namespace logdevice {

// A SequencerLocator that will always return the same destination node.
class StaticLocator : public SequencerLocator {
 public:
  explicit StaticLocator(NodeID dest) : dest_(dest) {}

  int locateSequencer(
      logid_t log_id,
      Completion cf,
      std::shared_ptr<configuration::SequencersConfig>) override {
    cf(E::OK, log_id, dest_);
    return 0;
  }
  bool isAllowedToCache() const override {
    return can_cache_;
  }

  bool can_cache_{true};

 private:
  NodeID dest_;
};

class SequencerRouterTest : public ::testing::Test,
                            public SequencerRouter::Handler {
 public:
  SequencerRouterTest() {
    dbg::assertOnData = true;
  }

  std::unique_ptr<SequencerRouter>
  createRouter(logid_t log_id,
               std::shared_ptr<const NodesConfiguration> nodes_config) {
    cluster_state_ =
        std::make_unique<MockClusterState>(nodes_config->clusterSize());
    auto router = std::make_unique<MockSequencerRouter>(
        log_id, this, nodes_config, locator_, cluster_state_.get());
    return std::move(router);
  }

  void onSequencerKnown(NodeID dest, SequencerRouter::flags_t flags) override {
    next_node_ = std::make_pair(dest, flags);
  }
  void onSequencerRoutingFailure(Status status) override {
    status_ = status;
  }

  std::shared_ptr<SequencerLocator> locator_;
  std::pair<NodeID,
            SequencerRouter::flags_t>
      next_node_;             // node the next message will be routed to
  Status status_{E::UNKNOWN}; // status of the whole operatio
  std::unique_ptr<ClusterState> cluster_state_{nullptr};
};

// In this test, SequencerLocator ignores the list of available sequencers and
// always returns the same node, the one that just so happens to be unavailable.
// Tests that SequencerRouter is able to detect this an report an error as early
// as possible.
TEST_F(SequencerRouterTest, RoutingToUnavailableNode) {
  const NodeID N0(0, 1);

  // all logs map to N0
  locator_ = std::make_shared<StaticLocator>(N0);

  auto router = createRouter(logid_t(1), createSimpleNodesConfig(4));
  router->start();
  ASSERT_EQ(std::make_pair(N0, SequencerRouter::flags_t(0)), next_node_);

  router->onNodeUnavailable(N0, E::CONNFAILED);
  EXPECT_EQ(E::CONNFAILED, status_);
}

// Tests a scenario in which the node that log 1 maps to replies with a
// preemption redirect, but the node it redirected to is unavailable.
TEST_F(SequencerRouterTest, PreemptedByUnavailableNode) {
  const NodeID N0(0, 1), N1(1, 1);

  // N0 takes care of all logs by default
  locator_ = std::make_shared<StaticLocator>(N0);

  auto router = createRouter(logid_t(1), createSimpleNodesConfig(4));
  router->start();
  ASSERT_EQ(std::make_pair(N0, SequencerRouter::flags_t(0)), next_node_);

  // N0 redirects to N1, saying that it got preempted
  router->onRedirected(N0, N1, E::PREEMPTED);
  ASSERT_EQ(std::make_pair(N1, SequencerRouter::flags_t(0)), next_node_);

  // N1 is down, we expect another message to be sent to N0
  router->onNodeUnavailable(N1, E::CONNFAILED);
  EXPECT_EQ(std::make_pair(N0, SequencerRouter::flags_t(0)), next_node_);

  // If N0 still replies with E::PREEMPTED (it didn't detect that N1 is down
  // yet), request should fail
  router->onRedirected(N0, N1, E::PREEMPTED);
  ASSERT_EQ(std::make_pair(N1, SequencerRouter::flags_t(0)), next_node_);

  router->onNodeUnavailable(N1, E::CONNFAILED);
  EXPECT_EQ(E::CONNFAILED, status_);
}

// Tests a scenario in which the node that log 1 maps to replies with a
// redirect, but the node it redirected to is not ready. the router should
// set both redirect_cycle and preempted flags to prevent being redirected
// again either with a regular redirect or preempted redirect.
TEST_F(SequencerRouterTest, RedirectedToNotReadyNode) {
  const NodeID N0(0, 1), N1(1, 1);

  // N0 takes care of all logs by default
  locator_ = std::make_shared<StaticLocator>(N0);

  auto router = createRouter(logid_t(1), createSimpleNodesConfig(4));
  router->start();
  ASSERT_EQ(std::make_pair(N0, SequencerRouter::flags_t(0)), next_node_);

  // N0 redirects to N1, saying that it got preempted
  router->onRedirected(N0, N1, E::REDIRECTED);
  ASSERT_EQ(std::make_pair(N1, SequencerRouter::flags_t(0)), next_node_);

  // N1 is not ready (sequencer may be disabled), we expect another
  // message to be sent to N0 and flags must include PREEMPTED. This is
  // to prevent any redirect at all, even if the sequencer tries to reactivate
  // and finds it is in preempted state.
  SequencerRouter::flags_t expected_flags = SequencerRouter::FORCE_REACTIVATION;
  router->onNodeUnavailable(N1, E::NOTREADY);
  EXPECT_EQ(std::make_pair(N0, expected_flags), next_node_);
}

// Tests a scenario in which the node that log 1 maps to replies with a
// preemption redirect, but the node it redirected to is dead and the
// sequencer knows it and sets the REDIRECT_NOT_ALIVE flag
TEST_F(SequencerRouterTest, PreemptedByDeadNode) {
  const NodeID N0(0, 1), N1(1, 1);

  // N0 takes care of all logs by default
  locator_ = std::make_shared<StaticLocator>(N0);

  auto router = createRouter(logid_t(1), createSimpleNodesConfig(4));
  router->start();
  ASSERT_EQ(std::make_pair(N0, SequencerRouter::flags_t(0)), next_node_);

  // N0 redirects to N1, saying that it got preempted but knows it is
  // dead and sets the REDIRECT_NOT_ALIVE flag.
  // the following is done by the AppendRequest
  router->onDeadNode(N1, E::DISABLED);
  router->onRedirected(N0, NodeID(), E::PREEMPTED);

  // next selected node should be N0 again, and flags should force reactivation
  SequencerRouter::flags_t expected_flags = SequencerRouter::FORCE_REACTIVATION;
  ASSERT_EQ(std::make_pair(N0, expected_flags), next_node_);

  // If N0 still replies with E::PREEMPTED (it didn't detect that N1 is down
  // yet), request should fail
  router->onRedirected(N0, NodeID(), E::PREEMPTED);
  EXPECT_EQ(E::NOSEQUENCER, status_);
}

// Tests if the node with the location matching the sequencerAffinity is chosen
// as the sequencer. If there are none, it makes sure the SequencerLocator
// still picks something.
TEST_F(SequencerRouterTest, SequencerAffinityTest) {
  const NodeID N0(0, 1), N1(1, 1), N2(2, 1), N3(3, 1), N4(4, 1), N5(5, 1),
      N6(6, 1);
  auto settings = create_default_settings<Settings>();

  // Config with 2 regions each with 1 node
  auto nodes_config = std::make_shared<const NodesConfiguration>();
  NodeSetTestUtil::addNodes(nodes_config, 1, 2, "rgn1.dc1.cl1.row1.rck1");
  NodeSetTestUtil::addNodes(nodes_config, 1, 2, "rgn2.dc2.cl2.row2.rck2");

  std::shared_ptr<Configuration> config =
      Configuration::fromJsonFile(
          TEST_CONFIG_FILE("sequencer_affinity_2nodes.conf"))
          ->withNodesConfiguration(std::move(nodes_config));

  cluster_state_ = std::make_unique<MockClusterState>(
      config->getNodesConfiguration()->clusterSize());

  settings.use_sequencer_affinity = true;
  locator_ = std::make_unique<MockHashBasedSequencerLocator>(
      cluster_state_.get(), config, settings);

  // Log with id 1 prefers rgn1. N0 is the only node in that region.
  auto router =
      std::make_unique<MockSequencerRouter>(logid_t(1),
                                            this,
                                            config->getNodesConfiguration(),
                                            locator_,
                                            cluster_state_.get());
  router->start();
  EXPECT_EQ(std::make_pair(N0, SequencerRouter::flags_t(0)), next_node_);

  // Log with id 2 prefers rgn2. N1 is the only node in that region.
  router =
      std::make_unique<MockSequencerRouter>(logid_t(2),
                                            this,
                                            config->getNodesConfiguration(),
                                            locator_,
                                            cluster_state_.get());
  router->start();
  EXPECT_EQ(std::make_pair(N1, SequencerRouter::flags_t(0)), next_node_);

  // Log with id 3 prefers rgn3. No nodes are in that region so it chooses a
  // sequencer at random.
  router =
      std::make_unique<MockSequencerRouter>(logid_t(3),
                                            this,
                                            config->getNodesConfiguration(),
                                            locator_,
                                            cluster_state_.get());
  router->start();
  EXPECT_TRUE(std::make_pair(N0, SequencerRouter::flags_t(0)) == next_node_ ||
              std::make_pair(N1, SequencerRouter::flags_t(0)) == next_node_);

  settings.use_sequencer_affinity = false;
  locator_ = std::make_unique<MockHashBasedSequencerLocator>(
      cluster_state_.get(), config, settings);

  router =
      std::make_unique<MockSequencerRouter>(logid_t(2),
                                            this,
                                            config->getNodesConfiguration(),
                                            locator_,
                                            cluster_state_.get());
  router->start();
  EXPECT_NE(std::make_pair(N1, SequencerRouter::flags_t(0)), next_node_);

  // Config with 3 regions each with 3 nodes
  nodes_config = std::make_shared<const NodesConfiguration>();
  NodeSetTestUtil::addNodes(nodes_config, 3, 2, "rgn1.dc1.cl1.row1.rck1");
  NodeSetTestUtil::addNodes(nodes_config, 3, 2, "rgn2.dc2.cl2.row2.rck2");
  NodeSetTestUtil::addNodes(nodes_config, 1, 2, "rgn3.dc3.cl3.row3.rck3");

  config = Configuration::fromJsonFile(
               TEST_CONFIG_FILE("sequencer_affinity_7nodes.conf"))
               ->withNodesConfiguration(std::move(nodes_config));

  cluster_state_ = std::make_unique<MockClusterState>(
      config->getNodesConfiguration()->clusterSize());

  settings.use_sequencer_affinity = true;
  locator_ = std::make_unique<MockHashBasedSequencerLocator>(
      cluster_state_.get(), config, settings);

  // Log with id 1 prefers rgn1. This region has 3 nodes.
  router =
      std::make_unique<MockSequencerRouter>(logid_t(1),
                                            this,
                                            config->getNodesConfiguration(),
                                            locator_,
                                            cluster_state_.get());
  router->start();
  EXPECT_EQ(std::make_pair(N1, SequencerRouter::flags_t(0)), next_node_);
  router->onNodeUnavailable(N1, E::CONNFAILED);
  EXPECT_EQ(std::make_pair(N0, SequencerRouter::flags_t(0)), next_node_);
  router->onNodeUnavailable(N0, E::CONNFAILED);
  EXPECT_EQ(std::make_pair(N2, SequencerRouter::flags_t(0)), next_node_);
  router->onNodeUnavailable(N2, E::CONNFAILED);
  EXPECT_TRUE(std::make_pair(N3, SequencerRouter::flags_t(0)) == next_node_ ||
              std::make_pair(N4, SequencerRouter::flags_t(0)) == next_node_ ||
              std::make_pair(N5, SequencerRouter::flags_t(0)) == next_node_ ||
              std::make_pair(N6, SequencerRouter::flags_t(0)) == next_node_);

  // Log with id 3 prefers rgn3 but we disable the use-sequencer-affinity
  // setting.
  router =
      std::make_unique<MockSequencerRouter>(logid_t(3),
                                            this,
                                            config->getNodesConfiguration(),
                                            locator_,
                                            cluster_state_.get());
  router->start();
  EXPECT_EQ(std::make_pair(N6, SequencerRouter::flags_t(0)), next_node_);

  settings.use_sequencer_affinity = false;
  locator_ = std::make_unique<MockHashBasedSequencerLocator>(
      cluster_state_.get(), config, settings);
  router =
      std::make_unique<MockSequencerRouter>(logid_t(3),
                                            this,
                                            config->getNodesConfiguration(),
                                            locator_,
                                            cluster_state_.get());
  router->start();
  // Should pick a sequencer that is not N6 since N6 has very low weight.
  EXPECT_TRUE(std::make_pair(N0, SequencerRouter::flags_t(0)) == next_node_ ||
              std::make_pair(N1, SequencerRouter::flags_t(0)) == next_node_ ||
              std::make_pair(N2, SequencerRouter::flags_t(0)) == next_node_ ||
              std::make_pair(N3, SequencerRouter::flags_t(0)) == next_node_ ||
              std::make_pair(N4, SequencerRouter::flags_t(0)) == next_node_ ||
              std::make_pair(N5, SequencerRouter::flags_t(0)) == next_node_);
}

// Tests if the log ID is not in config, SequencerRouter returns NOTFOUND
TEST_F(SequencerRouterTest, LogNotFound) {
  auto settings = create_default_settings<Settings>();

  // Config with 2 regions each with 1 node
  // only 3 logs are configured.
  auto nodes_config = std::make_shared<const NodesConfiguration>();
  NodeSetTestUtil::addNodes(nodes_config, 1, 2, "rgn1.dc1.cl1.row1.rck1");
  NodeSetTestUtil::addNodes(nodes_config, 1, 2, "rgn2.dc2.cl2.row2.rck2");

  std::shared_ptr<Configuration> config =
      Configuration::fromJsonFile(
          TEST_CONFIG_FILE("sequencer_affinity_2nodes.conf"))
          ->withNodesConfiguration(std::move(nodes_config));

  cluster_state_ = std::make_unique<MockClusterState>(
      config->getNodesConfiguration()->clusterSize());

  settings.use_sequencer_affinity = true;
  locator_ = std::make_unique<MockHashBasedSequencerLocator>(
      cluster_state_.get(), config, settings);

  // log ID 4 doesn't exist
  auto router =
      std::make_unique<MockSequencerRouter>(logid_t(4),
                                            this,
                                            config->getNodesConfiguration(),
                                            locator_,
                                            cluster_state_.get());
  router->start();
  EXPECT_EQ(E::NOTFOUND, status_);
}

}} // namespace facebook::logdevice
