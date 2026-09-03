#ifndef ICE_MDNS_H_
#define ICE_MDNS_H_

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "EventLoop.h"
#include "hsocket.h"

namespace ice {

// mDNS (RFC 6762) constants
#define MDNS_PORT                 5353
#define MDNS_GROUP_IPV4           "224.0.0.251"
#define MDNS_GROUP_IPV6           "ff02::fb"
#define MDNS_SUFFIX               ".local"
#define MDNS_NAME_MAXLEN          255         // "<uuid>.local" is 42 bytes

#define MDNS_TTL                  120         // seconds (RFC 6762 Section 10.1)
#define MDNS_QUERY_INTERVAL       1000        // ms, query retransmission (RFC 6762 Section 5.4)
#define MDNS_MAX_QUERIES          3           // initial query + retransmissions
#define MDNS_ANNOUNCE_INTERVAL    1000        // ms, between gratuitous announcements
#define MDNS_MAX_ANNOUNCES        3
#define MDNS_RESOLVE_TIMEOUT_MS   4000
#define MDNS_TICK_INTERVAL        200         // ms, service tick
#define MDNS_STOP_TIMEOUT_S       5           // s, stop() waits for the loop thread to close the handles

// DNS record types (RFC 1035) used by mDNS
enum MdnsRecordType : uint16_t {
    MDNS_TYPE_A       = 1,
    MDNS_TYPE_PTR     = 12,
    MDNS_TYPE_AAAA    = 28,
    MDNS_TYPE_SRV     = 33,
    MDNS_TYPE_ANY     = 255,
};

#define MDNS_CLASS_IN             1
// QU bit in a question / cache-flush bit in a record (RFC 6762)
#define MDNS_UNICAST_RESPONSE     0x8000

// "<uuid>.local" - WebRTC style name used to hide a local ICE candidate address
std::string mdnsGenerateName();
// true if name ends with ".local" (case insensitive)
bool isMdnsName(const std::string& name);
// lowercase and strip the trailing dot, so that names can be used as map keys
std::string mdnsNormalizeName(const std::string& name);
// "ip:port" of a resolved address, for logging
std::string mdnsAddrString(const sockaddr_u& addr);

struct MdnsQuestion {
    std::string name;
    uint16_t type = MDNS_TYPE_A;
    bool unicastResponse = false;  // question class carries the QU bit
};

struct MdnsRecord {
    std::string name;
    uint16_t type = MDNS_TYPE_A;
    uint16_t rclass = MDNS_CLASS_IN;
    uint32_t ttl = MDNS_TTL;
    sockaddr_u addr;                       // rdata of A / AAAA records
    std::vector<uint8_t> rdata;            // raw rdata of any record type

    bool cacheFlush() const { return (rclass & MDNS_UNICAST_RESPONSE) != 0; }
    void setCacheFlush(bool on) {
        if (on) rclass |= MDNS_UNICAST_RESPONSE;
        else    rclass &= ~MDNS_UNICAST_RESPONSE;
    }
};

// Minimal mDNS message: what we need is a couple of questions and A/AAAA answers
struct MdnsMessage {
    bool response = false;
    bool authoritative = false;
    bool truncated = false;
    std::vector<MdnsQuestion> questions;
    std::vector<MdnsRecord> answers;
    std::vector<MdnsRecord> additionals;

    std::vector<uint8_t> encode() const;
    static bool decode(const uint8_t* data, size_t len, MdnsMessage* msg);
    // Collect A/AAAA records of the given name from answers and additionals
    bool findAddress(const std::string& name, sockaddr_u* addr) const;
};

// MdnsService is the mDNS endpoint of an ICE agent:
// - responder: answers A/AAAA queries of the local names published by this process,
//   so that a peer can map "<uuid>.local" of a hidden host candidate back to an IP.
// - resolver: queries the IP of a remote "<uuid>.local" candidate address.
// One instance is shared by all IceSessions of an IceAgent, it is bound to one event loop.
// NOTE: the underlying socket is only created lazily on first use.
class MdnsService : public std::enable_shared_from_this<MdnsService> {
public:
    // Called exactly once per resolve() request, addr is NULL on failure.
    using ResolveCallback = std::function<void(const std::string& name, const sockaddr_u* addr)>;

    explicit MdnsService(hv::EventLoopPtr loop);
    ~MdnsService();

    bool isRunning() const { return running_; }
    // The responder can only see multicast queries when port 5353 was bound.
    bool canAnswerQueries() const { return port_bound_; }

    // ---- responder ----
    // Publish an address for a name and announce it (thread-safe).
    void publish(const std::string& name, const sockaddr_u& addr);
    // Withdraw a published name, sending a goodbye record with TTL 0 (thread-safe).
    void unpublish(const std::string& name);
    bool isPublished(const std::string& name) const;

    // ---- resolver ----
    void resolve(const std::string& name, ResolveCallback cb, int timeoutMs = MDNS_RESOLVE_TIMEOUT_MS);
    void cancel(const std::string& name);
    bool isResolving(const std::string& name) const;

    // Tears the service down in its event loop thread. Called from another thread it
    // blocks until the handles were closed, so that the caller may drop its reference.
    void stop();

private:
    struct Announcement {
        std::string name;
        sockaddr_u addr;
        int sent = 0;
        uint64_t nextSendMs = 0;
    };

    struct Query {
        std::string name;
        // Callers waiting for the same name share one query and are all notified
        std::vector<ResolveCallback> cbs;
        int sent = 0;
        uint64_t nextSendMs = 0;
        uint64_t expireMs = 0;
    };

    int ensureRunning();                    // create sockets and the tick timer
    void doStop();
    void postToLoop(std::function<void()> fn);

    void publishInLoop(const std::string& name, const sockaddr_u& addr);
    void unpublishInLoop(const std::string& name);
    void resolveInLoop(const std::string& name, const ResolveCallback& cb, int timeoutMs);
    void cancelInLoop(const std::string& name);
    void onTick();
    void onRecv(hio_t* io, void* buf, int readbytes);
    void handleMessage(const MdnsMessage& msg, const struct sockaddr* from);
    void answerQuery(const MdnsQuestion& question, const struct sockaddr* from);
    void sendQuery(const std::string& name);
    // @param family: address family the packet has to leave on
    int sendUnicast(const std::vector<uint8_t>& data, const struct sockaddr* to, int family);
    int sendMulticast(const std::vector<uint8_t>& data, int family);

    hio_t* openSocket(int family);

    hv::EventLoopPtr loop_;
    bool running_ = false;
    bool port_bound_ = false;
    std::vector<hio_t*> ios_;
    std::vector<std::string> interfaces4_;     // local ipv4 addresses, one multicast outgoing per iface
    std::map<std::string, Announcement> announcements_;
    std::map<std::string, Query> queries_;
    hv::TimerID tick_timer_ = INVALID_TIMER_ID;
};

using MdnsServicePtr = std::shared_ptr<MdnsService>;

} // namespace ice

#endif // ICE_MDNS_H_
