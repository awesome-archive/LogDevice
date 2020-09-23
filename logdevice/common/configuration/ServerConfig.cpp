/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#define __STDC_FORMAT_MACROS // pull in PRId64 etc

#include "logdevice/common/configuration/ServerConfig.h"

#include <algorithm>
#include <cinttypes>
#include <fcntl.h>
#include <netdb.h>
#include <utility>

#include <folly/Conv.h>
#include <folly/DynamicConverter.h>
#include <folly/FileUtil.h>
#include <folly/compression/Compression.h>
#include <folly/json.h>
#include <folly/synchronization/Baton.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "logdevice/common/FailureDomainNodeSet.h"
#include "logdevice/common/FlowGroupDependencies.h"
#include "logdevice/common/NodeID.h"
#include "logdevice/common/SlidingWindow.h"
#include "logdevice/common/commandline_util_chrono.h"
#include "logdevice/common/configuration/ConfigParser.h"
#include "logdevice/common/configuration/LogsConfigParser.h"
#include "logdevice/common/configuration/ParsingHelpers.h"
#include "logdevice/common/configuration/nodes/utils.h"
#include "logdevice/common/debug.h"
#include "logdevice/common/types_internal.h"
#include "logdevice/common/util.h"
#include "logdevice/include/Err.h"

using namespace facebook::logdevice::configuration::parser;
using facebook::logdevice::configuration::NodeRole;

namespace facebook { namespace logdevice {

// set of keys that are used in configuration json format
static const std::set<std::string> config_recognized_keys = {
    "client_settings",
    "cluster",
    "cluster_creation_time",
    "defaults",
    "log_namespace_delimiter",
    "logs",
    "nodes",
    "metadata_logs",
    "principals",
    "security_information",
    "server_settings",
    "traffic_shaping",
    "read_throttling",
    "version",
    "zookeeper",
};

std::unique_ptr<ServerConfig>
ServerConfig::fromJson(const std::string& jsonPiece) {
  auto parsed = parseJson(jsonPiece);
  // Make sure the parsed string is actually an object
  if (!parsed.isObject()) {
    ld_error("configuration must be a map");
    err = E::INVALID_CONFIG;
    return nullptr;
  }
  return ServerConfig::fromJson(parsed);
}

std::unique_ptr<ServerConfig>
ServerConfig::fromJson(const folly::dynamic& parsed) {
  std::string clusterName;
  config_version_t version;
  OptionalTimestamp clusterCreationTime;
  MetaDataLogsConfig metaDataLogsConfig;
  PrincipalsConfig principalsConfig;
  SecurityConfig securityConfig;
  TrafficShapingConfig trafficShapingConfig;
  ShapingConfig readIOShapingConfig(
      std::set<NodeLocationScope>{NodeLocationScope::NODE},
      std::set<NodeLocationScope>{NodeLocationScope::NODE});
  SettingsConfig serverSettingsConfig;
  SettingsConfig clientSettingsConfig;

  // We need the namespace delimiter before loading log configuration, but we
  // can only set it in the LogsConfig after we've chosen the final LogsConfig
  // instance below.
  std::string ns_delimiter = LogsConfig::default_namespace_delimiter_;

  // This setting has to be in the main config, because a client that doesn't
  // have the logs config should still be able to understand namespaces
  // correctly
  std::string ns_delim_fbstr;
  if (getStringFromMap(parsed, "log_namespace_delimiter", ns_delim_fbstr)) {
    // default delimiter
    // validate that it's a single character.
    if (ns_delim_fbstr.size() > 1) {
      // this must be at most 1-character long.
      ld_error("Cannot accept the value of \"log_namespace_delimiter\", value "
               "is '%s'. This must be at most 1 character, failing!",
               ns_delim_fbstr.c_str());
      err = E::INVALID_CONFIG;
      return nullptr;
    }
    ns_delimiter = ns_delim_fbstr;
  }

  InternalLogs internalLogs(ns_delimiter);

  // ParseSecurityInfo should be called before ParseLogs and ParseMetaDataLog
  // as the securityConfig is used in both.
  bool success = parseClusterName(parsed, clusterName) &&
      parsePrincipals(parsed, principalsConfig) &&
      parseVersion(parsed, version) &&
      parseClusterCreationTime(parsed, clusterCreationTime) &&
      parseSecurityInfo(parsed, securityConfig) &&
      parseTrafficShaping(parsed, trafficShapingConfig) &&
      parseReadIOThrottling(parsed, readIOShapingConfig) &&
      parseMetaDataLog(parsed, securityConfig, metaDataLogsConfig) &&
      parseSettings(parsed, "server_settings", serverSettingsConfig) &&
      parseSettings(parsed, "client_settings", clientSettingsConfig) &&
      parseInternalLogs(parsed, securityConfig, internalLogs);

  if (!success) {
    return nullptr;
  }

  folly::dynamic customFields = folly::dynamic::object;
  for (auto& pair : parsed.items()) {
    if (config_recognized_keys.find(pair.first.asString()) !=
        config_recognized_keys.end()) {
      // This key is supposed to be parsed by logdevice
      continue;
    }
    customFields[pair.first] = pair.second;
  }

  auto config = fromData(std::move(clusterName),
                         std::move(metaDataLogsConfig),
                         std::move(principalsConfig),
                         std::move(securityConfig),
                         std::move(trafficShapingConfig),
                         std::move(readIOShapingConfig),
                         std::move(serverSettingsConfig),
                         std::move(clientSettingsConfig),
                         std::move(internalLogs),
                         std::move(clusterCreationTime),
                         std::move(customFields),
                         ns_delimiter);

  config->setVersion(version);
  return config;
}

ServerConfig::ServerConfig(std::string cluster_name,
                           MetaDataLogsConfig metaDataLogsConfig,
                           PrincipalsConfig principalsConfig,
                           SecurityConfig securityConfig,
                           TrafficShapingConfig trafficShapingConfig,
                           ShapingConfig readIOShapingConfig,
                           SettingsConfig serverSettingsConfig,
                           SettingsConfig clientSettingsConfig,
                           InternalLogs internalLogs,
                           OptionalTimestamp clusterCreationTime,
                           folly::dynamic customFields,
                           const std::string& ns_delimiter)
    : clusterName_(std::move(cluster_name)),
      clusterCreationTime_(std::move(clusterCreationTime)),
      metaDataLogsConfig_(std::move(metaDataLogsConfig)),
      principalsConfig_(std::move(principalsConfig)),
      securityConfig_(std::move(securityConfig)),
      trafficShapingConfig_(std::move(trafficShapingConfig)),
      readIOShapingConfig_(std::move(readIOShapingConfig)),
      serverSettingsConfig_(std::move(serverSettingsConfig)),
      clientSettingsConfig_(std::move(clientSettingsConfig)),
      internalLogs_(std::move(internalLogs)),
      ns_delimiter_(ns_delimiter),
      customFields_(std::move(customFields)) {}

std::shared_ptr<const Principal>
ServerConfig::getPrincipalByName(const std::string* name) const {
  return principalsConfig_.getPrincipalByName(name);
}

std::unique_ptr<ServerConfig>
ServerConfig::fromData(std::string cluster_name,
                       MetaDataLogsConfig metadata_logs,
                       PrincipalsConfig principalsConfig,
                       SecurityConfig securityConfig,
                       TrafficShapingConfig trafficShapingConfig,
                       ShapingConfig readIOShapingConfig,
                       SettingsConfig serverSettingsConfig,
                       SettingsConfig clientSettingsConfig,
                       InternalLogs internalLogs,
                       OptionalTimestamp clusterCreationTime,
                       folly::dynamic customFields,
                       const std::string& ns_delimiter) {
  return std::unique_ptr<ServerConfig>(
      new ServerConfig(std::move(cluster_name),
                       std::move(metadata_logs),
                       std::move(principalsConfig),
                       std::move(securityConfig),
                       std::move(trafficShapingConfig),
                       std::move(readIOShapingConfig),
                       std::move(serverSettingsConfig),
                       std::move(clientSettingsConfig),
                       std::move(internalLogs),
                       std::move(clusterCreationTime),
                       std::move(customFields),
                       ns_delimiter));
}

std::unique_ptr<ServerConfig>
ServerConfig::fromDataTest(std::string cluster_name,
                           MetaDataLogsConfig metadata_logs,
                           PrincipalsConfig principalsConfig,
                           SecurityConfig securityConfig,
                           TrafficShapingConfig trafficShapingConfig,
                           ShapingConfig readIOShapingConfig,
                           SettingsConfig serverSettingsConfig,
                           SettingsConfig clientSettingsConfig,
                           InternalLogs internalLogs,
                           OptionalTimestamp clusterCreationTime,
                           folly::dynamic customFields,
                           const std::string& ns_delimiter) {
  auto config = std::unique_ptr<ServerConfig>(
      new ServerConfig(std::move(cluster_name),
                       std::move(metadata_logs),
                       std::move(principalsConfig),
                       std::move(securityConfig),
                       std::move(trafficShapingConfig),
                       std::move(readIOShapingConfig),
                       std::move(serverSettingsConfig),
                       std::move(clientSettingsConfig),
                       std::move(internalLogs),
                       std::move(clusterCreationTime),
                       std::move(customFields),
                       ns_delimiter));
  return config;
}

std::unique_ptr<ServerConfig> ServerConfig::copy() const {
  std::unique_ptr<ServerConfig> config = fromData(clusterName_,
                                                  metaDataLogsConfig_,
                                                  principalsConfig_,
                                                  securityConfig_,
                                                  trafficShapingConfig_,
                                                  readIOShapingConfig_,
                                                  serverSettingsConfig_,
                                                  clientSettingsConfig_,
                                                  internalLogs_,
                                                  getClusterCreationTime(),
                                                  getCustomFields(),
                                                  ns_delimiter_);
  config->setVersion(version_);
  config->setMainConfigMetadata(main_config_metadata_);
  return config;
}

std::shared_ptr<ServerConfig> ServerConfig::withMetaDataLogsConfig(
    MetaDataLogsConfig metaDataLogsConfig) const {
  std::shared_ptr<ServerConfig> config = fromData(clusterName_,
                                                  std::move(metaDataLogsConfig),
                                                  principalsConfig_,
                                                  securityConfig_,
                                                  trafficShapingConfig_,
                                                  readIOShapingConfig_,
                                                  serverSettingsConfig_,
                                                  clientSettingsConfig_,
                                                  internalLogs_,
                                                  getClusterCreationTime(),
                                                  getCustomFields(),
                                                  ns_delimiter_);
  config->setVersion(version_);
  config->setMainConfigMetadata(main_config_metadata_);
  return config;
}

std::shared_ptr<ServerConfig>
ServerConfig::withVersion(config_version_t version) const {
  std::shared_ptr<ServerConfig> config = fromData(clusterName_,
                                                  metaDataLogsConfig_,
                                                  principalsConfig_,
                                                  securityConfig_,
                                                  trafficShapingConfig_,
                                                  readIOShapingConfig_,
                                                  serverSettingsConfig_,
                                                  clientSettingsConfig_,
                                                  internalLogs_,
                                                  getClusterCreationTime(),
                                                  getCustomFields(),
                                                  ns_delimiter_);
  config->setVersion(version);
  config->setMainConfigMetadata(main_config_metadata_);
  return config;
}

std::shared_ptr<ServerConfig>
ServerConfig::withServerSettings(SettingsConfig server_settings) const {
  std::shared_ptr<ServerConfig> config = fromData(clusterName_,
                                                  metaDataLogsConfig_,
                                                  principalsConfig_,
                                                  securityConfig_,
                                                  trafficShapingConfig_,
                                                  readIOShapingConfig_,
                                                  std::move(server_settings),
                                                  clientSettingsConfig_,
                                                  internalLogs_,
                                                  getClusterCreationTime(),
                                                  getCustomFields(),
                                                  ns_delimiter_);
  config->setVersion(version_);
  config->setMainConfigMetadata(main_config_metadata_);
  return config;
}

std::shared_ptr<ServerConfig>
ServerConfig::withClientSettings(SettingsConfig client_settings) const {
  std::shared_ptr<ServerConfig> config = fromData(clusterName_,
                                                  metaDataLogsConfig_,
                                                  principalsConfig_,
                                                  securityConfig_,
                                                  trafficShapingConfig_,
                                                  readIOShapingConfig_,
                                                  serverSettingsConfig_,
                                                  std::move(client_settings),
                                                  internalLogs_,
                                                  getClusterCreationTime(),
                                                  getCustomFields(),
                                                  ns_delimiter_);
  config->setVersion(version_);
  config->setMainConfigMetadata(main_config_metadata_);
  return config;
}

std::shared_ptr<ServerConfig> ServerConfig::createEmpty() {
  return fromData(
      std::string(),
      MetaDataLogsConfig(),
      PrincipalsConfig(),
      SecurityConfig(),
      TrafficShapingConfig(),
      ShapingConfig(std::set<NodeLocationScope>{NodeLocationScope::NODE},
                    std::set<NodeLocationScope>{NodeLocationScope::NODE}),
      SettingsConfig(),
      SettingsConfig(),
      InternalLogs(),
      OptionalTimestamp(),
      folly::dynamic::object());
}

const std::string ServerConfig::toString(const LogsConfig* with_logs,
                                         const ZookeeperConfig* with_zk,
                                         bool compress) const {
  // Grab the lock and initialize the cached result if this is the first call
  // to toString()
  std::lock_guard<std::mutex> guard(to_string_cache_mutex_);

  // Normally LogsConfig::getVersion() uniquely defines the contents of the
  // logs config, so we can use cached toString() result if version matches.
  // However, unit tests may modify LocalLogsConfig in place without changing
  // version. In this case we shouldn't use cache.
  auto local_logs_config =
      dynamic_cast<const configuration::LocalLogsConfig*>(with_logs);
  bool no_cache = local_logs_config && local_logs_config->wasModifiedInPlace();

  if (with_logs) {
    uint64_t logs_config_version = with_logs->getVersion();
    if (logs_config_version != last_to_string_logs_config_version_ ||
        no_cache) {
      // Clear the cache for the full config if the LogsConfig has changed
      last_to_string_logs_config_version_ = LSN_INVALID;
      all_to_string_cache_.clear();
      compressed_all_to_string_cache_.clear();
    }
  }

  std::string uncached_config_str;
  std::string& config_str = no_cache
      ? uncached_config_str
      : with_logs ? all_to_string_cache_ : main_to_string_cache_;
  if (config_str.empty()) {
    config_str = toStringImpl(with_logs, with_zk);
  }
  ld_check(!config_str.empty());

  if (!compress) {
    return config_str;
  }

  std::string uncached_compressed_config_str;
  std::string& compressed_config_str = no_cache
      ? uncached_compressed_config_str
      : with_logs ? compressed_all_to_string_cache_
                  : compressed_main_to_string_cache_;
  if (compressed_config_str.empty()) {
    using folly::IOBuf;
    std::unique_ptr<IOBuf> input =
        IOBuf::wrapBuffer(config_str.data(), config_str.size());
    auto codec = folly::io::getCodec(folly::io::CodecType::GZIP);
    std::unique_ptr<IOBuf> compressed;
    try {
      compressed = codec->compress(input.get());
    } catch (const std::invalid_argument& ex) {
      ld_error("gzip compression of config failed");
      return compressed_config_str;
    }
    compressed_config_str = compressed->moveToFbString().toStdString();
  }
  return compressed_config_str;
}

std::string ServerConfig::toStringImpl(const LogsConfig* with_logs,
                                       const ZookeeperConfig* with_zk) const {
  auto json = toJson(with_logs, with_zk);

  folly::json::serialization_opts opts;
  opts.pretty_formatting = true;
  opts.sort_keys = true;
  return folly::json::serialize(json, opts);
}

folly::dynamic ServerConfig::toJson(const LogsConfig* with_logs,
                                    const ZookeeperConfig* with_zk) const {
  folly::dynamic meta_nodeset = folly::dynamic::array;
  for (auto index : metaDataLogsConfig_.metadata_nodes) {
    meta_nodeset.push_back(index);
  }

  folly::dynamic json_all = folly::dynamic::object("cluster", clusterName_)(
      "version", version_.val())("internal_logs", internalLogs_.toDynamic())(
      "principals", principalsConfig_.toFollyDynamic())(
      "read_throttling", readIOShapingConfig_.toFollyDynamic())(
      "traffic_shaping", trafficShapingConfig_.toFollyDynamic())(
      "server_settings", folly::toDynamic(serverSettingsConfig_))(
      "client_settings", folly::toDynamic(clientSettingsConfig_));

  // In the NCM world, the metadata_logs section won't exit. Let's only
  // serialize it if it exists.
  if (getMetaDataLogGroup() != nullptr) {
    folly::dynamic metadata_logs =
        getMetaDataLogGroupInDir().toFollyDynamic(true /*is_metadata*/);

    metadata_logs["nodeset"] = meta_nodeset;
    metadata_logs["nodeset_selector"] =
        NodeSetSelectorTypeToString(metaDataLogsConfig_.nodeset_selector_type);
    metadata_logs["sequencers_write_metadata_logs"] =
        metaDataLogsConfig_.sequencers_write_metadata_logs;
    metadata_logs["sequencers_provision_epoch_store"] =
        metaDataLogsConfig_.sequencers_provision_epoch_store;
    auto& metadata_version = metaDataLogsConfig_.metadata_version_to_write;
    if (metadata_version.has_value()) {
      metadata_logs["metadata_version"] = metadata_version.value();
    }

    json_all["metadata_logs"] = std::move(metadata_logs);
  }

  if (clusterCreationTime_.has_value()) {
    json_all["cluster_creation_time"] = clusterCreationTime_.value().count();
  }
  if (with_logs != nullptr) {
    json_all["logs"] = with_logs->toJson();
  }
  if (ns_delimiter_ != LogsConfig::default_namespace_delimiter_) {
    json_all["log_namespace_delimiter"] = ns_delimiter_;
  }
  // Authentication Information is optional
  if (securityConfig_.securityOptionsEnabled()) {
    json_all["security_information"] = securityConfig_.toFollyDynamic();
  }

  if (with_zk) {
    json_all["zookeeper"] = with_zk->toFollyDynamic();
  }

  // insert custom fields
  for (auto& pair : customFields_.items()) {
    json_all[pair.first] = pair.second;
  }

  return json_all;
}

}} // namespace facebook::logdevice
