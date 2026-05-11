#include "ice_sdp.h"

#include <sstream>
#include <cstring>
#include <cstdlib>
#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace ice {

static std::vector<std::string> splitString(const std::string& str, char delim) {
    std::vector<std::string> tokens;
    std::istringstream iss(str);
    std::string token;
    while (std::getline(iss, token, delim)) {
        if (!token.empty()) {
            tokens.push_back(token);
        }
    }
    return tokens;
}

static std::string trimString(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\r\n");
    size_t end = str.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return str.substr(start, end - start + 1);
}

std::string IceSdp::generateAttributes(
    const std::string& ufrag,
    const std::string& pwd,
    const std::vector<IceCandidate>& candidates,
    bool isLite,
    bool trickle) {

    std::ostringstream oss;

    if (isLite) {
        oss << "a=ice-lite\r\n";
    }
    oss << "a=ice-ufrag:" << ufrag << "\r\n";
    oss << "a=ice-pwd:" << pwd << "\r\n";
    if (trickle) {
        oss << "a=ice-options:trickle\r\n";
    }

    for (const auto& cand : candidates) {
        oss << generateCandidateLine(cand) << "\r\n";
    }

    return oss.str();
}

std::string IceSdp::generateCandidateLine(const IceCandidate& candidate) {
    // Format: a=candidate:<foundation> <component> <transport> <priority> <addr> <port> typ <type>
    //         [raddr <addr> rport <port>] [tcptype <type>]
    std::ostringstream oss;

    // Get IP and port
    char ip[INET6_ADDRSTRLEN] = {0};
    int port = 0;
    if (candidate.addr.sa.sa_family == AF_INET) {
        inet_ntop(AF_INET, &candidate.addr.sin.sin_addr, ip, sizeof(ip));
        port = ntohs(candidate.addr.sin.sin_port);
    } else if (candidate.addr.sa.sa_family == AF_INET6) {
        inet_ntop(AF_INET6, &candidate.addr.sin6.sin6_addr, ip, sizeof(ip));
        port = ntohs(candidate.addr.sin6.sin6_port);
    }

    std::string transport = (candidate.protocol == TransportProtocol::UDP) ? "udp" : "tcp";

    oss << "a=candidate:" << candidate.foundation
        << " " << candidate.componentId
        << " " << transport
        << " " << candidate.priority
        << " " << ip
        << " " << port
        << " typ " << IceCandidate::typeString(candidate.type);

    // Related address (for srflx, prflx, relay)
    if (candidate.type != CandidateType::Host &&
        candidate.relatedAddr.sa.sa_family != 0) {
        char rip[INET6_ADDRSTRLEN] = {0};
        int rport = 0;
        if (candidate.relatedAddr.sa.sa_family == AF_INET) {
            inet_ntop(AF_INET, &candidate.relatedAddr.sin.sin_addr, rip, sizeof(rip));
            rport = ntohs(candidate.relatedAddr.sin.sin_port);
        } else if (candidate.relatedAddr.sa.sa_family == AF_INET6) {
            inet_ntop(AF_INET6, &candidate.relatedAddr.sin6.sin6_addr, rip, sizeof(rip));
            rport = ntohs(candidate.relatedAddr.sin6.sin6_port);
        }
        oss << " raddr " << rip << " rport " << rport;
    }

    // TCP type
    if (candidate.protocol == TransportProtocol::TCP && candidate.tcpType != TcpType::None) {
        oss << " tcptype " << IceCandidate::tcpTypeString(candidate.tcpType);
    }

    return oss.str();
}

IceSdp::ParseResult IceSdp::parseAttributes(const std::string& sdp) {
    ParseResult result;

    std::istringstream iss(sdp);
    std::string line;
    while (std::getline(iss, line)) {
        line = trimString(line);
        if (line.empty()) continue;

        if (line.find("a=ice-ufrag:") == 0) {
            result.ufrag = line.substr(12);
        } else if (line.find("a=ice-pwd:") == 0) {
            result.pwd = line.substr(10);
        } else if (line.find("a=ice-lite") == 0) {
            result.isLite = true;
        } else if (line.find("a=ice-options:") == 0) {
            std::string options = line.substr(14);
            if (options.find("trickle") != std::string::npos) {
                result.trickle = true;
            }
        } else if (line.find("a=candidate:") == 0) {
            IceCandidate cand;
            if (parseCandidate(line, &cand)) {
                result.candidates.push_back(cand);
            }
        }
    }

    return result;
}

bool IceSdp::parseCandidate(const std::string& line, IceCandidate* candidate) {
    // a=candidate:<foundation> <component> <transport> <priority> <addr> <port> typ <type> [extensions]
    std::string content = line;
    if (content.find("a=candidate:") == 0) {
        content = content.substr(12);
    } else if (content.find("candidate:") == 0) {
        content = content.substr(10);
    }

    auto tokens = splitString(content, ' ');
    if (tokens.size() < 8) return false;

    // Basic fields
    candidate->foundation = tokens[0];
    candidate->componentId = (uint32_t)atoi(tokens[1].c_str());

    std::string transport = tokens[2];
    std::transform(transport.begin(), transport.end(), transport.begin(), ::tolower);
    candidate->protocol = (transport == "tcp") ? TransportProtocol::TCP : TransportProtocol::UDP;

    candidate->priority = (uint32_t)strtoul(tokens[3].c_str(), nullptr, 10);

    // Address and port
    std::string addrStr = tokens[4];
    int port = atoi(tokens[5].c_str());

    // Parse address
    memset(&candidate->addr, 0, sizeof(candidate->addr));
    if (addrStr.find(':') != std::string::npos) {
        // IPv6
        candidate->addr.sin6.sin6_family = AF_INET6;
        inet_pton(AF_INET6, addrStr.c_str(), &candidate->addr.sin6.sin6_addr);
        candidate->addr.sin6.sin6_port = htons(port);
    } else {
        // IPv4
        candidate->addr.sin.sin_family = AF_INET;
        inet_pton(AF_INET, addrStr.c_str(), &candidate->addr.sin.sin_addr);
        candidate->addr.sin.sin_port = htons(port);
    }

    // tokens[6] should be "typ"
    if (tokens.size() < 8 || tokens[6] != "typ") return false;
    candidate->type = IceCandidate::typeFromString(tokens[7]);

    // Parse extensions
    for (size_t i = 8; i + 1 < tokens.size(); i += 2) {
        if (tokens[i] == "raddr" && i + 3 < tokens.size()) {
            std::string raddrStr = tokens[i + 1];
            // Look for rport
            if (i + 2 < tokens.size() && tokens[i + 2] == "rport" && i + 3 < tokens.size()) {
                int rport = atoi(tokens[i + 3].c_str());
                memset(&candidate->relatedAddr, 0, sizeof(candidate->relatedAddr));
                if (raddrStr.find(':') != std::string::npos) {
                    candidate->relatedAddr.sin6.sin6_family = AF_INET6;
                    inet_pton(AF_INET6, raddrStr.c_str(), &candidate->relatedAddr.sin6.sin6_addr);
                    candidate->relatedAddr.sin6.sin6_port = htons(rport);
                } else {
                    candidate->relatedAddr.sin.sin_family = AF_INET;
                    inet_pton(AF_INET, raddrStr.c_str(), &candidate->relatedAddr.sin.sin_addr);
                    candidate->relatedAddr.sin.sin_port = htons(rport);
                }
                i += 2; // skip rport token pair
            }
        } else if (tokens[i] == "tcptype") {
            candidate->tcpType = IceCandidate::tcpTypeFromString(tokens[i + 1]);
        }
    }

    return true;
}

} // namespace ice
