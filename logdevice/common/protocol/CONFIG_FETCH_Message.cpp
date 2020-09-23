/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "logdevice/common/protocol/CONFIG_FETCH_Message.h"

#include <folly/Memory.h>

#include "logdevice/common/Processor.h"
#include "logdevice/common/Sender.h"
#include "logdevice/common/Worker.h"
#include "logdevice/common/configuration/Configuration.h"
#include "logdevice/common/configuration/nodes/NodesConfigurationCodec.h"
#include "logdevice/common/protocol/CONFIG_CHANGED_Message.h"

namespace facebook { namespace logdevice {

void CONFIG_FETCH_Header::serialize(ProtocolWriter& writer) const {
  if (writer.proto() >=
      Compatibility::ProtocolVersion::RID_IN_CONFIG_MESSAGES) {
    writer.write(rid);
    writer.write(my_version);
  }
  writer.write(config_type);
}

CONFIG_FETCH_Header CONFIG_FETCH_Header::deserialize(ProtocolReader& reader) {
  request_id_t rid;
  uint64_t my_version = 0;
  CONFIG_FETCH_Header::ConfigType config_type;

  if (reader.proto() >=
      Compatibility::ProtocolVersion::RID_IN_CONFIG_MESSAGES) {
    reader.read(&rid);
    reader.read(&my_version);
  }

  reader.read(&config_type);

  return CONFIG_FETCH_Header{
      rid,
      config_type,
      my_version,
  };
}

void CONFIG_FETCH_Message::serialize(ProtocolWriter& writer) const {
  header_.serialize(writer);
}

MessageReadResult CONFIG_FETCH_Message::deserialize(ProtocolReader& reader) {
  CONFIG_FETCH_Message message;
  message.header_ = CONFIG_FETCH_Header::deserialize(reader);
  return reader.result([message = std::move(message)] {
    return new CONFIG_FETCH_Message(message);
  });
}

bool CONFIG_FETCH_Message::isCallerWaitingForCallback() const {
  return header_.rid != REQUEST_ID_INVALID;
}

Message::Disposition CONFIG_FETCH_Message::onReceived(const Address& from) {
  RATELIMIT_INFO(std::chrono::seconds(20),
                 1,
                 "CONFIG_FETCH messages received. E.g., from %s",
                 Sender::describeConnection(from).c_str());

  switch (header_.config_type) {
    case CONFIG_FETCH_Header::ConfigType::LOGS_CONFIG:
      return handleLogsConfigRequest(from);
    case CONFIG_FETCH_Header::ConfigType::MAIN_CONFIG:
      return handleMainConfigRequest(from);
    case CONFIG_FETCH_Header::ConfigType::NODES_CONFIGURATION:
      return handleNodesConfigurationRequest(from);
  }

  ld_error("Received a CONFIG_FETCH message with an unknown ConfigType: %u. "
           "Ignoring it.",
           static_cast<uint8_t>(header_.config_type));
  return Disposition::ERROR;
}

Message::Disposition
CONFIG_FETCH_Message::handleMainConfigRequest(const Address& from) {
  RATELIMIT_ERROR(std::chrono::seconds(10),
                  1,
                  "CONFIG_FETCH for the main config is no longer supported.");
  auto msg = std::make_unique<CONFIG_CHANGED_Message>(
      CONFIG_CHANGED_Header(E::NOTSUPPORTED));
  sendMessage(std::move(msg), from);
  return Disposition::ERROR;
}

Message::Disposition
CONFIG_FETCH_Message::handleLogsConfigRequest(const Address& from) {
  ld_error("CONFIG_FETCH for logs config is currently not supported.");
  err = E::BADMSG;
  auto bad_msg =
      std::make_unique<CONFIG_CHANGED_Message>(CONFIG_CHANGED_Header(err));

  int rv = sendMessage(std::move(bad_msg), from);
  if (rv != 0) {
    ld_error("Sending CONFIG_CHANGED_Message to %s failed with error %s",
             from.toString().c_str(),
             error_description(err));
  }
  return Disposition::ERROR;
}

Message::Disposition
CONFIG_FETCH_Message::handleNodesConfigurationRequest(const Address& from) {
  auto nodes_cfg = getNodesConfiguration();
  ld_check(nodes_cfg);

  CONFIG_CHANGED_Header hdr{
      Status::OK,
      header_.rid,
      static_cast<uint64_t>(
          nodes_cfg->getLastChangeTimestamp().time_since_epoch().count()),
      nodes_cfg->getVersion(),
      getMyNodeID(),
      CONFIG_CHANGED_Header::ConfigType::NODES_CONFIGURATION,
      isCallerWaitingForCallback() ? CONFIG_CHANGED_Header::Action::CALLBACK
                                   : CONFIG_CHANGED_Header::Action::UPDATE};

  std::unique_ptr<CONFIG_CHANGED_Message> msg;
  if (nodes_cfg->getVersion().val() <= header_.my_version) {
    // The requester already have an up to date version.
    hdr.status = Status::UPTODATE;
    msg = std::make_unique<CONFIG_CHANGED_Message>(hdr, "");
  } else {
    auto serialized = nodes_cfg->serialize();
    if (!serialized) {
      // Failed to serialize configuration, the details should have been logged
      // already
      return Disposition::NORMAL;
    }
    msg = std::make_unique<CONFIG_CHANGED_Message>(hdr, std::move(*serialized));
  }

  int rv = sendMessage(std::move(msg), from);
  if (rv != 0) {
    RATELIMIT_ERROR(std::chrono::seconds(10),
                    1,
                    "Sending CONFIG_CHANGED_Message to %s failed with error %s",
                    from.toString().c_str(),
                    error_description(err));
  }
  return Disposition::NORMAL;
}

NodeID CONFIG_FETCH_Message::getMyNodeID() const {
  return Worker::onThisThread()->processor_->getMyNodeID();
}

std::shared_ptr<const configuration::nodes::NodesConfiguration>
CONFIG_FETCH_Message::getNodesConfiguration() {
  return Worker::onThisThread()->getNodesConfiguration();
}

int CONFIG_FETCH_Message::sendMessage(
    std::unique_ptr<CONFIG_CHANGED_Message> msg,
    const Address& to) {
  return Worker::onThisThread()->sender().sendMessage(std::move(msg), to);
}

}} // namespace facebook::logdevice
