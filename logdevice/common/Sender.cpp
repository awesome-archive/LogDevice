/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "logdevice/common/Sender.h"

#include <chrono>
#include <stdexcept>
#include <unordered_map>

#include <folly/DynamicConverter.h>
#include <folly/Optional.h>
#include <folly/Random.h>
#include <folly/container/F14Map.h>
#include <folly/json.h>

#include "logdevice/common/AdminCommandTable.h"
#include "logdevice/common/ClientIdxAllocator.h"
#include "logdevice/common/Connection.h"
#include "logdevice/common/ConstructorFailed.h"
#include "logdevice/common/FlowGroup.h"
#include "logdevice/common/Processor.h"
#include "logdevice/common/ResourceBudget.h"
#include "logdevice/common/ShapingContainer.h"
#include "logdevice/common/Sockaddr.h"
#include "logdevice/common/SocketCallback.h"
#include "logdevice/common/SocketDependencies.h"
#include "logdevice/common/Worker.h"
#include "logdevice/common/configuration/ServerConfig.h"
#include "logdevice/common/configuration/nodes/ServerAddressRouter.h"
#include "logdevice/common/configuration/nodes/utils.h"
#include "logdevice/common/debug.h"
#include "logdevice/common/network/IConnectionFactory.h"
#include "logdevice/common/protocol/CONFIG_CHANGED_Message.h"
#include "logdevice/common/protocol/Message.h"
#include "logdevice/common/protocol/MessageDispatch.h"
#include "logdevice/common/settings/Settings.h"
#include "logdevice/common/stats/ServerHistograms.h"
#include "logdevice/common/stats/Stats.h"
#include "logdevice/common/util.h"

namespace facebook { namespace logdevice {

using steady_clock = std::chrono::steady_clock;
using namespace configuration::nodes;

namespace {

// An object of this functor class is registered with every
// client Connection  managed by this Sender and is called when the
// Connection  closes.
class DisconnectedClientCallback : public SocketCallback {
 public:
  explicit DisconnectedClientCallback(Sender* sender) : sender_(sender) {}
  // calls noteDisconnectedClient() of the current thread's Sender
  void operator()(Status st, const Address& name) override;

 private:
  Sender* sender_;
};

} // namespace

namespace admin_command_table {
template <>
std::string Converter<ClientID>::operator()(ClientID client,
                                            bool /* prettify */) {
  return Sender::describeConnection(Address(client));
}

template <>
std::string Converter<Address>::operator()(Address address,
                                           bool /* prettify */) {
  return Sender::describeConnection(address);
}
} // namespace admin_command_table

class SenderImpl {
 public:
  explicit SenderImpl(ClientIdxAllocator* client_id_allocator)
      : client_id_allocator_(client_id_allocator) {}

  // a map of all Connections that have been created on this Worker thread
  // in order to talk to LogDevice servers. sendMessage() to the node_index_t or
  // NodeID of that Connection will try to reconnect. The rate of reconnection
  // attempts is controlled by a ConnectionThrottle.
  folly::F14NodeMap<node_index_t, std::unique_ptr<Connection>> server_conns_;

  // a map of all Connections wrapping connections that were accepted from
  // clients, keyed by 32-bit client ids. This map is empty on clients.
  folly::F14NodeMap<ClientID, std::unique_ptr<Connection>, ClientID::Hash>
      client_conns_;

  ClientIdxAllocator* client_id_allocator_;

  // Starts cleanup_connections_timer_ if has not been started yet
  void ensureCleanupTimerActive(Sender& sender) {
    if (!cleanup_connections_timer_.isActive()) {
      auto prev_context = Worker::packRunContext();
      cleanup_connections_timer_.setCallback(
          [&sender]() { sender.cleanupConnections(); });
      cleanup_connections_timer_.activate(
          Worker::settings().socket_health_check_period);
      Worker::unpackRunContext(prev_context);
    }
  }

  Timer cleanup_connections_timer_;
};

bool SenderProxy::canSendToImpl(const Address& addr,
                                TrafficClass tc,
                                BWAvailableCallback& on_bw_avail) {
  auto w = Worker::onThisThread();
  return w->sender().canSendToImpl(addr, tc, on_bw_avail);
}

int SenderProxy::sendMessageImpl(std::unique_ptr<Message>&& msg,
                                 const Address& addr,
                                 BWAvailableCallback* on_bw_avail,
                                 SocketCallback* onclose) {
  auto w = Worker::onThisThread();
  return w->sender().sendMessageImpl(
      std::move(msg), addr, on_bw_avail, onclose);
}

void SenderBase::MessageCompletion::send() {
  auto prev_context = Worker::packRunContext();
  RunContext run_context(msg_->type_);
  const auto w = Worker::onThisThread();
  w->onStartedRunning(run_context);
  w->message_dispatch_->onSent(*msg_, status_, destination_, enqueue_time_);
  msg_.reset(); // count destructor as part of message's execution time
  w->onStoppedRunning(run_context);
  Worker::unpackRunContext(prev_context);
}

Sender::Sender(Worker* worker,
               Processor* processor,
               std::shared_ptr<const Settings> settings,
               struct event_base* base,
               const configuration::ShapingConfig& tsc,
               ClientIdxAllocator* client_id_allocator,
               bool is_gossip_sender,
               std::shared_ptr<const NodesConfiguration> nodes,
               node_index_t my_index,
               folly::Optional<NodeLocation> my_location,
               std::unique_ptr<IConnectionFactory> connection_factory,
               StatsHolder* stats)
    : worker_(worker),
      processor_(processor),
      settings_(settings),
      connection_factory_(std::move(connection_factory)),
      impl_(new SenderImpl(client_id_allocator)),
      is_gossip_sender_(is_gossip_sender),
      nodes_(std::move(nodes)),
      my_node_index_(my_index),
      my_location_(std::move(my_location)) {
  nw_shaping_container_ = std::make_unique<ShapingContainer>(
      static_cast<size_t>(NodeLocationScope::ROOT) + 1,
      base,
      tsc,
      std::make_shared<NwShapingFlowGroupDeps>(stats, this));

  auto scope = NodeLocationScope::NODE;
  for (auto& fg : nw_shaping_container_->flow_groups_) {
    fg.setScope(scope);
    scope = NodeLocation::nextGreaterScope(scope);
  }
}

Sender::~Sender() {
  deliverCompletedMessages();
}

void Sender::onCompletedMessagesAvailable(void* arg, short) {
  Sender* self = reinterpret_cast<Sender*>(arg);
  self->deliverCompletedMessages();
}

int Sender::addClient(int fd,
                      const Sockaddr& client_addr,
                      ResourceBudget::Token conn_token,
                      SocketType type,
                      ConnectionType conntype,
                      ConnectionKind connection_kind) {
  if (shutting_down_) {
    ld_check(false); // listeners are shut down before Senders.
    ld_error("Sender is shut down");
    err = E::SHUTDOWN;
    return -1;
  }

  eraseDisconnectedClients();
  impl_->ensureCleanupTimerActive(*this);

  ClientID client_name(impl_->client_id_allocator_->issueClientIdx(
      worker_->worker_type_, worker_->idx_));

  try {
    // Until we have better information (e.g. in a future update to the
    // HELLO message), assume clients are within our region unless they
    // have connected via SSL.
    NodeLocationScope flow_group_scope = conntype == ConnectionType::SSL
        ? NodeLocationScope::ROOT
        : NodeLocationScope::REGION;

    auto& flow_group = nw_shaping_container_->selectFlowGroup(flow_group_scope);

    auto conn = connection_factory_->createConnection(
        fd,
        client_name,
        client_addr,
        std::move(conn_token),
        type,
        conntype,
        flow_group,
        std::make_unique<SocketDependencies>(processor_, this),
        connection_kind);

    auto res = impl_->client_conns_.emplace(client_name, std::move(conn));

    if (!res.second) {
      ld_critical("INTERNAL ERROR: attempt to add client %s (%s) that is "
                  "already in the client map",
                  client_name.toString().c_str(),
                  client_addr.toString().c_str());
      ld_check(0);
      err = E::EXISTS;
      return -1;
    }

    auto* cb = new DisconnectedClientCallback(this);
    // self-managed, destroyed by own operator()

    int rv = res.first->second->pushOnCloseCallback(*cb);

    if (rv != 0) { // unlikely
      ld_check(false);
      delete cb;
    }
  } catch (const ConstructorFailed&) {
    ld_error("Failed to construct a client Connection: error %d (%s)",
             static_cast<int>(err),
             error_description(err));
    return -1;
  }

  return 0;
}

void Sender::noteBytesQueued(size_t nbytes,
                             PeerType peer_type,
                             folly::Optional<MessageType> message_type) {
  // nbytes cannot exceed maximum message size
  ld_check(nbytes <= (size_t)Message::MAX_LEN + sizeof(ProtocolHeader));
  incrementBytesPending(nbytes, peer_type);
  StatsHolder* stats = Worker::stats();
  STAT_ADD(stats, sockets_bytes_pending_total, nbytes);
  STAT_ADD(stats, sockets_bytes_pending_max_worker, nbytes);
  if (message_type.has_value()) {
    MESSAGE_TYPE_STAT_ADD(
        stats, message_type.value(), message_bytes_pending, nbytes);
  }
}

void Sender::noteBytesDrained(size_t nbytes,
                              PeerType peer_type,
                              folly::Optional<MessageType> message_type) {
  decrementBytesPending(nbytes, peer_type);
  StatsHolder* stats = Worker::stats();
  STAT_SUB(stats, sockets_bytes_pending_total, nbytes);
  STAT_SUB(stats, sockets_bytes_pending_max_worker, nbytes);
  if (message_type.has_value()) {
    MESSAGE_TYPE_STAT_SUB(
        stats, message_type.value(), message_bytes_pending, nbytes);
  }
}

ssize_t Sender::getTcpSendBufSizeForClient(ClientID client_id) const {
  auto it = impl_->client_conns_.find(client_id);
  if (it == impl_->client_conns_.end()) {
    ld_error("client Connection %s not found", client_id.toString().c_str());
    ld_check(false);
    return -1;
  }
  return it->second->getTcpSendBufSize();
}

ssize_t Sender::getTcpSendBufOccupancyForClient(ClientID client_id) const {
  auto it = impl_->client_conns_.find(client_id);
  if (it == impl_->client_conns_.end()) {
    return -1;
  }
  return it->second->getTcpSendBufOccupancy();
}

void Sender::eraseDisconnectedClients() {
  auto prev = disconnected_clients_.before_begin();
  for (auto it = disconnected_clients_.begin();
       it != disconnected_clients_.end();) {
    const ClientID& cid = *it;
    const auto pos = impl_->client_conns_.find(cid);
    ld_assert(pos != impl_->client_conns_.end());
    ld_assert(pos->second->isClosed());
    ++it;
    // Check if there are users that still have reference to the Connection.
    // If yes, wait for the clients to drop the Connection instead of
    // reclaiming it here.
    if (!pos->second->isZombie()) {
      impl_->client_conns_.erase(pos);
      impl_->client_id_allocator_->releaseClientIdx(cid);
      disconnected_clients_.erase_after(prev);
      STAT_DECR(Worker::stats(), client_connection_close_backlog);
    } else {
      ++prev;
    }
  }
}

bool Sender::canSendToImpl(const Address& addr,
                           TrafficClass tc,
                           BWAvailableCallback& on_bw_avail) {
  ld_check(!on_bw_avail.active());

  Connection* conn = findConnection(addr);

  if (!conn) {
    if (addr.isClientAddress()) {
      // With no current client connection, the send is
      // guaranteeed to fail.
      return false;
    } else if (err == E::NOTCONN) {
      // sendMessage() will attempt to connect to a node
      // if no connection to that node currently exists.
      // Since we can't know for sure whether or not a
      // message will be throttled until after we're
      // connected and know the scope for that connection,
      // say that the send is expected to succeed so the
      // caller attempts to send and a connection attempt
      // will be made.
      return true;
    }
    return false;
  }

  Priority p = PriorityMap::fromTrafficClass()[tc];

  auto lock = nw_shaping_container_->lock();
  if (!conn->flow_group_.canDrain(p)) {
    conn->flow_group_.push(on_bw_avail, p);
    err = E::CBREGISTERED;
    FLOW_GROUP_STAT_INCR(Worker::stats(), conn->flow_group_, cbregistered);
    return false;
  }

  return true;
}

int Sender::sendMessageImpl(std::unique_ptr<Message>&& msg,
                            const Address& addr,
                            BWAvailableCallback* on_bw_avail,
                            SocketCallback* onclose) {
  Connection* conn = getConnection(addr, *msg);
  if (!conn) {
    // err set by getConnection()
    return -1;
  }

  int rv = sendMessageImpl(std::move(msg), *conn, on_bw_avail, onclose);
  ld_check(rv != 0 ? (bool)msg : !msg); // take ownership on success only
  if (rv != 0) {
    bool no_warning =
        // Some messages are implemented to gracefully handle
        // PROTONOSUPPORT, avoid log spew for them
        err == E::PROTONOSUPPORT && !msg->warnAboutOldProtocol();
    if (!no_warning) {
      RATELIMIT_LEVEL(
          err == E::CBREGISTERED ? dbg::Level::SPEW : dbg::Level::WARNING,
          std::chrono::seconds(10),
          3,
          "Unable to send a message of type %s to %s: error %s",
          messageTypeNames()[msg->type_].c_str(),
          Sender::describeConnection(addr).c_str(),
          error_name(err));
    }
  }
  return rv;
}

int Sender::sendMessageImpl(std::unique_ptr<Message>&& msg,
                            Connection& conn,
                            BWAvailableCallback* on_bw_avail,
                            SocketCallback* onclose) {
  ld_check(!shutting_down_);

  /* verify that we only send allowed messages via gossip Connection */
  if (is_gossip_sender_) {
    if (!allowedOnGossipConnection(msg->type_)) {
      RATELIMIT_WARNING(std::chrono::seconds(1),
                        1,
                        "Unexpected msg type:%u",
                        static_cast<unsigned char>(msg->type_));
      ld_check(false);
      err = E::INTERNAL;
      return -1;
    }
  }

  auto peer_type = conn.getInfo().getPeerType();
  if (!isHandshakeMessage(msg->type_) &&
      // Return ENOBUFS error Sender's outbuf limit and the Connection's
      // minimum out buf limit is reached.
      bytesPendingLimitReached(peer_type) && conn.minOutBufLimitReached()) {
    RATELIMIT_WARNING(std::chrono::seconds(1),
                      10,
                      "ENOBUFS for Sender. Peer type: %s. "
                      "Current sender outbuf usage: %zu. "
                      "Current sender peer outbuf usage : %zu",
                      peerTypeToString(peer_type),
                      getBytesPending(),
                      getBytesPending(peer_type));
    err = E::NOBUFS;
    STAT_INCR(Worker::stats(), send_failed_nobufs);
    return -1;
  }

  auto envelope = conn.registerMessage(std::move(msg));
  if (!envelope) {
    ld_check(err == E::INTERNAL || err == E::NOBUFS || err == E::NOTCONN ||
             err == E::PROTONOSUPPORT || err == E::UNREACHABLE);
    return -1;
  }
  ld_check(!msg);

  if (onclose) {
    conn.pushOnCloseCallback(*onclose);
  }

  auto lock = nw_shaping_container_->lock();

  Status error_to_inject = Worker::settings().message_error_injection_status;
  if (UNLIKELY(error_to_inject == E::DROPPED) &&
      envelope->message().type_ != MessageType::HELLO &&
      envelope->message().type_ != MessageType::ACK &&
      folly::Random::randDouble01() <
          Worker::settings().message_error_injection_chance_percent / 100.) {
    // Leak the envelope.
    // It'll stay in Connection's pendingq_ and will never be sent.
    ld_debug("Dropping message %s of size %lu",
             messageTypeNames()[envelope->message().type_].c_str(),
             envelope->cost());
    return 0;
  }
  auto inject_shaping_event = [&]() -> bool {
    return error_to_inject == E::CBREGISTERED &&
        conn.flow_group_.injectShapingEvent(
            envelope->priority(),
            Worker::settings().message_error_injection_chance_percent);
  };

  if (!inject_shaping_event() && conn.flow_group_.drain(*envelope)) {
    lock.unlock();
    FLOW_GROUP_STAT_INCR(Worker::stats(), conn.flow_group_, direct_dispatched);
    // Note: Some errors can only be detected during message
    // serialization.
    //       If this occurs just after releaseMessage() and the onSent()
    //       handler for the message responds to the error by queuing
    //       another message, Sender::sendMessage() can be reentered.
    conn.releaseMessage(*envelope);
    return 0;
  }

  // Message has hit a traffic shaping limit.
  if (on_bw_avail == nullptr) {
    // Sender/FlowGroup will release the message once bandwidth is
    // available.
    FLOW_GROUP_STAT_INCR(Worker::stats(), conn.flow_group_, deferred);
    FLOW_GROUP_MSG_STAT_INCR(
        Worker::stats(), conn.flow_group_, &envelope->message(), deferred);
    conn.flow_group_.push(*envelope);
    return 0;
  }

  // Message retransmission is the responsibility of the caller.  Return
  // message ownership to them.
  msg = conn.discardEnvelope(*envelope);

  FLOW_GROUP_STAT_INCR(Worker::stats(), conn.flow_group_, discarded);
  FLOW_GROUP_MSG_STAT_INCR(Worker::stats(), conn.flow_group_, msg, discarded);
  FLOW_GROUP_STAT_INCR(Worker::stats(), conn.flow_group_, cbregistered);
  conn.flow_group_.push(*on_bw_avail, msg->priority());
  conn.pushOnBWAvailableCallback(*on_bw_avail);
  err = E::CBREGISTERED;
  return -1;
}

Connection* Sender::findServerConnection(node_index_t idx) const {
  ld_check(idx >= 0);

  auto it = impl_->server_conns_.find(idx);
  if (it == impl_->server_conns_.end()) {
    return nullptr;
  }

  auto c = it->second.get();
  ld_check(c);
  ld_check(!c->getInfo().peer_name.isClientAddress());
  ld_check(c->getInfo().peer_name.asNodeID().index() == idx);

  return c;
}

Connection* Sender::findClientConnection(const ClientID& client_id) const {
  auto it = impl_->client_conns_.find(client_id);
  if (it == impl_->client_conns_.end()) {
    return nullptr;
  }
  auto c = it->second.get();
  ld_check(c);
  ld_check(c->getInfo().peer_name.isClientAddress());
  ld_check(c->getInfo().peer_name.id_.client_ == client_id);
  return c;
}

Connection* Sender::findConnection(const Address& addr) const {
  return addr.isClientAddress() ? findClientConnection(addr.id_.client_)
                                : findServerConnection(addr.asNodeID().index());
}

folly::Optional<uint16_t>
Sender::getSocketProtocolVersion(node_index_t idx) const {
  const auto* conn = findServerConnection(idx);
  return conn != nullptr ? conn->getInfo().protocol : folly::none;
}

ClientID Sender::getOurNameAtPeer(node_index_t node_index) const {
  const auto* info = getConnectionInfo(Address(node_index));
  folly::Optional<ClientID> name = info ? info->our_name_at_peer : folly::none;
  return name.value_or(ClientID::INVALID);
}

void Sender::deliverCompletedMessages() {
  CompletionQueue moved_queue = std::move(completed_messages_);
  while (!moved_queue.empty()) {
    std::unique_ptr<MessageCompletion> completion(&moved_queue.front());
    moved_queue.pop_front();
    if (!shutting_down_) {
      completion->send();
    }
  }
}

void Sender::resetServerSocketConnectThrottle(node_index_t node_id) {
  auto conn = findServerConnection(node_id);
  if (conn != nullptr) {
    conn->resetConnectThrottle();
  }
}

void Sender::setPeerShuttingDown(node_index_t node_id) {
  auto conn = findServerConnection(node_id);
  if (conn != nullptr) {
    conn->setPeerShuttingDown();
  }
}

int Sender::registerOnConnectionClosed(const Address& addr,
                                       SocketCallback& cb) {
  Connection* conn;

  if (addr.isClientAddress()) {
    auto pos = impl_->client_conns_.find(addr.id_.client_);
    if (pos == impl_->client_conns_.end()) {
      err = E::NOTFOUND;
      return -1;
    }
    conn = pos->second.get();
    if (conn->isClosed()) {
      err = E::NOTFOUND;
      return -1;
    }
  } else { // addr is a server address
    conn = findServerConnection(addr.asNodeID().index());
    if (!conn) {
      err = E::NOTFOUND;
      return -1;
    }
  }

  return conn->pushOnCloseCallback(cb);
}

void Sender::flushOutputAndClose(Status reason) {
  auto open_socket_count = 0;
  for (const auto& it : impl_->server_conns_) {
    if (it.second && !it.second->isClosed()) {
      it.second->flushOutputAndClose(reason);
      ++open_socket_count;
    }
  }

  for (auto& it : impl_->client_conns_) {
    if (it.second && !it.second->isClosed()) {
      if (reason == E::SHUTDOWN) {
        it.second->sendShutdown();
      }
      it.second->flushOutputAndClose(reason);
      ++open_socket_count;
    }
  }

  ld_log(open_socket_count ? dbg::Level::INFO : dbg::Level::SPEW,
         "Number of open Connections : %d",
         open_socket_count);
}
int Sender::closeConnection(Address addr, Status reason) {
  if (addr.isClientAddress()) {
    return closeClientSocket(addr.asClientID(), reason);
  } else {
    return closeServerSocket(addr.asNodeID().index(), reason);
  }
}

int Sender::closeClientSocket(ClientID cid, Status reason) {
  ld_check(cid.valid());

  auto pos = impl_->client_conns_.find(cid);
  if (pos == impl_->client_conns_.end()) {
    err = E::NOTFOUND;
    return -1;
  }

  pos->second->close(reason);
  return 0;
}

int Sender::closeServerSocket(node_index_t peer, Status reason) {
  Connection* c = findServerConnection(peer);
  if (!c) {
    err = E::NOTFOUND;
    return -1;
  }

  if (!c->isClosed()) {
    c->close(reason);
  }

  return 0;
}

void Sender::closeAllSockets() {
  uint32_t server_sockets_closed = 0;
  for (auto& entry : impl_->server_conns_) {
    if (entry.second && !entry.second->isClosed()) {
      server_sockets_closed++;
      entry.second->close(E::SHUTDOWN);
    }
  }
  ld_info("Num server sockets closed: %u", server_sockets_closed);

  uint32_t client_sockets_closed = 0;
  for (auto& entry : impl_->client_conns_) {
    if (entry.second && !entry.second->isClosed()) {
      client_sockets_closed++;
      entry.second->close(E::SHUTDOWN);
    }
  }
  ld_info("Num clients sockets closed: %u", client_sockets_closed);
}

void Sender::beginShutdown() {
  shutting_down_ = true;
  flushOutputAndClose(E::SHUTDOWN);
}

void Sender::forceShutdown() {
  if (!shutting_down_) {
    ld_warning("Force shutdown of Sender w/o graceful shutdown attempt");
    shutting_down_ = true;
  }
  closeAllSockets();
  impl_->server_conns_.clear();
  impl_->client_conns_.clear();
}

bool Sender::isShutdownCompleted() const {
  // Go over all Connections at shutdown to find pending work. This could help
  // in figuring which Connections are slow in draining buffers.
  bool go_over_all_sockets = !worker_->isAcceptingWork();

  int num_open_server_sockets = 0;
  Connection* max_pending_work_server = nullptr;
  size_t server_with_max_pending_bytes = 0;
  for (const auto& it : impl_->server_conns_) {
    if (it.second && !it.second->isClosed()) {
      if (!go_over_all_sockets) {
        return false;
      }

      ++num_open_server_sockets;
      const auto conn = it.second.get();
      size_t pending_bytes = conn->getBytesPending();
      if (server_with_max_pending_bytes < pending_bytes) {
        max_pending_work_server = conn;
        server_with_max_pending_bytes = pending_bytes;
      }
    }
  }

  int num_open_client_sockets = 0;
  ClientID max_pending_work_clientID;
  Connection* max_pending_work_client = nullptr;
  size_t client_with_max_pending_bytes = 0;
  for (auto& it : impl_->client_conns_) {
    if (!it.second->isClosed()) {
      if (!go_over_all_sockets) {
        return false;
      }

      ++num_open_client_sockets;
      const auto conn = it.second.get();
      const size_t pending_bytes = conn->getBytesPending();
      if (client_with_max_pending_bytes < pending_bytes) {
        max_pending_work_client = conn;
        max_pending_work_clientID = it.first;
        client_with_max_pending_bytes = pending_bytes;
      }
    }
  }

  // None of the Connections are open return true.
  if (!num_open_client_sockets && !num_open_server_sockets) {
    return true;
  }

  RATELIMIT_INFO(
      std::chrono::seconds(5),
      5,
      "Connections still open: Server Connection count %d (max pending "
      "bytes: %lu in Connection 0x%p), Client Connection count %d (max "
      "pending bytes: %lu for client %s Connection 0x%p)",
      num_open_server_sockets,
      server_with_max_pending_bytes,
      (void*)max_pending_work_server,
      num_open_client_sockets,
      client_with_max_pending_bytes,
      max_pending_work_clientID.toString().c_str(),
      (void*)max_pending_work_client);

  return false;
}

bool Sender::isClosed(const Address& addr) const {
  if (addr.isClientAddress()) {
    auto pos = impl_->client_conns_.find(addr.id_.client_);
    if (pos == impl_->client_conns_.end() || pos->second->isClosed()) {
      return true;
    }
  } else { // addr is a server address
    Connection* conn = findServerConnection(addr.asNodeID().index());
    if (!conn || conn->isClosed()) {
      return true;
    }
  }
  return false;
}

int Sender::checkServerConnection(node_index_t node) const {
  Connection* c = findServerConnection(node);
  if (!c || c->getInfo().peer_name.asNodeID().index() != node) {
    err = E::NOTFOUND;
    return -1;
  }

  if (!c->isSSL() && useSSLWith(node)) {
    // We have a plaintext connection, but we need an encrypted one.
    err = E::SSLREQUIRED;
    return -1;
  }

  // check if the Connection to destination has reached its buffer limit
  if (c->sizeLimitsExceeded()) {
    err = E::NOBUFS;
    return -1;
  }

  return c->checkServerConnection();
}

int Sender::checkClientConnection(ClientID cid, bool check_peer_is_node) {
  ld_check(cid.valid());
  auto pos = impl_->client_conns_.find(cid);
  if (pos == impl_->client_conns_.end() || pos->second->isClosed()) {
    err = E::NOTFOUND;
    return -1;
  }

  if (!pos->second->isHandshaken()) {
    err = E::NOTCONN;
    return -1;
  }

  if (check_peer_is_node && pos->second->getInfo().isPeerClient()) {
    err = E::NOTANODE;
    return -1;
  }

  return 0;
}

int Sender::connect(node_index_t nid) {
  if (shutting_down_) {
    err = E::SHUTDOWN;
    return -1;
  }

  Connection* c = initServerConnection(NodeID(nid), SocketType::DATA);
  if (!c) {
    return -1;
  }

  return c->connect();
}

bool Sender::useSSLWith(node_index_t node_id,
                        bool* cross_boundary_out,
                        bool* authentication_out) const {
  // Determine whether we need to use SSL by comparing our location with the
  // location of the target node.
  bool cross_boundary = false;
  NodeLocationScope diff_level = settings_->ssl_boundary;

  cross_boundary = configuration::nodes::getNodeSSL(
      *nodes_, my_location_, node_id, diff_level);

  auto server_config = worker_->getServerConfig();
  // Determine whether we need to use an SSL Connection for authentication.
  bool authentication =
      (server_config->getAuthenticationType() == AuthenticationType::SSL);

  if (cross_boundary_out) {
    *cross_boundary_out = cross_boundary;
  }
  if (authentication_out) {
    *authentication_out = authentication;
  }

  return cross_boundary || authentication;
}

Connection* Sender::initServerConnection(NodeID nid, SocketType sock_type) {
  ld_check(!shutting_down_);
  const auto node_cfg = nodes_->getNodeServiceDiscovery(nid.index());

  // Don't try to connect if the node is not in config.
  // If the Connection was already connected but the node removed from config,
  // it will be closed once noteConfigurationChanged() executes.
  if (!node_cfg) {
    err = E::NOTINCONFIG;
    return nullptr;
  }

  auto it = impl_->server_conns_.find(nid.index());
  if (it != impl_->server_conns_.end()) {
    // for all connections :
    //     create new connection if the existing connection is closed.
    // for DATA connection:
    //     create new connection if the existing connection is not SSL but
    //     should be.
    // for GOSSIP connection:
    //     create new connection if the existing connection is not SSL but
    //     ssl_on_gossip_port is true or the existing connection is SSL but
    //     the ssl_on_gossip_port is false.
    const bool should_create_new = it->second->isClosed() ||
        (sock_type != SocketType::GOSSIP && !it->second->isSSL() &&
         useSSLWith(nid.index())) ||
        (it->second->isSSL() != Worker::settings().ssl_on_gossip_port &&
         sock_type == SocketType::GOSSIP);

    if (should_create_new) {
      // We have a plaintext connection, but now we need an encrypted one.
      // Scheduling this Connection to be closed and moving it out of
      // server_conns_ to initialize an SSL connection in its place.
      STAT_INCR(Worker::stats(), server_connection_close_backlog);
      worker_->add([s = std::move(it->second)] {
        if (s->good()) {
          s->close(E::SSLREQUIRED);
        }
        STAT_DECR(Worker::stats(), server_connection_close_backlog);
      });
      ld_check(!it->second);
      impl_->server_conns_.erase(it);
      it = impl_->server_conns_.end();
    }
  }

  if (it == impl_->server_conns_.end()) {
    impl_->ensureCleanupTimerActive(*this);
    // Don't try to connect if we expect a generation and it's different than
    // what is in the config.
    if (nid.generation() == 0) {
      nid = nodes_->getNodeID(nid.index());
    } else if (nodes_->getNodeGeneration(nid.index()) != nid.generation()) {
      err = E::NOTINCONFIG;
      return nullptr;
    }

    // Determine whether we should use SSL and what the flow group scope is
    auto flow_group_scope = NodeLocationScope::NODE;
    if (nid.index() != my_node_index_) {
      if (my_location_.has_value() && node_cfg->location) {
        flow_group_scope =
            my_location_->closestSharedScope(*node_cfg->location);
      } else {
        // Assume within the same region for now, since cross region should
        // use SSL and have a location specified in the config.
        flow_group_scope = NodeLocationScope::REGION;
      }
    }

    bool cross_boundary = false;
    bool ssl_authentication = false;
    bool use_ssl =
        useSSLWith(nid.index(), &cross_boundary, &ssl_authentication);
    if (sock_type == SocketType::GOSSIP) {
      ld_check(is_gossip_sender_);
      if (Worker::settings().send_to_gossip_port) {
        use_ssl = Worker::settings().ssl_on_gossip_port;
      }
    }

    try {
      auto& flow_group =
          nw_shaping_container_->selectFlowGroup(flow_group_scope);

      auto conn = connection_factory_->createConnection(
          nid,
          sock_type,
          use_ssl ? ConnectionType::SSL : ConnectionType::PLAIN,
          flow_group,
          std::make_unique<SocketDependencies>(processor_, this));

      auto res = impl_->server_conns_.emplace(nid.index(), std::move(conn));
      it = res.first;
    } catch (ConstructorFailed& exp) {
      ld_critical("Could not create server Connection to node %s sock_type %s "
                  "use_ssl %d. %s",
                  toString(nid).c_str(),
                  socketTypeToString(sock_type),
                  use_ssl,
                  exp.what());
      if (err == E::NOTINCONFIG || err == E::NOSSLCONFIG) {
        return nullptr;
      }
      ld_check(false);
      err = E::INTERNAL;
      return nullptr;
    }
  }

  ld_check(it->second != nullptr);
  return it->second.get();
}

Sockaddr Sender::getSockaddr(const Address& addr) {
  const auto* info = getConnectionInfo(addr);
  return info ? info->peer_address : Sockaddr::INVALID;
}

ConnectionType Sender::getSockConnType(const Address& addr) {
  const auto* info = getConnectionInfo(addr);
  return info ? info->connection_type : ConnectionType::NONE;
}

Connection* FOLLY_NULLABLE Sender::getConnection(const ClientID& cid) {
  if (shutting_down_) {
    err = E::SHUTDOWN;
    return nullptr;
  }

  auto pos = impl_->client_conns_.find(cid);
  if (pos == impl_->client_conns_.end()) {
    err = E::UNREACHABLE;
    return nullptr;
  }
  return pos->second.get();
}

Connection* FOLLY_NULLABLE Sender::getConnection(const NodeID& nid,
                                                 const Message& msg) {
  if (shutting_down_) {
    err = E::SHUTDOWN;
    return nullptr;
  }

  SocketType sock_type;
  if (is_gossip_sender_) {
    ld_check(allowedOnGossipConnection(msg.type_));
    sock_type = SocketType::GOSSIP;
  } else {
    sock_type = SocketType::DATA;
  }

  Connection* conn = initServerConnection(nid, sock_type);
  if (!conn) {
    // err set by initServerConnection()
    return nullptr;
  }

  int rv = conn->connect();

  if (rv != 0 && err != E::ALREADY && err != E::ISCONN) {
    // err can't be UNREACHABLE because sock must be a server Connection
    ld_check(err == E::UNROUTABLE || err == E::DISABLED || err == E::SYSLIMIT ||
             err == E::NOMEM || err == E::INTERNAL);
    return nullptr;
  }

  // conn is now connecting or connected, send msg
  ld_assert(conn->connect() == -1 && (err == E::ALREADY || err == E::ISCONN));
  return conn;
}
Connection* FOLLY_NULLABLE Sender::getConnection(const Address& addr,

                                                 const Message& msg) {
  return addr.isClientAddress() ? getConnection(addr.asClientID())
                                : getConnection(addr.asNodeID(), msg);
}

Connection* FOLLY_NULLABLE Sender::findConnection(const Address& addr) {
  if (addr.isClientAddress()) {
    // err, if any, set by getConnection().
    return getConnection(addr.asClientID());
  }

  Connection* conn = nullptr;
  NodeID nid = addr.asNodeID();
  auto it = impl_->server_conns_.find(nid.index());
  if (it != impl_->server_conns_.end()) {
    conn = it->second.get();
  }
  if (!conn) {
    err = E::NOTINCONFIG;
  }
  return conn;
}

const PrincipalIdentity* Sender::getPrincipal(const Address& address) {
  const auto* info = getConnectionInfo(address);
  return info ? info->principal.get() : nullptr;
}

folly::Optional<std::string> Sender::getCSID(const Address& addr) const {
  const auto* info = getConnectionInfo(addr);
  return info ? info->csid : folly::none;
}

std::string Sender::getClientLocation(const ClientID& cid) const {
  const auto* info = getConnectionInfo(Address(cid));
  folly::Optional<std::string> location =
      info ? info->client_location : folly::none;
  return location.value_or("");
}

const ConnectionInfo* FOLLY_NULLABLE
Sender::getConnectionInfo(const Address& address) const {
  const auto* connection = findConnection(address);
  return connection != nullptr ? &connection->getInfo() : nullptr;
}

bool Sender::setConnectionInfo(const Address& address,
                               const ConnectionInfo& new_info) {
  auto* connection = findConnection(address);
  if (connection == nullptr) {
    return false;
  }
  auto dscp_mark = detectDSCP(new_info);
  if (dscp_mark) {
    connection->setDSCP(dscp_mark.value());
  }
  connection->setInfo(new_info);
  return true;
}

folly::Optional<uint8_t> Sender::detectDSCP(const ConnectionInfo& info) {
  folly::Optional<uint8_t> dscp = folly::none;
  auto config = worker_->getServerConfig();
  // Only set DSCP on incoming connections
  if (info.peer_name.isClientAddress()) {
    // Try to find principal-specific settings for DSCP
    for (const auto& identity : info.principal->identities) {
      auto principal_cfg = config->getPrincipalByName(&identity.second);
      if (principal_cfg != nullptr && principal_cfg->egress_dscp != 0) {
        dscp = principal_cfg->egress_dscp;
        break;
      }
    }
    // Set default DSCP for incoming connection from server nodes
    if (!dscp.hasValue() && info.peer_node_idx) {
      dscp = settings_->server_dscp_default;
    }
  }
  return dscp;
}

std::pair<Sender::ExtractPeerIdentityResult, PrincipalIdentity>
Sender::extractPeerIdentity(const Address& addr) {
  auto connection = findConnection(addr);
  if (connection == nullptr) {
    return std::make_pair(Sender::ExtractPeerIdentityResult::NOT_FOUND,
                          PrincipalIdentity(Principal::UNAUTHENTICATED));
  }
  if (!connection->isSSL()) {
    return std::make_pair(Sender::ExtractPeerIdentityResult::NOT_SSL,
                          PrincipalIdentity(Principal::UNAUTHENTICATED));
  }
  auto identity = connection->extractPeerIdentity();
  if (!identity.has_value()) {
    return std::make_pair(Sender::ExtractPeerIdentityResult::PARSING_FAILED,
                          PrincipalIdentity(Principal::INVALID));
  }
  return std::make_pair(
      Sender::ExtractPeerIdentityResult::SUCCESS, std::move(identity).value());
}

/* static */
Sockaddr Sender::sockaddrOrInvalid(const Address& addr) {
  Worker* w = Worker::onThisThread(false);
  if (!w) {
    return Sockaddr();
  }
  return w->sender().getSockaddr(addr);
}

folly::Optional<node_index_t> Sender::getNodeIdx(const Address& addr) const {
  if (!addr.isClientAddress()) {
    return addr.id_.node_.index();
  }
  const auto* connection = findClientConnection(addr.asClientID());
  return connection ? connection->getInfo().peer_node_idx : folly::none;
}

/* static */
std::string Sender::describeConnection(const Address& addr) {
  if (!ThreadID::isWorker()) {
    return addr.toString();
  }
  Worker* w = Worker::onThisThread();
  ld_check(w);

  if (w->shuttingDown()) {
    return addr.toString();
  }

  if (!addr.valid()) {
    return addr.toString();
  }

  // index of worker to which a connection from or to peer identified by
  // this Address is assigned
  ClientIdxAllocator& alloc = w->processor_->clientIdxAllocator();
  std::pair<WorkerType, worker_id_t> assignee = addr.isClientAddress()
      ? alloc.getWorkerId(addr.id_.client_)
      : std::make_pair(w->worker_type_, w->idx_);

  std::string res;
  res.reserve(64);

  if (assignee.second.val() == -1) {
    res += "(disconnected)";
  } else {
    res += Worker::getName(assignee.first, assignee.second);
  }
  res += ":";
  res += addr.toString();
  if (!addr.isClientAddress() ||
      (assignee.first == w->worker_type_ && assignee.second == w->idx_)) {
    const Sockaddr& sa = w->sender().getSockaddr(addr);
    res += " (";
    res += sa.valid() ? sa.toString() : std::string("UNKNOWN");
    res += ")";
  }

  return res;
}

int Sender::noteDisconnectedClient(ClientID client_name) {
  ld_check(client_name.valid());
  if (worker_->shuttingDown()) {
    // This instance might already be (partially) destroyed.
    return 0;
  }

  if (impl_->client_conns_.count(client_name) == 0) {
    ld_critical(
        "INTERNAL ERROR: the name of a disconnected client Connection  %s "
        "is not in the client map",
        client_name.toString().c_str());
    ld_check(0);
    err = E::NOTFOUND;
    return -1;
  }
  STAT_INCR(Worker::stats(), client_connection_close_backlog);
  disconnected_clients_.push_front(client_name);

  return 0;
}

void DisconnectedClientCallback::operator()(Status st, const Address& name) {
  ld_debug("Sender's DisconnectedClientCallback called for Connection  %s "
           "with status %s",
           Sender::describeConnection(name).c_str(),
           error_name(st));

  ld_check(!active());

  sender_->noteDisconnectedClient(name.id_.client_);

  delete this;
}

void Sender::noteConfigurationChanged(
    std::shared_ptr<const NodesConfiguration> nodes_configuration) {
  nodes_ = std::move(nodes_configuration);
  disconnectFromNodesThatChangedAddressOrGeneration();
}

void Sender::onSettingsUpdated(std::shared_ptr<const Settings> new_settings) {
  settings_.swap(new_settings);
  disconnectFromNodesThatChangedAddressOrGeneration();
}

void Sender::disconnectFromNodesThatChangedAddressOrGeneration() {
  std::vector<std::unique_ptr<Connection>> to_close;

  for (auto it = impl_->server_conns_.begin();
       it != impl_->server_conns_.end();) {
    auto& s = it->second;
    if (s->isNodeConnectionAddressOrGenerationOutdated()) {
      // Socket close might trigger a rehash on the map
      // and invalidate iterators which will cause a memory violation.
      // So let's move it out first.
      to_close.push_back(std::move(it->second));
      it = impl_->server_conns_.erase(it);
    } else {
      ++it;
    }
  }

  for (auto& connection : to_close) {
    auto node_id = connection->getInfo().peer_name.id_.node_;
    ld_info("Closing connection to %s with E::NOTINFCONFIG",
            Sender::describeConnection(Address(node_id)).c_str());
    connection->close(E::NOTINCONFIG);
  }
}

bool Sender::bytesPendingLimitReached(const PeerType peer_type) const {
  auto& settings = Worker::settings();
  size_t limit = settings.outbufs_mb_max_per_thread * 1024 * 1024;
  const bool enforce_per_peer_type_limit =
      settings.server && settings.outbufs_limit_per_peer_type_enabled;
  if (enforce_per_peer_type_limit) {
    // If per peer-type limit is enabled, equally divide the budget.
    limit = (limit / (size_t)PeerType::NUM_PEER_TYPES);
  }
  size_t bytes_pending;
  if (enforce_per_peer_type_limit) {
    bytes_pending = getBytesPending(peer_type);
  } else {
    bytes_pending = getBytesPending();
  }

  const bool limit_reached = (bytes_pending > limit);

  return limit_reached;
}

void Sender::queueMessageCompletion(std::unique_ptr<Message> msg,
                                    const Address& to,
                                    Status s,
                                    const SteadyTimestamp t) {
  auto mc = std::make_unique<MessageCompletion>(std::move(msg), to, s, t);
  completed_messages_.push_back(*mc.release());
  if (!delivering_completed_messages_.exchange(true)) {
    auto deferred_execution = [this] {
      delivering_completed_messages_.store(false);
      deliverCompletedMessages();
    };
    if (worker_->getNumPriorities() > 1) {
      worker_->addWithPriority(
          std::move(deferred_execution), folly::Executor::HI_PRI);
    } else {
      worker_->add(std::move(deferred_execution));
    }
  }
}

std::string Sender::dumpQueuedMessages(Address addr) const {
  std::map<MessageType, int> counts;
  if (addr.valid()) {
    const Connection* conn = nullptr;
    if (addr.isClientAddress()) {
      const auto it = impl_->client_conns_.find(addr.id_.client_);
      if (it != impl_->client_conns_.end()) {
        conn = it->second.get();
      }
    } else {
      const auto idx = addr.asNodeID().index();
      const auto it = impl_->server_conns_.find(idx);
      if (it != impl_->server_conns_.end()) {
        conn = it->second.get();
      }
    }
    if (conn == nullptr) {
      // Unexpected but not worth asserting on (crashing the server) since
      // this is debugging code
      return "<Connection  not found>";
    }
    conn->dumpQueuedMessages(&counts);
  } else {
    for (const auto& entry : impl_->server_conns_) {
      entry.second->dumpQueuedMessages(&counts);
    }

    for (const auto& entry : impl_->client_conns_) {
      entry.second->dumpQueuedMessages(&counts);
    }
  }
  std::unordered_map<std::string, int> strmap;
  for (const auto& entry : counts) {
    strmap[messageTypeNames()[entry.first].c_str()] = entry.second;
  }
  return folly::toJson(folly::toDynamic(strmap));
}

void Sender::forAllConnections(
    const std::function<void(const Connection&)>& cb) const {
  for (const auto& entry : impl_->server_conns_) {
    cb(*entry.second);
  }
  for (const auto& entry : impl_->client_conns_) {
    cb(*entry.second);
  }
}

void Sender::forEachConnection(
    const std::function<void(const ConnectionInfo&)>& cb) const {
  forAllConnections([&cb](const Connection& c) { cb(c.getInfo()); });
}

std::shared_ptr<const std::atomic<bool>>
Sender::getConnectionToken(const ClientID cid) const {
  ld_check(cid.valid());
  const auto* info = getConnectionInfo(Address(cid));
  return info && info->is_active->load() ? info->is_active : nullptr;
}

void Sender::cleanupConnections() {
  size_t num_sockets = 0, sockets_closed = 0;
  size_t sock_stalled = 0, sock_active = 0;
  size_t reason_net_slow = 0, reason_recv_slow = 0, app_limited = 0;
  size_t max_slow_closures = settings_->rate_limit_socket_closed;
  auto start_time = SteadyTimestamp::now();
  auto close_if_slow = [&](Connection& conn) {
    auto status = conn.checkSocketHealth();
    bool is_slow = status == SocketDrainStatusType::STALLED ||
        (status == SocketDrainStatusType::NET_SLOW &&
         reason_net_slow < max_slow_closures);
    sock_active += status == SocketDrainStatusType::ACTIVE ? 1 : 0;
    app_limited += status == SocketDrainStatusType::IDLE ? 1 : 0;
    sock_stalled += status == SocketDrainStatusType::STALLED ? 1 : 0;
    reason_recv_slow += status == SocketDrainStatusType::RECV_SLOW ? 1 : 0;
    reason_net_slow += status == SocketDrainStatusType::NET_SLOW ? 1 : 0;

    if (is_slow) {
      ++sockets_closed;
      conn.close(E::TIMEDOUT);
    }
  };

  bool is_client = !settings_->server;
  size_t idle_connections_closed = 0;
  size_t max_idle_closures = settings_->rate_limit_idle_connection_closed;
  auto watermark = start_time - settings_->idle_connection_keep_alive;
  auto close_if_idle = [&](Connection& conn) {
    if (conn.isIdleAfter(watermark)) {
      ++idle_connections_closed;
      ++sockets_closed;
      conn.close(E::IDLE);
    }
  };

  for (auto& entry : impl_->server_conns_) {
    Connection* conn = entry.second.get();
    if (conn) {
      ++num_sockets;
      close_if_slow(*conn);
      // Only close client to server connections
      if (is_client && idle_connections_closed < max_idle_closures &&
          !conn->isClosed()) {
        close_if_idle(*conn);
      }
    } else {
      RATELIMIT_CRITICAL(
          std::chrono::seconds(10),
          1,
          "Unexpected null server socket found for nid %u. T59653729",
          entry.first);
    }
  }
  for (auto& entry : impl_->client_conns_) {
    Connection* conn = entry.second.get();
    if (conn) {
      ++num_sockets;
      close_if_slow(*conn);
    } else {
      RATELIMIT_CRITICAL(
          std::chrono::seconds(10),
          1,
          "Unexpected null client socket found for cid %s. T59653729",
          entry.first.toString().c_str());
    }
  }
  if (sockets_closed) {
    RATELIMIT_WARNING(
        std::chrono::seconds(10),
        10,
        "Sender closed %lu connections of which %lu had slow network"
        ", %lu had slow receiver, %lu connections were idle",
        sockets_closed,
        reason_net_slow,
        reason_recv_slow,
        idle_connections_closed);
  }
  auto socket_health_unknown = num_sockets - sock_active - app_limited -
      sock_stalled - reason_recv_slow - reason_net_slow;
  STAT_SET(Worker::stats(), num_sockets, num_sockets);
  STAT_SET(Worker::stats(), sock_active, sock_active);
  STAT_SET(Worker::stats(), sock_health_unknown, socket_health_unknown);
  STAT_SET(Worker::stats(), sock_stalled, sock_stalled);
  STAT_SET(Worker::stats(), sock_app_limited, app_limited);
  STAT_SET(Worker::stats(), sock_receiver_throttled, reason_recv_slow);
  STAT_SET(Worker::stats(), sock_network_throttled, reason_net_slow);
  STAT_SET(Worker::stats(), sock_idle, idle_connections_closed);
  auto diff = SteadyTimestamp::now() - start_time;
  STAT_ADD(Worker::stats(), slow_socket_detection_time, to_msec(diff).count());
  impl_->cleanup_connections_timer_.activate(
      Worker::settings().socket_health_check_period);
}

void Sender::fillDebugInfo(InfoSocketsTable& table) const {
  forAllConnections(
      [&table](const Connection& conn) { conn.getDebugInfo(table); });
}
}} // namespace facebook::logdevice
