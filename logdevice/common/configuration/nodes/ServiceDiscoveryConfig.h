/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include <folly/Optional.h>
#include <folly/container/F14Map.h>

#include "logdevice/common/ShardID.h"
#include "logdevice/common/Sockaddr.h"
#include "logdevice/common/SocketTypes.h"
#include "logdevice/common/configuration/NodeLocation.h"
#include "logdevice/common/configuration/nodes/NodeAttributesConfig.h"
#include "logdevice/common/configuration/nodes/NodeRole.h"
#include "logdevice/common/configuration/nodes/gen-cpp2/NodesConfiguration_types.h"
#include "logdevice/common/toString.h"

namespace facebook::logdevice::configuration::nodes {

struct NodeServiceDiscovery {
  using ClientNetworkPriority =
      facebook::logdevice::thrift::ClientNetworkPriority;
  using RoleSet = configuration::nodes::RoleSet;
  using TagMap = std::unordered_map<std::string, std::string>;

  /*
   * This is a unique name for the node in the cluster. This is currently not a
   * required field and can be empty.
   */
  std::string name{};

  /*
   * The version provides better control over node self-registration logic.
   * A node will be allowed to update its attributes on joining the cluster
   * only if the proposed version is greater or equal than the current one.
   * The node with the lower version will then be preempted.
   */
  uint64_t version{};

  /**
   * The IP (v4 or v6) address, including port number.
   */
  Sockaddr default_client_data_address{};

  /**
   * The IP (v4 or v6) gossip address, including port number. Semantically, if
   * it's folly::none, it means that the gossip connections should go to the
   * data address.
   */
  folly::Optional<Sockaddr> gossip_address{};

  /**
   * The IP (v4 or v6) address, including port number, for SSL communication.
   * In production this will mostly be identical to address, except for the
   * port. We need both address and ssl_address, so the server could serve
   * both non-SSL and SSL clients.
   */
  folly::Optional<Sockaddr> ssl_address{};

  /**
   * The IP (v4 or v6) address, including port number, for admin server. It can
   * also be a unix socket.
   * If it's folly::none, it means that the node doesn't have admin server
   * enabled.
   */
  folly::Optional<Sockaddr> admin_address;

  /**
   * The IP (v4 or v6) address, including port number, for server-to-server
   * traffic. If it's folly::none, it means that server-to-server traffic
   * doesn't have a dedicated address.
   */
  folly::Optional<Sockaddr> server_to_server_address;

  /**
   * The IP (v4 or v6) address, including port number, for server-to-server
   * Thrift API request traffic. If it's folly::none, it means the node does not
   * accept requests from other nodes over Thrift.
   */
  folly::Optional<Sockaddr> server_thrift_api_address;

  /**
   * The IP (v4 or v6) address, including port number, for server-facing
   * Thrift API request traffic. If it's folly::none, it means the node does not
   * accept requests from clients over Thrift.
   */
  folly::Optional<Sockaddr> client_thrift_api_address;

  /**
   * A mapping from network priority to IP (v4 or v6) address w/ port number.
   */
  folly::F14FastMap<ClientNetworkPriority, Sockaddr> addresses_per_priority;

  /**
   * Location information of the node.
   */
  folly::Optional<NodeLocation> location{};

  /**
   * Bitmap storing node roles
   */
  RoleSet roles{};

  /**
   * Custom tags. LogDevice server doesn't use them but they're useful to
   * companion services.
   */
  TagMap tags;

  const Sockaddr& getGossipAddress() const;

  const Sockaddr& getServerToServerAddress() const;

  bool hasRole(NodeRole role) const {
    auto id = static_cast<size_t>(role);
    return roles.test(id);
  }

  std::string locationStr() const {
    if (!location.has_value()) {
      return "";
    }
    return location.value().toString();
  }

  bool operator==(const NodeServiceDiscovery& rhs) const {
    return default_client_data_address == rhs.default_client_data_address &&
        gossip_address == rhs.gossip_address &&
        ssl_address == rhs.ssl_address && admin_address == rhs.admin_address &&
        server_to_server_address == rhs.server_to_server_address &&
        server_thrift_api_address == rhs.server_thrift_api_address &&
        client_thrift_api_address == rhs.client_thrift_api_address &&
        addresses_per_priority == rhs.addresses_per_priority &&
        location == rhs.location && roles == rhs.roles && tags == rhs.tags &&
        name == rhs.name && version == rhs.version;
  }

  bool operator!=(const NodeServiceDiscovery& rhs) const {
    return !(*this == rhs);
  }

  bool isValid() const;
  std::string toString() const;
  bool isValidForReset(const NodeServiceDiscovery& current) const;

  const RoleSet& getRoles() const {
    return roles;
  }

  static std::string networkPriorityToString(const ClientNetworkPriority&);
};

using ServiceDiscoveryConfig = NodeAttributesConfig<NodeServiceDiscovery>;

} // namespace facebook::logdevice::configuration::nodes
