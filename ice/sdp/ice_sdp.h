#ifndef ICE_SDP_H_
#define ICE_SDP_H_

#include <string>
#include <vector>
#include "../candidate/ice_candidate.h"

namespace ice {

// ICE SDP attribute generation and parsing
// Handles: a=ice-ufrag, a=ice-pwd, a=ice-options, a=ice-lite, a=candidate

class IceSdp {
public:
    // Generate SDP attributes for an ICE session
    static std::string generateAttributes(
        const std::string& ufrag,
        const std::string& pwd,
        const std::vector<IceCandidate>& candidates,
        bool isLite = false,
        bool trickle = true);

    // Generate a single a=candidate line
    static std::string generateCandidateLine(const IceCandidate& candidate);

    // Parse SDP attributes and extract ICE info
    struct ParseResult {
        std::string ufrag;
        std::string pwd;
        std::vector<IceCandidate> candidates;
        bool isLite = false;
        bool trickle = false;
    };

    static ParseResult parseAttributes(const std::string& sdp);

    // Parse a single a=candidate: line
    static bool parseCandidate(const std::string& line, IceCandidate* candidate);
};

} // namespace ice

#endif // ICE_SDP_H_
