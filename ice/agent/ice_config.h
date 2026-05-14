#ifndef ICE_CONFIG_H_
#define ICE_CONFIG_H_

#include <string>
#include <vector>
#include "hsocket.h"

namespace ice {

enum class IceMode;
enum class NominationMode;

// Network address
struct NetAddr {
    std::string host;
    int port = 0;

    NetAddr() = default;
    NetAddr(const std::string& h, int p) : host(h), port(p) {}
    std::string toString() const {
        return host + ":" + std::to_string(port);
    }
    bool toSockaddr(sockaddr_u* addr) const {
        return sockaddr_set_ipport(addr, host.c_str(), port) == 0;
    }
};

// TURN server configuration
struct TurnServerConfig {
    NetAddr addr;
    std::string username;
    std::string password;
    enum Proto { UDP = 0, TCP = 1, TLS = 2 } protocol = UDP;
};

// ICE Agent configuration
struct IceConfig {
    // STUN servers for server-reflexive candidates
    std::vector<NetAddr> stunServers;

    // TURN servers for relay candidates
    std::vector<TurnServerConfig> turnServers;

    // Local ports to bind (0 = system-assigned ephemeral)
    int udpPort = 0;
    int tcpPort = 0;

    // Bind host (default: all interfaces)
    std::string bindHost = "0.0.0.0";

    // Timing parameters
    int checkIntervalMs = 50;       // Ta: connectivity check pacing (ms)
    int gatheringTimeoutMs = 10000; // Max time for gathering
    int connectivityTimeoutMs = 30000; // Max time for connectivity checks

    // ICE features
    bool gatherTcp = false;         // Gather TCP candidates
    bool gatherHost = true;         // Gather host candidates
    bool gatherSrflx = true;        // Gather server-reflexive candidates
    bool gatherRelay = false;       // Gather relay candidates (requires TURN)

    // Nomination
    NominationMode nomination = (NominationMode)0; // Regular

    // Software string (for STUN SOFTWARE attribute)
    std::string software = "libhv-ice/1.0";
};

} // namespace ice

#endif // ICE_CONFIG_H_
