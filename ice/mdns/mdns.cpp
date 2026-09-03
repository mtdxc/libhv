#include "mdns.h"

#include <chrono>
#include <cstring>
#include <future>
#include <iomanip>
#include <random>
#include <sstream>

#include "../stun/stun_message.h"

#include "hlog.h"
#include "hloop.h"
#include "ifconfig.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

namespace ice {

namespace {

void appendBe16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back((uint8_t)(v >> 8));
    buf.push_back((uint8_t)(v & 0xFF));
}

void appendBe32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back((uint8_t)(v >> 24));
    buf.push_back((uint8_t)(v >> 16));
    buf.push_back((uint8_t)(v >> 8));
    buf.push_back((uint8_t)(v & 0xFF));
}

// "abc.local" => 3abc5local00
void encodeName(const std::string& name, std::vector<uint8_t>& buf) {
    size_t start = 0;
    while (start < name.size()) {
        size_t end = name.find('.', start);
        if (end == std::string::npos) end = name.size();
        size_t len = end - start;
        if (len > 63) len = 63;  // DNS labels are at most 63 bytes
        if (len > 0) {
            buf.push_back((uint8_t)len);
            buf.insert(buf.end(), name.begin() + start, name.begin() + start + len);
        }
        if (end == name.size()) break;
        start = end + 1;
    }
    buf.push_back(0);  // root label
}

// Decode a DNS name, following compression pointers (RFC 1035 Section 4.1.4).
// @param pos: in/out, advanced past the name (excluding the bytes pointed to)
bool decodeName(const uint8_t* data, size_t len, size_t& pos, std::string& name) {
    name.clear();
    size_t cursor = pos;
    bool endSet = false;
    for (int guard = 0; guard < 128; ++guard) {  // guard against pointer loops
        if (cursor >= len) return false;
        uint8_t label = data[cursor];
        if (label == 0) {
            if (!endSet) pos = cursor + 1;
            return true;
        }
        if ((label & 0xC0) == 0xC0) {
            if (cursor + 1 >= len) return false;
            size_t offset = (((size_t)(label & 0x3F) << 8) | data[cursor + 1]);
            if (!endSet) {
                pos = cursor + 2;
                endSet = true;
            }
            if (offset >= len) return false;
            cursor = offset;
            continue;
        }
        if ((label & 0xC0) != 0) return false;  // unknown label format
        if (cursor + 1 + label > len) return false;
        if (!name.empty()) name += '.';
        name.append((const char*)data + cursor + 1, label);
        cursor += 1 + label;
    }
    return false;
}

// Address rdata of A/AAAA records, empty for any other record type
std::vector<uint8_t> recordRdata(const MdnsRecord& record) {
    if (!record.rdata.empty()) return record.rdata;
    const uint8_t* p = nullptr;
    size_t rdataLen = 0;
    if (record.type == MDNS_TYPE_A && record.addr.sa.sa_family == AF_INET) {
        p = (const uint8_t*)&record.addr.sin.sin_addr;
        rdataLen = 4;
    } else if (record.type == MDNS_TYPE_AAAA && record.addr.sa.sa_family == AF_INET6) {
        p = (const uint8_t*)&record.addr.sin6.sin6_addr;
        rdataLen = 16;
    }
    if (!p) return {};
    return std::vector<uint8_t>(p, p + rdataLen);
}

void encodeRecord(const MdnsRecord& record, std::vector<uint8_t>& buf) {
    auto rdata = recordRdata(record);
    encodeName(record.name, buf);
    appendBe16(buf, record.type);
    appendBe16(buf, record.rclass);
    appendBe32(buf, record.ttl);
    appendBe16(buf, (uint16_t)rdata.size());
    buf.insert(buf.end(), rdata.begin(), rdata.end());
}

bool decodeRecord(const uint8_t* data, size_t len, size_t& pos, MdnsRecord* record) {
    std::string name;
    if (!decodeName(data, len, pos, name)) return false;
    if (pos + 10 > len) return false;
    record->name = mdnsNormalizeName(name);
    record->type = read_be16(data + pos);
    record->rclass = read_be16(data + pos + 2);
    record->ttl = read_be32(data + pos + 4);
    uint16_t rdlength = read_be16(data + pos + 8);
    pos += 10;
    if (pos + rdlength > len) return false;
    record->rdata.assign(data + pos, data + pos + rdlength);
    memset(&record->addr, 0, sizeof(record->addr));
    if (record->type == MDNS_TYPE_A && rdlength == 4) {
        record->addr.sin.sin_family = AF_INET;
        memcpy(&record->addr.sin.sin_addr, data + pos, 4);
    } else if (record->type == MDNS_TYPE_AAAA && rdlength == 16) {
        record->addr.sin6.sin6_family = AF_INET6;
        memcpy(&record->addr.sin6.sin6_addr, data + pos, 16);
    }
    pos += rdlength;
    return true;
}

int familyOfRecordType(uint16_t type) {
    return type == MDNS_TYPE_AAAA ? AF_INET6 : AF_INET;
}

// A gratuitous response (announcement/goodbye) or an answer to a query, holding one
// A/AAAA record of the given address with cache-flush set.
MdnsMessage makeAddressMessage(const std::string& name, const sockaddr_u& addr, uint32_t ttl) {
    MdnsMessage msg;
    msg.response = true;
    msg.authoritative = true;
    MdnsRecord record;
    record.name = name;
    record.type = addr.sa.sa_family == AF_INET6 ? MDNS_TYPE_AAAA : MDNS_TYPE_A;
    record.addr = addr;
    record.ttl = ttl;
    record.setCacheFlush(true);
    msg.answers.push_back(record);
    return msg;
}

}  // namespace
std::string mdnsNormalizeName(const std::string& name) {
    std::string result;
    result.reserve(name.size());
    for (char c : name) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        result += c;
    }
    while (!result.empty() && result.back() == '.') {
        result.pop_back();
    }
    return result;
}

bool isMdnsName(const std::string& name) {
    static const std::string suffix = MDNS_SUFFIX;
    if (name.size() <= suffix.size()) return false;
    std::string tail = name.substr(name.size() - suffix.size());
    return mdnsNormalizeName(tail) == suffix;
}

std::string mdnsAddrString(const sockaddr_u& addr) {
    char buf[SOCKADDR_STRLEN] = {0};
    sockaddr_str((sockaddr_u*)&addr, buf, sizeof(buf));
    return buf;
}

std::string mdnsGenerateName() {
    static const char digits[] = "0123456789abcdef";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 15);

    // random UUID v4, the naming scheme browsers use for hidden ICE candidates
    std::string uuid;
    uuid.reserve(36);
    for (int i = 0; i < 36; ++i) {
        switch (i) {
        case 8: case 13: case 18: case 23:
            uuid += '-';
            continue;
        case 14:
            uuid += '4';  // version 4
            continue;
        case 19:
            uuid += digits[8 + dist(gen) % 4];  // RFC 4122 variant
            continue;
        default:
            uuid += digits[dist(gen)];
        }
    }
    return uuid + MDNS_SUFFIX;
}

// ---------------------------------------------------------------------------
// MdnsMessage
// ---------------------------------------------------------------------------

std::vector<uint8_t> MdnsMessage::encode() const {
    uint16_t flags = 0;
    if (response) flags |= 0x8000;        // QR
    if (authoritative) flags |= 0x0400;   // AA
    if (truncated) flags |= 0x0200;       // TC

    std::vector<uint8_t> buf;
    buf.reserve(256);
    appendBe16(buf, 0);                                  // ID is always 0 for mDNS
    appendBe16(buf, flags);
    appendBe16(buf, (uint16_t)questions.size());
    appendBe16(buf, (uint16_t)answers.size());
    appendBe16(buf, 0);                                  // NSCOUNT
    appendBe16(buf, (uint16_t)additionals.size());
    for (const auto& question : questions) {
        encodeName(question.name, buf);
        appendBe16(buf, question.type);
        uint16_t rclass = MDNS_CLASS_IN;
        if (question.unicastResponse) rclass |= MDNS_UNICAST_RESPONSE;
        appendBe16(buf, rclass);
    }
    for (const auto& record : answers) {
        encodeRecord(record, buf);
    }
    for (const auto& record : additionals) {
        encodeRecord(record, buf);
    }
    return buf;
}

bool MdnsMessage::decode(const uint8_t* data, size_t len, MdnsMessage* msg) {
    if (!data || !msg || len < 12) return false;
    msg->questions.clear();
    msg->answers.clear();
    msg->additionals.clear();

    uint16_t flags = read_be16(data + 2);
    msg->response = (flags & 0x8000) != 0;
    msg->authoritative = (flags & 0x0400) != 0;
    msg->truncated = (flags & 0x0200) != 0;
    uint16_t counts[4] = {
        read_be16(data + 4),   // QDCOUNT
        read_be16(data + 6),   // ANCOUNT
        read_be16(data + 8),   // NSCOUNT
        read_be16(data + 10),  // ARCOUNT
    };

    size_t pos = 12;
    for (int i = 0; i < counts[0]; ++i) {
        MdnsQuestion question;
        std::string name;
        if (!decodeName(data, len, pos, name)) return false;
        if (pos + 4 > len) return false;
        question.name = mdnsNormalizeName(name);
        question.type = read_be16(data + pos);
        uint16_t rclass = read_be16(data + pos + 2);
        question.unicastResponse = (rclass & MDNS_UNICAST_RESPONSE) != 0;
        pos += 4;
        msg->questions.push_back(std::move(question));
    }

    MdnsRecord record;
    for (int i = 0; i < counts[1]; ++i) {
        if (!decodeRecord(data, len, pos, &record)) return false;
        msg->answers.push_back(record);
    }
    for (int i = 0; i < counts[2]; ++i) {
        if (!decodeRecord(data, len, pos, &record)) return false;  // authority is unused
    }
    for (int i = 0; i < counts[3]; ++i) {
        if (!decodeRecord(data, len, pos, &record)) return false;
        msg->additionals.push_back(record);
    }
    return true;
}

bool MdnsMessage::findAddress(const std::string& name, sockaddr_u* addr) const {
    std::string wanted = mdnsNormalizeName(name);
    for (const auto& section : {answers, additionals}) {
        for (const auto& record : section) {
            if (record.name != wanted) continue;
            if (record.type != MDNS_TYPE_A && record.type != MDNS_TYPE_AAAA) continue;
            if (record.ttl == 0) continue;  // goodbye
            if (addr) *addr = record.addr;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// MdnsService
// ---------------------------------------------------------------------------

MdnsService::MdnsService(hv::EventLoopPtr loop) : loop_(loop) {}

MdnsService::~MdnsService() {
    // IceAgent::stop() is expected to have called stop(); tear the sockets down
    // here as a last resort so that a session that was never stopped leaks nothing.
    if (running_) doStop();
}

void MdnsService::postToLoop(std::function<void()> fn) {
    if (!fn) return;
    if (!loop_ || loop_->isInLoopThread()) {
        fn();
        return;
    }
    std::weak_ptr<MdnsService> weak = shared_from_this();
    loop_->runInLoop([weak, fn = std::move(fn)]() {
        if (auto self = weak.lock()) fn();
    });
}

hio_t* MdnsService::openSocket(int family) {
    hloop_t* loop = loop_ ? loop_->loop() : nullptr;
    if (!loop) return nullptr;

    int fd = socket(family, SOCK_DGRAM, 0);
    if (fd < 0) {
        hlogw("mdns: socket(family=%d) failed: %s", family, socket_strerror(socket_errno()));
        return nullptr;
    }
    // NOTE: SO_REUSEADDR has to be set before bind, otherwise sharing port 5353
    // with the system mDNS responder fails (at least on Windows).
    so_reuseaddr(fd, 1);
    so_reuseport(fd, 1);

    sockaddr_u bindAddr;
    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sa.sa_family = (unsigned short)family;
    if (family == AF_INET6) {
        bindAddr.sin6.sin6_port = htons(MDNS_PORT);
#ifdef IPV6_V6ONLY
        // the ipv6 socket must not steal the ipv4 bind of another mDNS socket
        ip_v6only(fd, 1);
#endif
    } else {
        bindAddr.sin.sin_port = htons(MDNS_PORT);
        bindAddr.sin.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    if (bind(fd, &bindAddr.sa, SOCKADDR_LEN(&bindAddr)) < 0) {
        hlogw("mdns: bind %s:%d failed: %s", family == AF_INET6 ? "[::]" : "0.0.0.0", MDNS_PORT,
              socket_strerror(socket_errno()));
        // Port 5353 is exclusively taken by another process. Queries still work from an
        // ephemeral port when the QU bit asks responders for an unicast reply, but the
        // responder side is dead: we will not see any multicast query.
        memset(&bindAddr, 0, sizeof(bindAddr));
        bindAddr.sa.sa_family = (unsigned short)family;
        if (bind(fd, &bindAddr.sa, SOCKADDR_LEN(&bindAddr)) < 0) {
            hlogw("mdns: bind ephemeral port failed: %s", socket_strerror(socket_errno()));
            closesocket(fd);
            return nullptr;
        }
    } else if (family == AF_INET) {
        port_bound_ = true;
    }

    // mDNS packets must be sent with a hop limit of 255 and stay on the local link
    unsigned char multicastTtl = 255;
    unsigned char multicastLoop = 1;
    if (family == AF_INET6) {
#ifdef IPV6_MULTICAST_HOPS
        setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, (const char*)&multicastTtl, sizeof(multicastTtl));
#endif
#ifdef IPV6_MULTICAST_LOOP
        setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, (const char*)&multicastLoop, sizeof(multicastLoop));
#endif
    } else {
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&multicastTtl, sizeof(multicastTtl));
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&multicastLoop, sizeof(multicastLoop));
    }

    // Join the mDNS group, per local interface for ipv4 so that multi-homed hosts
    // receive queries on every link.
    if (family == AF_INET) {
        struct ip_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        inet_pton(AF_INET, MDNS_GROUP_IPV4, &mreq.imr_multiaddr);
        if (interfaces4_.empty()) {
            mreq.imr_interface.s_addr = htonl(INADDR_ANY);
            setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mreq, sizeof(mreq));
        } else {
            for (const auto& ip : interfaces4_) {
                if (inet_pton(AF_INET, ip.c_str(), &mreq.imr_interface) != 1) continue;
                if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mreq, sizeof(mreq)) < 0) {
                    hlogd("mdns: join %s on %s failed: %s", MDNS_GROUP_IPV4, ip.c_str(), socket_strerror(socket_errno()));
                }
            }
        }
    }
#ifdef IPV6_JOIN_GROUP
    else {
        struct ipv6_mreq mreq6;
        memset(&mreq6, 0, sizeof(mreq6));
        inet_pton(AF_INET6, MDNS_GROUP_IPV6, &mreq6.ipv6mr_multiaddr);
        mreq6.ipv6mr_interface = 0;  // kernel selected interface
        setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, (const char*)&mreq6, sizeof(mreq6));
    }
#endif

    hio_t* io = hio_get(loop, fd);
    if (!io) {
        closesocket(fd);
        return nullptr;
    }
    hio_set_context(io, this);
    hio_setcb_read(io, [](hio_t* io, void* buf, int readbytes) {
        MdnsService* self = (MdnsService*)hio_context(io);
        if (self) self->onRecv(io, buf, readbytes);
    });
    hio_read(io);
    return io;
}

int MdnsService::ensureRunning() {
    if (running_) return 0;
    if (!loop_ || !loop_->loop()) return -1;
    if (!loop_->isInLoopThread()) {
        hlogw("mdns: ensureRunning must be called in the loop thread");
        return -1;
    }

    if (interfaces4_.empty()) {
        std::vector<ifconfig_t> ifcs;
        ifconfig(ifcs);
        for (const auto& ifc : ifcs) {
            if (ifc.ip[0] == '\0') continue;
            interfaces4_.push_back(ifc.ip);
        }
    }

    port_bound_ = false;
    hio_t* v4 = openSocket(AF_INET);
    if (v4) ios_.push_back(v4);
    hio_t* v6 = openSocket(AF_INET6);
    if (v6) ios_.push_back(v6);
    if (ios_.empty()) {
        hlogw("mdns: no mDNS socket could be created");
        return -1;
    }

    std::weak_ptr<MdnsService> weak = shared_from_this();
    tick_timer_ = loop_->setTimer(MDNS_TICK_INTERVAL, [weak](hv::TimerID) {
        if (auto self = weak.lock()) self->onTick();
    }, INFINITE);
    running_ = true;
    hlogi("mdns: started on %zu socket(s), responder=%d", ios_.size(), (int)port_bound_);
    return 0;
}

void MdnsService::doStop() {
    hloop_t* loop = loop_ ? loop_->loop() : nullptr;
    if (loop && tick_timer_ != INVALID_TIMER_ID) {
        loop_->killTimer(tick_timer_);
    }
    tick_timer_ = INVALID_TIMER_ID;

    auto ios = ios_;
    ios_.clear();
    for (auto io : ios) {
        if (io) {
            hio_set_context(io, nullptr);
            hio_close(io);
        }
    }

    // Nobody is left to answer queries or to wait for a resolution
    auto queries = std::move(queries_);
    queries_.clear();
    for (auto& kv : queries) {
        for (auto& cb : kv.second.cbs) {
            if (cb) cb(kv.second.name, nullptr);
        }
    }
    announcements_.clear();
    running_ = false;
    port_bound_ = false;
    hlogi("mdns: stopped");
}

void MdnsService::stop() {
    // NOTE: the handles are owned by the event loop, so doStop has to run there. Keep a
    // reference alive until it did, otherwise the caller could drop the last shared_ptr
    // and ~MdnsService would tear the handles down from the wrong thread.
    std::shared_ptr<MdnsService> self = shared_from_this();
    if (!loop_ || loop_->isInLoopThread()) {
        doStop();
        return;
    }
    auto stopped = std::make_shared<std::promise<void>>();
    auto finished = stopped->get_future();
    loop_->runInLoop([self, stopped]() {
        self->doStop();
        stopped->set_value();
    });
    // A loop that is not running anymore will never execute the task, do not hang on it
    finished.wait_for(std::chrono::seconds(MDNS_STOP_TIMEOUT_S));
}

void MdnsService::publish(const std::string& name, const sockaddr_u& addr) {
    if (!isMdnsName(name)) {
        hlogw("mdns: publish %s is not a .local name", name.c_str());
        return;
    }
    postToLoop([this, name, addr]() { publishInLoop(name, addr); });
}

void MdnsService::publishInLoop(const std::string& name, const sockaddr_u& addr) {
    if (ensureRunning() != 0) {
        hlogw("mdns: publish %s failed, mDNS service is unavailable", name.c_str());
        return;
    }
    std::string key = mdnsNormalizeName(name);
    Announcement announcement;
    announcement.name = key;
    announcement.addr = addr;
    announcement.nextSendMs = hloop_now_ms(loop_->loop());
    announcements_[key] = announcement;
    hlogi("mdns: published %s -> %s", key.c_str(), mdnsAddrString(addr).c_str());
}

void MdnsService::unpublish(const std::string& name) {
    // NOTE: announcements_ may only be touched in the loop thread, the lookup is done there
    std::string key = mdnsNormalizeName(name);
    postToLoop([this, key]() { unpublishInLoop(key); });
}

void MdnsService::unpublishInLoop(const std::string& name) {
    std::string key = mdnsNormalizeName(name);
    auto it = announcements_.find(key);
    if (it == announcements_.end()) return;
    if (running_) {
        // goodbye: the same record with TTL 0 tells responders to drop it right away
        auto msg = makeAddressMessage(key, it->second.addr, 0);
        sendMulticast(msg.encode(), it->second.addr.sa.sa_family);
    }
    announcements_.erase(it);
    hlogi("mdns: unpublished %s", key.c_str());
}

bool MdnsService::isPublished(const std::string& name) const {
    return announcements_.count(mdnsNormalizeName(name)) != 0;
}

void MdnsService::resolve(const std::string& name, ResolveCallback cb, int timeoutMs) {
    if (!isMdnsName(name)) {
        if (cb) cb(name, nullptr);
        return;
    }
    postToLoop([this, name, cb, timeoutMs]() { resolveInLoop(name, cb, timeoutMs); });
}

void MdnsService::resolveInLoop(const std::string& name, const ResolveCallback& cb, int timeoutMs) {
    if (ensureRunning() != 0) {
        hlogw("mdns: resolve %s failed, mDNS service is unavailable", name.c_str());
        if (cb) cb(name, nullptr);
        return;
    }
    std::string key = mdnsNormalizeName(name);
    auto it = queries_.find(key);
    if (it != queries_.end()) {
        // A query for the same name is already on the wire, only chain the callback
        it->second.cbs.push_back(cb);
        hlogi("mdns: resolve %s already pending", key.c_str());
        return;
    }
    if (timeoutMs <= 0) timeoutMs = MDNS_RESOLVE_TIMEOUT_MS;

    Query query;
    query.name = key;
    query.cbs.push_back(cb);
    query.nextSendMs = hloop_now_ms(loop_->loop());
    query.expireMs = query.nextSendMs + timeoutMs;
    queries_[key] = query;
    sendQuery(key);
    hlogi("mdns: resolving %s timeout=%dms", key.c_str(), timeoutMs);
}

void MdnsService::cancel(const std::string& name) {
    // NOTE: queries_ may only be touched in the loop thread, the lookup is done there
    std::string key = mdnsNormalizeName(name);
    postToLoop([this, key]() { cancelInLoop(key); });
}

void MdnsService::cancelInLoop(const std::string& name) {
    auto it = queries_.find(mdnsNormalizeName(name));
    if (it == queries_.end()) return;
    auto cbs = std::move(it->second.cbs);
    queries_.erase(it);
    for (auto& cb : cbs) {
        if (cb) cb(name, nullptr);
    }
}

bool MdnsService::isResolving(const std::string& name) const {
    return queries_.count(mdnsNormalizeName(name)) != 0;
}

void MdnsService::sendQuery(const std::string& name) {
    MdnsMessage msg;
    MdnsQuestion question;
    question.name = name;
    question.type = MDNS_TYPE_A;
    // Ask for a unicast reply when we could not bind the well known port, otherwise
    // responders multicast the answer to 5353 where we are not listening.
    question.unicastResponse = !port_bound_;
    msg.questions.push_back(question);
    sendMulticast(msg.encode(), AF_INET);

    if (ios_.size() > 1) {
        // the ipv6 socket queries for AAAA records of the same name
        msg.questions.clear();
        question.type = MDNS_TYPE_AAAA;
        msg.questions.push_back(question);
        sendMulticast(msg.encode(), AF_INET6);
    }
}

int MdnsService::sendUnicast(const std::vector<uint8_t>& data, const struct sockaddr* to, int family) {
    if (data.empty() || !to) return -1;
    for (auto io : ios_) {
        if (!io) continue;
        struct sockaddr* localaddr = hio_localaddr(io);
        if (localaddr && localaddr->sa_family != family) continue;
        return hio_sendto(io, data.data(), data.size(), (struct sockaddr*)to);
    }
    return -1;
}

int MdnsService::sendMulticast(const std::vector<uint8_t>& data, int family) {
    if (data.empty() || ios_.empty()) return -1;
    hio_t* io = nullptr;
    for (auto candidate : ios_) {
        struct sockaddr* localaddr = candidate ? hio_localaddr(candidate) : nullptr;
        if (localaddr && localaddr->sa_family == family) {
            io = candidate;
            break;
        }
    }
    if (!io) return -1;

    sockaddr_u dest;
    memset(&dest, 0, sizeof(dest));
    sockaddr_set_ipport(&dest, family == AF_INET6 ? MDNS_GROUP_IPV6 : MDNS_GROUP_IPV4, MDNS_PORT);

    if (family != AF_INET || interfaces4_.empty()) {
        return hio_sendto(io, data.data(), data.size(), &dest.sa);
    }
    // Multi-homed host: send one copy per interface, otherwise the packets leave
    // on the interface selected by the routing table only.
    int ret = -1;
    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    for (const auto& ip : interfaces4_) {
        if (inet_pton(AF_INET, ip.c_str(), &mreq.imr_interface) != 1) continue;
        setsockopt(hio_fd(io), IPPROTO_IP, IP_MULTICAST_IF, (const char*)&mreq.imr_interface, sizeof(struct in_addr));
        ret = hio_sendto(io, data.data(), data.size(), &dest.sa);
    }
    return ret;
}

void MdnsService::onRecv(hio_t* io, void* buf, int readbytes) {
    MdnsMessage msg;
    if (!MdnsMessage::decode((const uint8_t*)buf, readbytes, &msg)) return;
    handleMessage(msg, hio_peeraddr(io));
}

void MdnsService::handleMessage(const MdnsMessage& msg, const struct sockaddr* from) {
    if (msg.response) {
        // Collect the hits first: a resolve callback may start a new query and must not
        // invalidate the container that is being walked.
        std::vector<std::pair<std::string, sockaddr_u>> hits;
        for (const auto& kv : queries_) {
            sockaddr_u addr;
            if (msg.findAddress(kv.first, &addr)) {
                hits.push_back(std::make_pair(kv.first, addr));
            }
        }
        for (auto& hit : hits) {
            auto it = queries_.find(hit.first);
            if (it == queries_.end()) continue;
            auto cbs = std::move(it->second.cbs);
            queries_.erase(it);
            hlogi("mdns: resolved %s -> %s", hit.first.c_str(), mdnsAddrString(hit.second).c_str());
            for (auto& cb : cbs) {
                if (cb) cb(hit.first, &hit.second);
            }
        }
        return;
    }

    if (announcements_.empty()) return;
    for (const auto& question : msg.questions) {
        answerQuery(question, from);
    }
}

void MdnsService::answerQuery(const MdnsQuestion& question, const struct sockaddr* from) {
    if (question.type != MDNS_TYPE_A && question.type != MDNS_TYPE_AAAA && question.type != MDNS_TYPE_ANY) return;
    auto it = announcements_.find(question.name);
    if (it == announcements_.end()) return;
    const auto& addr = it->second.addr;
    if (question.type != MDNS_TYPE_ANY && familyOfRecordType(question.type) != addr.sa.sa_family) return;

    MdnsMessage msg = makeAddressMessage(it->first, addr, MDNS_TTL);
    MdnsQuestion echoed = question;
    echoed.unicastResponse = false;  // the QU bit is a request, not part of the question
    msg.questions.push_back(echoed);
    auto data = msg.encode();

    // Unicast the answer back to the querier when it asked for it (QU bit) or when it
    // sent from a port other than 5353 (legacy queriers cannot receive multicast).
    int family = addr.sa.sa_family;
    if (question.unicastResponse || (from && sockaddr_port((sockaddr_u*)from) != MDNS_PORT)) {
        sendUnicast(data, from, family);
    } else {
        sendMulticast(data, family);
    }
}

void MdnsService::onTick() {
    if (!running_ || !loop_) return;
    uint64_t now = hloop_now_ms(loop_->loop());

    // Expired queries are reported after the walk, the callback may touch queries_
    std::vector<std::pair<std::string, std::vector<ResolveCallback>>> expired;
    for (auto it = queries_.begin(); it != queries_.end();) {
        auto& query = it->second;
        if (now >= query.expireMs) {
            expired.push_back(std::make_pair(query.name, std::move(query.cbs)));
            it = queries_.erase(it);
            continue;
        }
        if (query.sent < MDNS_MAX_QUERIES && now >= query.nextSendMs) {
            sendQuery(query.name);
            query.sent++;
            query.nextSendMs += MDNS_QUERY_INTERVAL;
        }
        ++it;
    }
    for (auto& item : expired) {
        hlogw("mdns: resolve %s timed out", item.first.c_str());
        for (auto& cb : item.second) {
            if (cb) cb(item.first, nullptr);
        }
    }

    for (auto it = announcements_.begin(); it != announcements_.end();) {
        auto& announcement = it->second;
        if (announcement.sent >= MDNS_MAX_ANNOUNCES) {
            // Announced often enough, keep answering queries from the table but stop the
            // gratuitous traffic.
            it->second.sent = MDNS_MAX_ANNOUNCES;
            ++it;
            continue;
        }
        if (now >= announcement.nextSendMs) {
            auto msg = makeAddressMessage(announcement.name, announcement.addr, MDNS_TTL);
            sendMulticast(msg.encode(), announcement.addr.sa.sa_family);
            announcement.sent++;
            announcement.nextSendMs = now + MDNS_ANNOUNCE_INTERVAL;
        }
        ++it;
    }
}

} // namespace ice
