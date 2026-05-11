#ifndef ICE_CANDIDATE_H_
#define ICE_CANDIDATE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "hsocket.h"

namespace ice {

// ICE Candidate Types (RFC 8445 Section 5.1.1)
enum class CandidateType {
    Host,              // Local interface address
    ServerReflexive,   // STUN mapped address (srflx)
    PeerReflexive,     // Discovered during connectivity check (prflx)
    Relay              // TURN relayed address
};

// ICE-TCP Candidate Types (RFC 6544)
enum class TcpType {
    None,       // UDP candidate
    Active,     // Will initiate TCP connection
    Passive,    // Will accept TCP connection
    SO          // Simultaneous-Open
};

// Transport protocol
enum class TransportProtocol {
    UDP,
    TCP
};

// ICE Candidate (RFC 8445 Section 5.1)
struct IceCandidate {
    // Required fields
    std::string foundation;      // Unique identifier for candidate pair pruning
    uint32_t componentId = 1;    // RTP=1, RTCP=2
    TransportProtocol protocol = TransportProtocol::UDP;
    uint32_t priority = 0;
    sockaddr_u addr;             // Transport address (IP + port)
    CandidateType type = CandidateType::Host;

    // Optional fields
    sockaddr_u relatedAddr;      // raddr/rport (base address for srflx/prflx/relay)
    TcpType tcpType = TcpType::None;

    // Internal use
    sockaddr_u baseAddr;         // Local address used for this candidate

    IceCandidate() {
        memset(&addr, 0, sizeof(addr));
        memset(&relatedAddr, 0, sizeof(relatedAddr));
        memset(&baseAddr, 0, sizeof(baseAddr));
    }

    // Get address as string "ip:port"
    std::string addrString() const;
    std::string relatedAddrString() const;

    // Type string for SDP
    static const char* typeString(CandidateType type);
    static CandidateType typeFromString(const std::string& str);
    static const char* tcpTypeString(TcpType type);
    static TcpType tcpTypeFromString(const std::string& str);
};

// Priority computation (RFC 8445 Section 5.1.2)
// priority = (2^24) * type_preference + (2^8) * local_preference + (2^0) * (256 - component_id)
uint32_t computeCandidatePriority(CandidateType type, uint32_t localPreference, uint32_t componentId);

// Type preference values (RFC 8445 Section 5.1.2.2)
uint32_t getTypePreference(CandidateType type);

// Compute local preference from IP address
uint32_t computeLocalPreference(const sockaddr_u& addr, TransportProtocol proto);

// Generate foundation string
std::string generateFoundation(CandidateType type, const sockaddr_u& baseAddr,
                               TransportProtocol proto, const std::string& serverAddr = "");

} // namespace ice

#endif // ICE_CANDIDATE_H_
