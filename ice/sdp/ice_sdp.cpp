#include "ice_sdp.h"

#include <sstream>
#include <cstring>
#include <cstdlib>

namespace ice {

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
        oss << "a=candidate:" << cand.toSdp() << "\r\n";
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
            if (cand.fromSdp(line.substr(12))) {
                result.candidates.push_back(cand);
            }
        }
    }

    return result;
}


} // namespace ice
