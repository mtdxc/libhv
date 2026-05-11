#include "ice_candidate.h"

#include <cstdio>
#include <cstring>
#include <functional>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace ice {

std::string IceCandidate::addrString() const {
    char buf[SOCKADDR_STRLEN] = {0};
    if (addr.sa.sa_family == AF_INET) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin.sin_addr, ip, sizeof(ip));
        snprintf(buf, sizeof(buf), "%s:%d", ip, ntohs(addr.sin.sin_port));
    } else if (addr.sa.sa_family == AF_INET6) {
        char ip[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, &addr.sin6.sin6_addr, ip, sizeof(ip));
        snprintf(buf, sizeof(buf), "[%s]:%d", ip, ntohs(addr.sin6.sin6_port));
    }
    return buf;
}

std::string IceCandidate::relatedAddrString() const {
    char buf[SOCKADDR_STRLEN] = {0};
    if (relatedAddr.sa.sa_family == AF_INET) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &relatedAddr.sin.sin_addr, ip, sizeof(ip));
        snprintf(buf, sizeof(buf), "%s:%d", ip, ntohs(relatedAddr.sin.sin_port));
    } else if (relatedAddr.sa.sa_family == AF_INET6) {
        char ip[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, &relatedAddr.sin6.sin6_addr, ip, sizeof(ip));
        snprintf(buf, sizeof(buf), "[%s]:%d", ip, ntohs(relatedAddr.sin6.sin6_port));
    }
    return buf;
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
        base = 65535 - 8192; // TCP gets lower preference
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

} // namespace ice
