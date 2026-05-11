#include "ice_agent.h"

namespace ice {

IceAgent::IceAgent(hv::EventLoopPtr loop) {
    if (loop) {
        loop_ = loop;
    } else {
        loop_thread_ = std::make_unique<hv::EventLoopThread>();
        loop_ = loop_thread_->loop();
    }
}

IceAgent::~IceAgent() {
    stop();
}

void IceAgent::setConfig(const IceConfig& config) {
    config_ = config;
}

int IceAgent::start() {
    if (running_) return 0;

    // Create and bind transports BEFORE starting the event loop thread.
    // hloop_create_udp_server and hloop_create_tcp_server operate on the hloop_t
    // which is already allocated. They must be called before hloop_run to be thread-safe.
    udp_transport_ = std::make_unique<UdpTransport>(loop_);
    int udpPort = udp_transport_->bind(config_.bindHost, config_.udpPort);
    if (udpPort < 0) {
        udp_transport_.reset();
        return -1;
    }

    // Create and bind TCP transport (if TCP candidates enabled)
    if (config_.gatherTcp) {
        tcp_transport_ = std::make_unique<TcpTransport>(loop_);
        int tcpPort = tcp_transport_->listen(config_.bindHost, config_.tcpPort);
        if (tcpPort < 0) {
            udp_transport_->close();
            udp_transport_.reset();
            tcp_transport_.reset();
            return -2;
        }
    }

    // Start event loop thread AFTER binding (the loop will pick up the IOs)
    if (loop_thread_ && !loop_thread_->isRunning()) {
        loop_thread_->start();
    }

    running_ = true;
    return 0;
}

void IceAgent::stop() {
    if (!running_) return;
    running_ = false;

    // Stop event loop thread FIRST.
    // After this, hloop_t is freed (AUTO_FREE) and loop_->loop() returns NULL.
    // This ensures no more callbacks fire and no pending events reference our objects.
    if (loop_thread_) {
        loop_thread_->stop(true);
    }

    // Now clean up sessions and transports.
    // Since the loop is gone, close() methods will skip hio_close/htimer_del calls.
    for (auto& session : sessions_) {
        session->close();
    }
    sessions_.clear();

    if (tcp_transport_) {
        tcp_transport_->close();
        tcp_transport_.reset();
    }
    if (udp_transport_) {
        udp_transport_->close();
        udp_transport_.reset();
    }
}

IceSessionPtr IceAgent::createSession(IceMode mode) {
    auto session = std::make_shared<IceSession>(mode, loop_);
    session->setUdpTransport(udp_transport_.get());
    if (tcp_transport_) {
        session->setTcpTransport(tcp_transport_.get());
    }
    sessions_.push_back(session);
    return session;
}

void IceAgent::destroySession(const IceSessionPtr& session) {
    session->close();
    sessions_.erase(
        std::remove(sessions_.begin(), sessions_.end(), session),
        sessions_.end());
}

int IceAgent::udpPort() const {
    return udp_transport_ ? udp_transport_->port() : 0;
}

int IceAgent::tcpPort() const {
    return tcp_transport_ ? tcp_transport_->port() : 0;
}

} // namespace ice
