/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include "logdevice/common/configuration/Node.h"
#include "logdevice/common/configuration/nodes/NodesConfiguration.h"

namespace facebook {
  namespace logdevice {
    namespace NodesConfigurationTestUtil {

extern const configuration::nodes::NodeServiceDiscovery::RoleSet kSeqRole;
extern const configuration::nodes::NodeServiceDiscovery::RoleSet kStorageRole;
extern const configuration::nodes::NodeServiceDiscovery::RoleSet kBothRoles;
extern const configuration::nodes::NodeServiceDiscovery::TagMap kTestTags;

configuration::nodes::NodeServiceDiscovery
genDiscovery(node_index_t n, const configuration::Node& node);

configuration::nodes::NodesConfiguration::Update
initialAddShardsUpdate(std::vector<node_index_t> node_idxs);

configuration::nodes::NodesConfiguration::Update
initialAddShardsUpdate(configuration::Nodes nodes,
                       ReplicationProperty metadata_rep = ReplicationProperty{
                           {NodeLocationScope::NODE, 2}});

std::shared_ptr<const configuration::nodes::NodesConfiguration> provisionNodes(
    configuration::nodes::NodesConfiguration::Update provision_update,
    std::unordered_set<ShardID> metadata_shards);

// If an idx is passed, we use it, otherwise we use the largest idx + 1.
configuration::nodes::NodesConfiguration::Update
addNewNodeUpdate(const configuration::nodes::NodesConfiguration& existing,
                 const configuration::Node& node,
                 folly::Optional<node_index_t> idx = folly::none);

configuration::nodes::NodesConfiguration::Update
addNewNodesUpdate(const configuration::nodes::NodesConfiguration& existing,
                  configuration::Nodes nodes);

// Create an NC::Update to transition all PROVISIONING shards to NONE by
// applying a MARK_SHARD_PROVISIONED transition.
configuration::nodes::NodesConfiguration::Update markAllShardProvisionedUpdate(
    const configuration::nodes::NodesConfiguration& existing);

// Create an NC::Update to transition all NONE shards to RW by applying a
// BOOTSTRAP_ENABLE_SHARD transition.
configuration::nodes::NodesConfiguration::Update bootstrapEnableAllShardsUpdate(
    const configuration::nodes::NodesConfiguration& existing,
    std::unordered_set<ShardID> metadata_shards = {});

// Create an NC::Update to unset the bootstrapping flag in both sequencer and
// storage memberhip.
configuration::nodes::NodesConfiguration::Update finalizeBootstrappingUpdate(
    const configuration::nodes::NodesConfiguration& existing);

// provision a specific LD nodes config with:
// 1) nodes N1, N2, N7, N9, N11, N13
// 2) N1 and N7 have sequencer role; N1, N2, N9, N11, N13 have storage role;
// 3) N2 and N9 are metadata storage nodes, metadata logs replicaton is
//    (rack, 2)
configuration::nodes::NodesConfiguration::Update initialAddShardsUpdate();
std::shared_ptr<const configuration::nodes::NodesConfiguration>
provisionNodes();

std::shared_ptr<const configuration::nodes::NodesConfiguration>
provisionNodes(std::vector<node_index_t> node_idxs,
               std::unordered_set<ShardID> metadata_shards = {});

std::shared_ptr<const configuration::nodes::NodesConfiguration>
provisionNodes(configuration::Nodes nodes,
               ReplicationProperty metadata_rep = ReplicationProperty{
                   {NodeLocationScope::NODE, 2}});

// provision a new node
configuration::nodes::NodesConfiguration::Update
addNewNodeUpdate(const configuration::nodes::NodesConfiguration& existing,
                 node_index_t new_node_idx);

// NOTE: the base_version below should be storage membership versions
// inside the NC instead of the NC versions.

// start enabling read on N17
configuration::nodes::NodesConfiguration::Update
enablingReadUpdate(membership::MembershipVersion::Type base_version);

// start disabling writes on N11 and N13
configuration::nodes::NodesConfiguration::Update
disablingWriteUpdate(membership::MembershipVersion::Type base_version);

configuration::nodes::NodesConfiguration::Update setStorageMembershipUpdate(
    const configuration::nodes::NodesConfiguration& existing,
    std::vector<ShardID> shards,
    folly::Optional<membership::StorageState> target_storage_state,
    folly::Optional<membership::MetaDataStorageState> target_metadata_state);

configuration::nodes::NodesConfiguration::Update setSequencerEnabledUpdate(
    const configuration::nodes::NodesConfiguration& existing,
    std::vector<node_index_t> nodes,
    bool target_sequencer_enabled);

configuration::nodes::NodesConfiguration::Update setSequencerWeightUpdate(
    const configuration::nodes::NodesConfiguration& existing,
    std::vector<node_index_t> nodes,
    double target_sequencer_weight);

configuration::nodes::NodesConfiguration::Update excludeFromNodesetUpdate(
    const configuration::nodes::NodesConfiguration& existing,
    std::vector<node_index_t> nodes,
    bool target_exclude_from_nodeset);

configuration::nodes::NodesConfiguration::Update
setMetadataReplicationPropertyUpdate(
    const configuration::nodes::NodesConfiguration& existing,
    ReplicationProperty metadata_rep);

configuration::nodes::NodesConfiguration::Update setNodeAttributesUpdate(
    node_index_t node,
    folly::Optional<configuration::nodes::NodeServiceDiscovery> svc_disc,
    folly::Optional<configuration::nodes::SequencerNodeAttribute> seq_attrs,
    folly::Optional<configuration::nodes::StorageNodeAttribute> storage_attrs);

configuration::nodes::NodesConfiguration::Update
shrinkNodesUpdate(const configuration::nodes::NodesConfiguration& existing,
                  std::vector<node_index_t> nodes);

}}} // namespace facebook::logdevice::NodesConfigurationTestUtil
