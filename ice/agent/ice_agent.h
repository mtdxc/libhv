#ifndef ICE_AGENT_H_
#define ICE_AGENT_H_

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

#include "EventLoopThread.h"
#include "ice_config.h"
#include "../session/ice_session.h"
#include "../transport/udp_transport.h"
#include "../transport/tcp_transport.h"

namespace ice {

// IceAgent: Top-level API managing all ICE sessions and transport
class IceAgent {
public:
    // Create agent with optional external event loop
    // If loop is null, creates its own EventLoopThread
    explicit IceAgent(hv::EventLoopPtr loop = nullptr);
    ~IceAgent();

    // Configuration (must be called before start())
    void setConfig(const IceConfig& config);
    const IceConfig& config() const { return config_; }

    // Start transport (bind ports)
    // Returns 0 on success, <0 on error
    int start();

    // Stop transport and all sessions
    void stop();

    // Create a new ICE session
    IceSessionPtr createSession(IceMode mode = IceMode::Full);

    // Destroy a session
    void destroySession(const IceSessionPtr& session);

    // Get bound ports
    int udpPort() const;
    int tcpPort() const;

    // Get event loop
    hv::EventLoopPtr loop() const { return loop_; }

    // Check if running
    bool isRunning() const { return running_; }

private:
    IceConfig config_;
    hv::EventLoopPtr loop_;
    std::unique_ptr<hv::EventLoopThread> loop_thread_; // owned if no external loop

    std::unique_ptr<UdpTransport> udp_transport_;
    std::unique_ptr<TcpTransport> tcp_transport_;

    std::vector<IceSessionPtr> sessions_;
    bool running_ = false;
};

} // namespace ice

#endif // ICE_AGENT_H_
