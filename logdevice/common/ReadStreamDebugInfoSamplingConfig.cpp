/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "logdevice/common/ReadStreamDebugInfoSamplingConfig.h"

#include <functional>

#include <folly/Synchronized.h>

#include "logdevice/common/ConfigSourceLocationParser.h"
#include "logdevice/common/ThriftCodec.h"
#include "logdevice/common/client_read_stream/AllClientReadStreams.h"
#include "logdevice/common/debug.h"

namespace {
template <typename Func>
struct AllReadStreamsDebugCallback
    : public facebook::logdevice::ConfigSource::AsyncCallback {
  explicit AllReadStreamsDebugCallback(Func cb) : callback_(cb) {}

  virtual void
  onAsyncGet(facebook::logdevice::ConfigSource* /* source */,
             const std::string& /* path */,
             facebook::logdevice::Status status,
             facebook::logdevice::ConfigSource::Output output) override {
    callback_(status, std::move(output));
  }

  Func callback_;
};

template <typename F>
std::unique_ptr<AllReadStreamsDebugCallback<F>> createCallback(F func) {
  return std::make_unique<AllReadStreamsDebugCallback<F>>(func);
}

} // namespace

namespace facebook { namespace logdevice {
ReadStreamDebugInfoSamplingConfig::ReadStreamDebugInfoSamplingConfig(
    std::shared_ptr<PluginRegistry> plugin_registry,
    const std::string& all_read_streams_debug_config_path)
    : all_read_streams_debug_cb_(createCallback(
          std::bind(&ReadStreamDebugInfoSamplingConfig::updateCallback,
                    this,
                    std::placeholders::_1,
                    std::placeholders::_2))) {
  std::vector<std::unique_ptr<ConfigSource>> sources;
  auto factories = plugin_registry->getMultiPlugin<ConfigSourceFactory>(
      PluginType::CONFIG_SOURCE_FACTORY);
  for (const auto& f : factories) {
    std::vector<std::unique_ptr<ConfigSource>> srcs = (*f)(plugin_registry);
    for (auto& src : srcs) {
      sources.emplace_back(std::move(src));
    }
  }

  // Determine which ConfigSource to use.
  auto src = ConfigSourceLocationParser::parse(
      sources, all_read_streams_debug_config_path);
  if (src.first == sources.end()) {
    return;
  }

  source_ = std::move(*src.first);
  auto& path = src.second;
  source_->setAsyncCallback(all_read_streams_debug_cb_.get());

  std::string config_str;
  ConfigSource::Output out;
  auto status = source_->getConfig(path, &out);
  updateCallback(status, out);
}

void ReadStreamDebugInfoSamplingConfig::setUpdateCallback(
    Callback&& updateCallback) {
  auto configWLock = configs_.wlock();
  updateCallback_ = std::move(updateCallback);
  auto configRLock = configWLock.moveFromWriteToRead();
  if (*configRLock) {
    updateCallback_(**configRLock);
  }
}

void ReadStreamDebugInfoSamplingConfig::unsetUpdateCallback() {
  auto configWLock = configs_.wlock();
  updateCallback_ = [](auto&&...) {};
}

void ReadStreamDebugInfoSamplingConfig::updateCallback(
    Status status,
    ConfigSource::Output out) {
  if (status != Status::OK) {
    return;
  }
  ld_debug("all read streams debugging config fetched.");
  // parse output as JSON string
  auto deserialized_configs =
      ThriftCodec::deserialize<apache::thrift::SimpleJSONSerializer,
                               configuration::all_read_streams_debug_config::
                                   thrift::AllReadStreamsDebugConfigs>(
          Slice::fromString(out.contents));
  if (deserialized_configs == nullptr) {
    ld_error(
        "Unable to deserialize fetched All Read Streams Debugging Config.");
  }
  configs_.exchange(std::move(deserialized_configs));
  auto configs = configs_.rlock();
  updateCallback_(**configs);
}
}} // namespace facebook::logdevice
