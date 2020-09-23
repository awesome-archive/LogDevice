/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include <functional>
#include <memory>

#include <folly/io/async/EventBaseThread.h>

#include "logdevice/common/WorkerType.h"
#include "logdevice/server/ServerSettings.h"

namespace facebook { namespace logdevice {

class ConnectionListener;
class EventLoop;
class EventLogStateMachine;
class Listener;
class LogStoreMonitor;
class Processor;
class RebuildingCoordinator;
class RebuildingSupervisor;
class ServerProcessor;
class ShardedStorageThreadPool;
class ShardedRocksDBLocalLogStore;
class SequencerPlacement;
class LogDeviceThriftServer;
class UnreleasedRecordDetector;
class Worker;

namespace maintenance {
class ClusterMaintenanceStateMachine;
}

/**
 * Function that orchestrates server shutdown. Performs the following sequence
 * of actions:
 *
 *   1. If fast_shutdown is set to false, call requestFailover on the
 *      SequencerPlacement object to failover currently handled shards to a
 *      different server.
 *   2. Stop admitting new requests into API and admin Thrift servers
 *   3. Destroys ConnectionListener, CommandListener, GossipListener,
 *      SSL connection and command listeners to stop accepting new
 *      connections.
 *   4. accepting_work_ is set to false on all Workers. This prevents worker
 *      threads from taking new work.
 *   5. ShardedStorageThreadPool's shutdown() method is called. All queued tasks
 *      are processed before threads exit.
 *   6. Waits until all workers complete any Requests they may still have
 *      that are active.
 *   7. Finally, destroys the Processor object, taking down all worker threads.
 *
 */
void shutdown_server(
    std::unique_ptr<LogDeviceThriftServer>& admin_server,
    std::unique_ptr<LogDeviceThriftServer>& s2s_thrift_api_server,
    std::unique_ptr<LogDeviceThriftServer>& c2s_thrift_api_server,
    std::unique_ptr<Listener>& connection_listener,
    std::map<ServerSettings::ClientNetworkPriority, std::unique_ptr<Listener>>&
        listeners_per_priority,
    std::unique_ptr<Listener>& gossip_listener,
    std::unique_ptr<Listener>& ssl_connection_listener,
    std::unique_ptr<Listener>& server_to_server_listener,
    std::unique_ptr<folly::EventBaseThread>& connection_listener_loop,
    std::unique_ptr<folly::EventBaseThread>& gossip_listener_loop,
    std::unique_ptr<folly::EventBaseThread>& server_to_server_listener_loop,
    std::unique_ptr<LogStoreMonitor>& logstore_monitor,
    std::shared_ptr<ServerProcessor>& processor,
    std::unique_ptr<ShardedStorageThreadPool>& storage_thread_pool,
    std::unique_ptr<ShardedRocksDBLocalLogStore>& sharded_store,
    std::shared_ptr<SequencerPlacement> sequencer_placement,
    std::unique_ptr<RebuildingCoordinator>& rebuilding_coordinator,
    std::unique_ptr<EventLogStateMachine>& event_log,
    std::unique_ptr<RebuildingSupervisor>& rebuilding_supervisor,
    std::shared_ptr<UnreleasedRecordDetector>& unreleased_record_detector,
    std::unique_ptr<maintenance::ClusterMaintenanceStateMachine>&
        cluster_maintenance_state_machine,
    bool fast_shutdown,
    uint64_t& shutdown_duration_ms);

/**
 * Helper function to post a request with a semaphore on workers with the
 * supplied worker_type and wait until they all complete.
 *
 * @return The number of workers that successfully handled the request.
 */
int post_and_wait(Processor* processor,
                  std::function<bool(WorkerType worker_type)> filter,
                  std::function<void(Worker* worker)> callback);

/**
 * Same as above except that this runs on all workers.
 */
int post_and_wait(Processor* processor,
                  std::function<void(Worker* worker)> callback);
}} // namespace facebook::logdevice
