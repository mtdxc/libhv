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

inline const char* TransportProtocolStr(TransportProtocol p) {
    switch (p) {  
    case TransportProtocol::UDP: return "udp";
    case TransportProtocol::TCP: return "tcp";
    default: return "";
    }
}
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
    // mDNS (RFC 6762) name "<uuid>.local" of a hidden host candidate. When it is set,
    // the candidate is advertised by name in SDP instead of by its literal address.
    std::string mdnsName;

    // Internal use
    sockaddr_u baseAddr;         // Local address used for this candidate

    IceCandidate() {
        memset(&addr, 0, sizeof(addr));
        memset(&relatedAddr, 0, sizeof(relatedAddr));
        memset(&baseAddr, 0, sizeof(baseAddr));
    }
    void update(const std::string& serverAddr="");
    std::string toSdp(bool prefix = false) const;
    bool fromSdp(const std::string& sdp);
    
    // Get address as string "ip:port"
    std::string addrString() const;
    std::string relatedAddrString() const;

    // An mDNS candidate carries a name instead of a literal address
    bool isMdns() const { return !mdnsName.empty(); }
    // false while an mDNS candidate is still waiting for resolution
    bool hasAddress() const;
    // Address as it appears in SDP: the mDNS name when hidden, "ip" otherwise
    std::string sdpAddress() const;
    // Store the address resolved from the mDNS name, keeping the SDP port
    void applyResolvedAddress(const sockaddr_u& resolved);

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
