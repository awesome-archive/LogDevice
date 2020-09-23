/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "logdevice/common/RandomNodeSelector.h"

#include <folly/Random.h>
#include <gtest/gtest.h>

#include "logdevice/common/configuration/Configuration.h"
#include "logdevice/common/test/TestUtil.h"
#include "logdevice/common/toString.h"

using namespace facebook::logdevice;

namespace {

using NodeSourceSet = RandomNodeSelector::NodeSourceSet;

TEST(RandomNodeSelector, OneNode) {
  auto node_config = createSimpleNodesConfig(1 /* 1 node */);

  EXPECT_EQ(
      node_config->getNodeID(0), RandomNodeSelector::getNode(*node_config));
}

TEST(RandomNodeSelector, ExcludeNode) {
  auto node_config = createSimpleNodesConfig(2 /* 2 nodes*/);

  auto exclude = node_config->getNodeID(0);
  auto node = node_config->getNodeID(1);
  EXPECT_EQ(node, RandomNodeSelector::getNode(*node_config, exclude));
}

TEST(RandomNodeSelector, DontExcludeSingleNode) {
  auto node_config = createSimpleNodesConfig(1 /* 1 node */);

  // exclude the only node there is
  auto exclude = node_config->getNodeID(0);
  auto node = exclude;

  EXPECT_EQ(node, RandomNodeSelector::getNode(*node_config, exclude));
}

struct SelectionParams {
  NodeSourceSet candidates;
  NodeSourceSet existing;
  NodeSourceSet blacklist;
  NodeSourceSet graylist;
  size_t num_required;
  size_t num_extras;
  folly::Optional<u_int32_t> seed;
  ClusterState* filter;
};

inline bool canPick(const SelectionParams& params, node_index_t n) {
  return params.candidates.count(n) > 0 && params.existing.count(n) == 0 &&
      params.blacklist.count(n) == 0 &&
      (!params.filter || params.filter->isNodeAlive(n));
}

inline NodeSourceSet setDifference(const NodeSourceSet& A,
                                   const NodeSourceSet& B) {
  NodeSourceSet result;
  for (const auto n : A) {
    if (B.count(n) == 0) {
      result.insert(n);
    }
  }
  return result;
}

inline NodeSourceSet setIntersection(const NodeSourceSet& A,
                                     const NodeSourceSet& B) {
  NodeSourceSet result;
  for (const auto n : A) {
    if (B.count(n) > 0) {
      result.insert(n);
    }
  }
  return result;
}

inline void validateResult(const SelectionParams& params,
                           const NodeSourceSet& result) {
  ld_debug("\nC:%s\nE:%s\nB:%s\nG:%s\nreq:%lu, extras:%lu\nR:%s\n.",
           toString(params.candidates).c_str(),
           toString(params.existing).c_str(),
           toString(params.blacklist).c_str(),
           toString(params.graylist).c_str(),
           params.num_required,
           params.num_extras,
           toString(result).c_str());
  const size_t num_can_pick = std::count_if(
      params.candidates.begin(), params.candidates.end(), [&](node_index_t n) {
        return canPick(params, n);
      });

  if (num_can_pick < params.num_required) {
    // selection must fail
    EXPECT_TRUE(result.empty());
    return;
  }

  // size requirements
  EXPECT_GE(result.size(), params.num_required);
  EXPECT_LE(result.size(), params.num_required + params.num_extras);
  EXPECT_LE(result.size(), num_can_pick);

  // all picked nodes must be legit
  EXPECT_TRUE(std::all_of(result.begin(), result.end(), [&](node_index_t n) {
    return canPick(params, n);
  }));

  // if any graylisted nodes are picked, all nodes in candiates - result must be
  // either graylisted or not eligible
  auto not_picked = setDifference(params.candidates, result);
  if (!setIntersection(result, params.graylist).empty()) {
    EXPECT_TRUE(
        std::none_of(not_picked.begin(), not_picked.end(), [&](node_index_t n) {
          return canPick(params, n) && params.graylist.count(n) == 0;
        }));
  }

  if (result.size() < params.num_required + params.num_extras) {
    EXPECT_TRUE(
        std::none_of(not_picked.begin(), not_picked.end(), [&](node_index_t n) {
          return canPick(params, n);
        }));
  }
}

inline NodeSourceSet genRandomSet(node_index_t min,
                                  node_index_t max,
                                  size_t min_size,
                                  size_t max_size) {
  ld_check(max >= min);
  ld_check(min_size <= max_size);
  const size_t all_size = max - min + 1;
  ld_check(all_size >= max_size);

  size_t size = folly::Random::rand32(min_size, max_size + 1);
  std::vector<node_index_t> nodes;
  nodes.resize(all_size);
  std::iota(nodes.begin(), nodes.end(), min);
  std::shuffle(nodes.begin(), nodes.end(), folly::ThreadLocalPRNG());
  nodes.resize(size);
  return NodeSourceSet(nodes.begin(), nodes.end());
}

TEST(RandomNodeSelector, SourceSelectBasicTest) {
  {
    auto result = RandomNodeSelector::select(
        {1, 2, 3, 4, 5}, {1}, {2}, {3}, 2, 1, folly::none, nullptr);
    NodeSourceSet expected{3, 4, 5};
    EXPECT_EQ(expected, result);
  }
  {
    auto result = RandomNodeSelector::select(
        {1, 2, 3, 4, 5}, {1}, {2, 3}, {}, 2, 1, folly::none, nullptr);
    NodeSourceSet expected{4, 5};
    EXPECT_EQ(expected, result);
  }
  {
    auto result = RandomNodeSelector::select(
        {1, 2, 3, 4, 5}, {1, 4}, {2, 3}, {}, 2, 0, folly::none, nullptr);
    EXPECT_TRUE(result.empty());
  }
}

TEST(RandomNodeSelector, SameOrderTest) {
  for (int i = 0; i < 100; i++) {
    folly::Optional<u_int32_t> seed = folly::Random::rand32();
    std::vector<int> candidates(10);
    std::iota(candidates.begin(), candidates.end(), 0);
    NodeSourceSet set(candidates.begin(), candidates.end());
    auto expected =
        RandomNodeSelector::select(set, {1}, {2}, {3}, 2, 1, seed, nullptr);
    for (int j = 0; j < 10; j++) {
      u_int32_t randomSeed = folly::Random::rand32();
      std::shuffle(candidates.begin(),
                   candidates.end(),
                   std::default_random_engine(randomSeed));
      NodeSourceSet shuffledSet(candidates.begin(), candidates.end());
      auto result = RandomNodeSelector::select(
          shuffledSet, {1}, {2}, {3}, 2, 1, seed, nullptr);
      EXPECT_EQ(expected, result);
    }
  }
}

TEST(RandomNodeSelector, SourceSelectionRandomTest) {
  for (int i = 0; i < 100; ++i) {
    SelectionParams params;
    params.candidates = genRandomSet(0, 100, 0, 100);
    params.existing = genRandomSet(0, 100, 0, 100);
    params.blacklist = genRandomSet(0, 100, 0, 100);
    params.graylist = genRandomSet(0, 100, 0, 100);
    params.num_required = folly::Random::rand32(1, 15);
    params.num_extras = folly::Random::rand32(0, 15);
    params.seed = folly::none;
    params.filter = nullptr;
    auto result = RandomNodeSelector::select(params.candidates,
                                             params.existing,
                                             params.blacklist,
                                             params.graylist,
                                             params.num_required,
                                             params.num_extras,
                                             params.seed,
                                             params.filter);
    validateResult(params, result);
  }
}

} // namespace
