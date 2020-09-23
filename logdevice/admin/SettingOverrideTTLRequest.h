/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>

#include "logdevice/common/Request.h"
#include "logdevice/common/Worker.h"
#include "logdevice/common/debug.h"
#include "logdevice/common/settings/SettingsUpdater.h"
#include "logdevice/include/Err.h"

namespace facebook { namespace logdevice {

class SettingOverrideTTLRequest;

/**
 * @file SettingOverrideTTLRequest.h
 *
 * This request will unset the setting when ttl expires. If there is already a
 * timer running, the new timer will override the old one.
 */
class SettingOverrideTTLRequest : public Request {
 public:
  explicit SettingOverrideTTLRequest(
      std::chrono::microseconds ttl,
      std::string name,
      std::shared_ptr<SettingsUpdater> settings_updater)
      : ttl_(ttl), name_(name), settings_updater_(settings_updater) {}

  void onTimeout();

  void setupTimer();

  Request::Execution execute() override;

  int getThreadAffinity(int /*nthreads*/) override;

 protected:
  void registerRequest();

  void destroy();

 private:
  Timer timer_;
  std::chrono::microseconds ttl_;
  std::string name_;
  std::shared_ptr<SettingsUpdater> settings_updater_;
};

}} // namespace facebook::logdevice
