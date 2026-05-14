#include "ice_candidate.h"
#include <sstream>
#include <cstdio>
#include <cstring>
#include <functional>
#include <hstring.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace ice {

std::string IceCandidate::addrString() const {
    char buf[SOCKADDR_STRLEN] = {0};
    return sockaddr_str(const_cast<sockaddr_u*>(&addr), buf, sizeof(buf));
}

std::string IceCandidate::relatedAddrString() const {
    char buf[SOCKADDR_STRLEN] = {0};
    return sockaddr_str(const_cast<sockaddr_u*>(&relatedAddr), buf, sizeof(buf));
}

const char* IceCandidate::typeString(CandidateType type) {
    switch (type) {
    case CandidateType::Host: return "host";
    case CandidateType::ServerReflexive: return "srflx";
    case CandidateType::PeerReflexive: return "prflx";
    case CandidateType::Relay: return "relay";
    }
    return "host";
}

CandidateType IceCandidate::typeFromString(const std::string& str) {
    if (str == "host") return CandidateType::Host;
    if (str == "srflx") return CandidateType::ServerReflexive;
    if (str == "prflx") return CandidateType::PeerReflexive;
    if (str == "relay") return CandidateType::Relay;
    return CandidateType::Host;
}

const char* IceCandidate::tcpTypeString(TcpType type) {
    switch (type) {
    case TcpType::None: return "";
    case TcpType::Active: return "active";
    case TcpType::Passive: return "passive";
    case TcpType::SO: return "so";
    }
    return "";
}

TcpType IceCandidate::tcpTypeFromString(const std::string& str) {
    if (str == "active") return TcpType::Active;
    if (str == "passive") return TcpType::Passive;
    if (str == "so") return TcpType::SO;
    return TcpType::None;
}

// RFC 8445 Section 5.1.2.2
uint32_t getTypePreference(CandidateType type) {
    switch (type) {
    case CandidateType::Host: return 126;
    case CandidateType::PeerReflexive: return 110;
    case CandidateType::ServerReflexive: return 100;
    case CandidateType::Relay: return 0;
    }
    return 0;
}

uint32_t computeLocalPreference(const sockaddr_u& addr, TransportProtocol proto) {
    // RFC 8445 recommends 65535 for single-homed, use IP-based differentiation
    // For ICE-TCP, give lower preference than UDP
    uint32_t base = 65535;
    if (proto == TransportProtocol::TCP) {
        base -= 8192; // TCP gets lower preference
    }

    // IPv6 gets slightly higher preference than IPv4
    if (addr.sa.sa_family == AF_INET6) {
        base = (base > 1) ? base - 1 : base;
    }

    return base;
}

uint32_t computeCandidatePriority(CandidateType type, uint32_t localPreference, uint32_t componentId) {
    uint32_t typePref = getTypePreference(type);
    return (typePref << 24) | ((localPreference & 0xFFFF) << 8) | (256 - componentId);
}

std::string generateFoundation(CandidateType type, const sockaddr_u& baseAddr,
                               TransportProtocol proto, const std::string& serverAddr) {
    // Foundation = hash of (type, base IP, protocol, STUN server)
    // Use a simple string-based approach
    char buf[256];
    char ip[INET6_ADDRSTRLEN] = {0};
    if (baseAddr.sa.sa_family == AF_INET) {
        inet_ntop(AF_INET, &baseAddr.sin.sin_addr, ip, sizeof(ip));
    } else if (baseAddr.sa.sa_family == AF_INET6) {
        inet_ntop(AF_INET6, &baseAddr.sin6.sin6_addr, ip, sizeof(ip));
    }

    snprintf(buf, sizeof(buf), "%d_%s_%d_%s",
             (int)type, ip, (int)proto, serverAddr.c_str());

    // Simple hash to produce numeric foundation
    std::hash<std::string> hasher;
    size_t h = hasher(std::string(buf));
    char result[16];
    snprintf(result, sizeof(result), "%zu", h % 1000000000);
    return result;
}

void IceCandidate::update(const std::string& serverAddr) {
    priority = computeCandidatePriority(type, computeLocalPreference(addr, protocol), componentId);
    foundation = generateFoundation(type, baseAddr, protocol, serverAddr);
}

std::string IceCandidate::toSdp() const{
    // Format: a=candidate:<foundation> <component> <transport> <priority> <addr> <port> typ <type>
    //         [raddr <addr> rport <port>] [tcptype <type>]
    std::ostringstream oss;

    // Get IP and port
    char ip[INET6_ADDRSTRLEN] = {0};
    sockaddr_u* addr = (sockaddr_u*)&this->addr;
    int port = sockaddr_port(addr);
    sockaddr_ip(addr, ip, sizeof(ip));

    std::string transport = (this->protocol == TransportProtocol::UDP) ? "udp" : "tcp";

    oss << foundation
        << " " << componentId
        << " " << transport
        << " " << priority
        << " " << ip
        << " " << port
        << " typ " << typeString(type);

    // Related address (for srflx, prflx, relay)
    if (this->type != CandidateType::Host && this->relatedAddr.sa.sa_family != 0) {
        sockaddr_u* raddr = (sockaddr_u*)&this->relatedAddr;
        char rip[INET6_ADDRSTRLEN] = {0};
        int rport = sockaddr_port(raddr);
        sockaddr_ip(raddr, rip, sizeof(rip));
        oss << " raddr " << rip << " rport " << rport;
    }

    // TCP type
    if (protocol == TransportProtocol::TCP && tcpType != TcpType::None) {
        oss << " tcptype " << tcpTypeString(tcpType);
    }

    return oss.str();
}

bool IceCandidate::fromSdp(const std::string& line) {
    // a=candidate:<foundation> <component> <transport> <priority> <addr> <port> typ <type> [extensions]
    std::string content = line;
    auto tokens = hv::split(content, ' ');
    if (tokens.size() < 8) return false;

    // Basic fields
    this->foundation = tokens[0];
    this->componentId = (uint32_t)atoi(tokens[1].c_str());

    std::string transport = hv::tolower(tokens[2]);
    this->protocol = (transport == "tcp") ? TransportProtocol::TCP : TransportProtocol::UDP;

    this->priority = (uint32_t)strtoul(tokens[3].c_str(), nullptr, 10);

    // Address and port
    std::string addrStr = tokens[4];
    int port = atoi(tokens[5].c_str());

    // Parse address
    memset(&this->addr, 0, sizeof(this->addr));
    if (addrStr.find(':') != std::string::npos) {
        // IPv6
        this->addr.sin6.sin6_family = AF_INET6;
        inet_pton(AF_INET6, addrStr.c_str(), &this->addr.sin6.sin6_addr);
        this->addr.sin6.sin6_port = htons(port);
    } else {
        // IPv4
        this->addr.sin.sin_family = AF_INET;
        inet_pton(AF_INET, addrStr.c_str(), &this->addr.sin.sin_addr);
        this->addr.sin.sin_port = htons(port);
    }

    // tokens[6] should be "typ"
    if (tokens.size() < 8 || tokens[6] != "typ") return false;
    this->type = IceCandidate::typeFromString(tokens[7]);

    // Parse extensions
    for (size_t i = 8; i + 1 < tokens.size(); i += 2) {
        if (tokens[i] == "raddr" && i + 3 < tokens.size()) {
            std::string raddrStr = tokens[i + 1];
            // Look for rport
            if (i + 2 < tokens.size() && tokens[i + 2] == "rport" && i + 3 < tokens.size()) {
                int rport = atoi(tokens[i + 3].c_str());
                memset(&this->relatedAddr, 0, sizeof(this->relatedAddr));
                if (raddrStr.find(':') != std::string::npos) {
                    this->relatedAddr.sin6.sin6_family = AF_INET6;
                    inet_pton(AF_INET6, raddrStr.c_str(), &this->relatedAddr.sin6.sin6_addr);
                    this->relatedAddr.sin6.sin6_port = htons(rport);
                } else {
                    this->relatedAddr.sin.sin_family = AF_INET;
                    inet_pton(AF_INET, raddrStr.c_str(), &this->relatedAddr.sin.sin_addr);
                    this->relatedAddr.sin.sin_port = htons(rport);
                }
                i += 2; // skip rport token pair
            }
        } else if (tokens[i] == "tcptype") {
            this->tcpType = tcpTypeFromString(tokens[i + 1]);
        }
    }

    return true;
}

} // namespace ice
