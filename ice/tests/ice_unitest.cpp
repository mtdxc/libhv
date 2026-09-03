#include <thread>
#include <chrono>
#include <atomic>
#include <future>
#include <memory>
#include <cstring>
#include <gtest/gtest.h>
#include "ice/ice.h"
#include "ice/rtp/RtpPacket.h"
#include "ice/rtp/RtcpPacket.h"
#include "ice/rtp/RtpMap.h"
#include "ice/sdp/Sdp.h"
#include "ice/Frame.h"

using namespace ice;
using namespace rtcp;

// ────────────────────────────────────────────────────────────
// STUN Message Tests
// ────────────────────────────────────────────────────────────

TEST(StunMessage, EncodeDecodeBasic) {
    StunMessage msg(STUN_METHOD_BINDING, STUN_CLASS_REQUEST);
    msg.addUsername("user1:user2");
    msg.addPriority(12345678);
    msg.addIceControlling(0x1122334455667788ULL);
    msg.addUseCandidate();

    auto encoded = msg.encode();
    ASSERT_GT(encoded.size(), 0u);

    StunMessage decoded;
    bool ok = StunMessage::decode(encoded.data(), encoded.size(), &decoded);
    ASSERT_TRUE(ok);

    EXPECT_EQ(decoded.getUsername(), "user1:user2");
    EXPECT_EQ(decoded.getPriority(), 12345678u);
    EXPECT_TRUE(decoded.hasUseCandidate());
    EXPECT_EQ(decoded.getIceControlling(), 0x1122334455667788ULL);
    EXPECT_TRUE(StunMessage::isStun(encoded.data(), encoded.size()));
}

TEST(StunMessage, EncodeDecodeWithAuth) {
    StunMessage msg(STUN_METHOD_BINDING, STUN_CLASS_REQUEST);
    msg.addUsername("localufrag:remoteufrag");
    msg.addPriority(99999);

    std::string password = "mysecretpassword";
    auto encoded = msg.encodeWithAuth(password);
    ASSERT_GT(encoded.size(), 0u);

    StunMessage decoded;
    bool ok = StunMessage::decode(encoded.data(), encoded.size(), &decoded);
    ASSERT_TRUE(ok);

    EXPECT_TRUE(decoded.verifyIntegrity(password));
    EXPECT_FALSE(decoded.verifyIntegrity("wrongpassword"));
    EXPECT_TRUE(decoded.verifyFingerprint());
}

TEST(StunMessage, XorMappedAddress) {
    struct sockaddr_in addr4;
    memset(&addr4, 0, sizeof(addr4));
    addr4.sin_family = AF_INET;
    addr4.sin_port = htons(12345);
    addr4.sin_addr.s_addr = htonl(0xC0A80164); // 192.168.1.100

    StunMessage msg(STUN_METHOD_BINDING, STUN_CLASS_SUCCESS_RESPONSE);
    msg.addXorMappedAddress((struct sockaddr*)&addr4);

    auto encoded = msg.encode();
    StunMessage decoded;
    StunMessage::decode(encoded.data(), encoded.size(), &decoded);

    struct sockaddr_storage result;
    bool ok = decoded.getXorMappedAddress(&result);
    ASSERT_TRUE(ok);

    struct sockaddr_in* res4 = (struct sockaddr_in*)&result;
    EXPECT_EQ(ntohs(res4->sin_port), 12345);
    EXPECT_EQ(ntohl(res4->sin_addr.s_addr), 0xC0A80164u);
}

TEST(StunMessage, ErrorResponse) {
    StunMessage msg(STUN_METHOD_BINDING, STUN_CLASS_ERROR_RESPONSE);
    msg.addErrorCode(STUN_ERROR_UNAUTHORIZED, "Unauthorized");

    auto encoded = msg.encode();
    StunMessage decoded;
    StunMessage::decode(encoded.data(), encoded.size(), &decoded);

    uint16_t code = 0;
    std::string reason;
    decoded.getErrorCode(&code, &reason);
    EXPECT_EQ(code, STUN_ERROR_UNAUTHORIZED);
    EXPECT_EQ(reason, "Unauthorized");
}

// ────────────────────────────────────────────────────────────
// ICE Candidate Tests
// ────────────────────────────────────────────────────────────

TEST(IceCandidate, PriorityOrder) {
    uint32_t hostPri = computeCandidatePriority(CandidateType::Host, 65535, 1);
    uint32_t srflxPri = computeCandidatePriority(CandidateType::ServerReflexive, 65535, 1);
    uint32_t prflxPri = computeCandidatePriority(CandidateType::PeerReflexive, 65535, 1);
    uint32_t relayPri = computeCandidatePriority(CandidateType::Relay, 65535, 1);

    // RFC 8445: host > peer-reflexive > server-reflexive > relay
    EXPECT_GT(hostPri, prflxPri);
    EXPECT_GT(prflxPri, srflxPri);
    EXPECT_GT(srflxPri, relayPri);
}

TEST(IceCandidate, TypeString) {
    EXPECT_STREQ(IceCandidate::typeString(CandidateType::Host), "host");
    EXPECT_STREQ(IceCandidate::typeString(CandidateType::ServerReflexive), "srflx");
    EXPECT_STREQ(IceCandidate::typeString(CandidateType::PeerReflexive), "prflx");
    EXPECT_STREQ(IceCandidate::typeString(CandidateType::Relay), "relay");
}

TEST(IceCandidate, CreateAndUpdate) {
    IceCandidate cand;
    cand.type = CandidateType::Host;
    cand.protocol = TransportProtocol::UDP;
    cand.componentId = 1;

    sockaddr_u addr;
    sockaddr_set_ipport(&addr, "192.168.1.100", 5000);
    memcpy(&cand.addr, &addr, sizeof(sockaddr_u));
    memcpy(&cand.baseAddr, &addr, sizeof(sockaddr_u));

    cand.update();

    EXPECT_FALSE(cand.foundation.empty());
    EXPECT_GT(cand.priority, 0u);
    EXPECT_FALSE(cand.addrString().empty());
}

TEST(IceCandidate, FromSdp) {
    std::string sdp = "a=candidate:123456 1 udp 2130706431 192.168.1.1 5000 typ host";
    IceCandidate cand;
    bool ok = cand.fromSdp(sdp);
    ASSERT_TRUE(ok);

    // Note: fromSdp may include "a=candidate:" prefix in foundation
    EXPECT_EQ(cand.foundation, "123456");
    EXPECT_EQ(cand.componentId, 1);
    EXPECT_EQ(cand.protocol, TransportProtocol::UDP);
    EXPECT_EQ(cand.priority, 2130706431u);
    EXPECT_EQ(cand.type, CandidateType::Host);
}

TEST(IceCandidate, ToSdpRoundTrip) {
    IceCandidate cand;
    cand.foundation = "abc123";
    cand.componentId = 1;
    cand.protocol = TransportProtocol::UDP;
    cand.priority = 2130706431;
    cand.type = CandidateType::Host;
    sockaddr_set_ipport(&cand.addr, "10.0.0.1", 8080);
    memcpy(&cand.baseAddr, &cand.addr, sizeof(sockaddr_u));
    cand.update();

    std::string sdp = cand.toSdp();
    EXPECT_NE(sdp.find("typ host"), std::string::npos);
    EXPECT_NE(sdp.find("10.0.0.1"), std::string::npos);
    EXPECT_NE(sdp.find("8080"), std::string::npos);
}

TEST(IceCandidate, TcpHostActiveSdpRoundTrip) {
    IceCandidate cand;
    cand.componentId = 1;
    cand.protocol = TransportProtocol::TCP;
    cand.tcpType = TcpType::Active;
    cand.type = CandidateType::Host;
    sockaddr_set_ipport(&cand.addr, "10.10.10.8", 45678);
    memcpy(&cand.baseAddr, &cand.addr, sizeof(sockaddr_u));
    cand.update();

    std::string sdp = cand.toSdp();
    EXPECT_NE(sdp.find(" typ host"), std::string::npos);
    EXPECT_NE(sdp.find(" tcptype active"), std::string::npos);

    IceCandidate parsed;
    ASSERT_TRUE(parsed.fromSdp(sdp));
    EXPECT_EQ(parsed.protocol, TransportProtocol::TCP);
    EXPECT_EQ(parsed.tcpType, TcpType::Active);
    EXPECT_EQ(parsed.type, CandidateType::Host);
}

// ────────────────────────────────────────────────────────────
// CandidatePair Tests
// ────────────────────────────────────────────────────────────

TEST(CandidatePair, ComputePriority) {
    uint64_t p1 = CandidatePair::computePairPriority(
        2130706431, 2130706431, IceRole::Controlling);
    uint64_t p2 = CandidatePair::computePairPriority(
        1694498815, 1694498815, IceRole::Controlling);

    // Higher candidate priorities should yield higher pair priorities
    EXPECT_GT(p1, p2);
}

TEST(CandidatePair, Comparison) {
    CandidatePair a, b;
    a.priority = 100;
    b.priority = 200;

    EXPECT_TRUE(b > a);
    EXPECT_TRUE(a < b);
}

TEST(CandidatePair, ToString) {
    CandidatePair pair;
    pair.local.type = CandidateType::Host;
    sockaddr_set_ipport(&pair.local.addr, "192.168.1.1", 5000);
    pair.local.update();

    pair.remote.type = CandidateType::ServerReflexive;
    sockaddr_set_ipport(&pair.remote.addr, "1.2.3.4", 6000);
    pair.remote.update();

    std::string s = pair.toString();
    EXPECT_NE(s.find("host"), std::string::npos);
    EXPECT_NE(s.find("srflx"), std::string::npos);
}

// ────────────────────────────────────────────────────────────
// IceCheckList Tests
// ────────────────────────────────────────────────────────────

TEST(IceCheckList, AddAndSort) {
    IceCheckList cl;

    for (int i = 0; i < 5; ++i) {
        auto pair = std::make_shared<CandidatePair>();
        pair->priority = static_cast<uint64_t>(i * 100);
        pair->state = PairState::Waiting;
        cl.addPair(pair);
    }

    cl.sort();
    auto pairs = cl.pairs();
    // After sort, priorities should be descending
    for (size_t i = 1; i < pairs.size(); ++i) {
        EXPECT_GE(pairs[i - 1]->priority, pairs[i]->priority);
    }
}

TEST(IceCheckList, GetNextPair) {
    IceCheckList cl;

    // Add a frozen pair and a waiting pair
    auto frozen = std::make_shared<CandidatePair>();
    frozen->priority = 500;
    frozen->state = PairState::Frozen;
    cl.addPair(frozen);

    auto waiting = std::make_shared<CandidatePair>();
    waiting->priority = 300;
    waiting->state = PairState::Waiting;
    cl.addPair(waiting);

    // getNextPair should return the waiting pair first (even though lower priority)
    auto next = cl.getNextPair();
    ASSERT_TRUE(next != nullptr);
    EXPECT_EQ(next->priority, 300u);
    EXPECT_EQ(next->state, PairState::InProgress);

    // Next call should return the frozen pair (unfrozen)
    next = cl.getNextPair();
    ASSERT_TRUE(next != nullptr);
    EXPECT_EQ(next->priority, 500u);
}

TEST(IceCheckList, TriggeredChecks) {
    IceCheckList cl;

    auto pair = std::make_shared<CandidatePair>();
    pair->priority = 100;
    pair->state = PairState::Waiting;
    cl.addPair(pair);

    cl.addTriggeredCheck(pair);

    // Triggered checks should be returned first
    auto next = cl.getNextPair();
    ASSERT_TRUE(next != nullptr);
    EXPECT_EQ(next->priority, 100u);
}

TEST(IceCheckList, TriggeredCheckCanonicalizesByAddress) {
    IceCheckList cl;

    auto canonical = std::make_shared<CandidatePair>();
    canonical->priority = 777;
    canonical->state = PairState::Waiting;
    sockaddr_set_ipport(&canonical->local.addr, "192.168.1.11", 5001);
    sockaddr_set_ipport(&canonical->remote.addr, "192.168.1.21", 6001);
    cl.addPair(canonical);

    // Different shared_ptr but same 5-tuple address pair.
    auto shadow = std::make_shared<CandidatePair>();
    shadow->priority = 1;
    shadow->state = PairState::Waiting;
    sockaddr_set_ipport(&shadow->local.addr, "192.168.1.11", 5001);
    sockaddr_set_ipport(&shadow->remote.addr, "192.168.1.21", 6001);
    cl.addTriggeredCheck(shadow);

    auto next = cl.getNextPair();
    ASSERT_TRUE(next != nullptr);
    EXPECT_EQ(next, canonical);
    EXPECT_EQ(canonical->state, PairState::InProgress);
}

TEST(IceCheckList, TriggeredCheckSkipsAlreadyInProgressCanonicalPair) {
    IceCheckList cl;

    auto canonical = std::make_shared<CandidatePair>();
    canonical->state = PairState::InProgress;
    sockaddr_set_ipport(&canonical->local.addr, "192.168.1.12", 5002);
    sockaddr_set_ipport(&canonical->remote.addr, "192.168.1.22", 6002);
    cl.addPair(canonical);

    auto shadow = std::make_shared<CandidatePair>();
    shadow->state = PairState::Waiting;
    sockaddr_set_ipport(&shadow->local.addr, "192.168.1.12", 5002);
    sockaddr_set_ipport(&shadow->remote.addr, "192.168.1.22", 6002);
    cl.addTriggeredCheck(shadow);

    auto next = cl.getNextPair();
    EXPECT_EQ(next, nullptr);
}

TEST(IceCheckList, IsCompleteFalseWhenTriggeredQueueNotEmpty) {
    IceCheckList cl;

    auto pair = std::make_shared<CandidatePair>();
    pair->state = PairState::Succeeded;
    sockaddr_set_ipport(&pair->local.addr, "192.168.1.13", 5003);
    sockaddr_set_ipport(&pair->remote.addr, "192.168.1.23", 6003);
    cl.addPair(pair);

    auto shadow = std::make_shared<CandidatePair>();
    shadow->state = PairState::Waiting;
    sockaddr_set_ipport(&shadow->local.addr, "192.168.1.13", 5003);
    sockaddr_set_ipport(&shadow->remote.addr, "192.168.1.23", 6003);
    cl.addTriggeredCheck(shadow);

    EXPECT_FALSE(cl.isComplete());
}

TEST(IceCheckList, FindByAddresses) {
    IceCheckList cl;

    auto pair = std::make_shared<CandidatePair>();
    sockaddr_set_ipport(&pair->local.addr, "192.168.1.10", 5000);
    sockaddr_set_ipport(&pair->remote.addr, "192.168.1.20", 6000);
    cl.addPair(pair);

    sockaddr_u localAddr;
    sockaddr_u remoteAddr;
    sockaddr_set_ipport(&localAddr, "192.168.1.10", 5000);
    sockaddr_set_ipport(&remoteAddr, "192.168.1.20", 6000);

    auto found = cl.findByAddresses(localAddr, remoteAddr);
    ASSERT_TRUE(found != nullptr);
    EXPECT_EQ(found, pair);
}

TEST(IceCheckList, PruneKeepsHighestPriorityFoundationPair) {
    IceCheckList cl;

    auto low = std::make_shared<CandidatePair>();
    low->local.foundation = "local-a";
    low->remote.foundation = "remote-a";
    low->priority = 100;
    cl.addPair(low);

    auto high = std::make_shared<CandidatePair>();
    high->local.foundation = "local-a";
    high->remote.foundation = "remote-a";
    high->priority = 300;
    cl.addPair(high);

    auto unique = std::make_shared<CandidatePair>();
    unique->local.foundation = "local-b";
    unique->remote.foundation = "remote-b";
    unique->priority = 200;
    cl.addPair(unique);

    cl.prune();

    ASSERT_EQ(cl.size(), 2u);
    EXPECT_TRUE(std::find(cl.pairs().begin(), cl.pairs().end(), high) != cl.pairs().end());
    EXPECT_TRUE(std::find(cl.pairs().begin(), cl.pairs().end(), unique) != cl.pairs().end());
}

TEST(IceCheckList, AllFailed) {
    IceCheckList cl;

    auto p1 = std::make_shared<CandidatePair>();
    p1->state = PairState::Failed;
    cl.addPair(p1);

    auto p2 = std::make_shared<CandidatePair>();
    p2->state = PairState::Failed;
    cl.addPair(p2);

    EXPECT_TRUE(cl.allFailed());

    // Add a non-failed pair
    auto p3 = std::make_shared<CandidatePair>();
    p3->state = PairState::Waiting;
    cl.addPair(p3);

    EXPECT_FALSE(cl.allFailed());
}

TEST(IceCheckList, IsComplete) {
    IceCheckList cl;

    auto p1 = std::make_shared<CandidatePair>();
    p1->state = PairState::Succeeded;
    cl.addPair(p1);

    // Succeeded pair means complete
    EXPECT_TRUE(cl.isComplete());
}

// ────────────────────────────────────────────────────────────
// SDP Tests
// ────────────────────────────────────────────────────────────

TEST(IceSdp, GenerateAttributes) {
    std::vector<IceCandidate> candidates;

    IceCandidate cand;
    cand.foundation = "123456";
    cand.componentId = 1;
    cand.protocol = TransportProtocol::UDP;
    cand.priority = 2130706431;
    cand.type = CandidateType::Host;
    sockaddr_set_ipport(&cand.addr, "192.168.1.1", 5000);
    candidates.push_back(cand);

    std::string sdp = IceSdp::generateAttributes("myufrag", "mypassword123456789012", candidates, false, true);

    EXPECT_NE(sdp.find("a=ice-ufrag:myufrag"), std::string::npos);
    EXPECT_NE(sdp.find("a=ice-pwd:mypassword123456789012"), std::string::npos);
    EXPECT_NE(sdp.find("a=candidate:"), std::string::npos);
    EXPECT_NE(sdp.find("192.168.1.1"), std::string::npos);
}

TEST(IceSdp, ParseAttributes) {
    std::string sdp =
        "a=ice-ufrag:testufrag\n"
        "a=ice-pwd:testpassword123456789012\n"
        "a=candidate:abc123 1 udp 2130706431 10.0.0.1 8080 typ host\n";

    auto result = IceSdp::parseAttributes(sdp);

    EXPECT_EQ(result.ufrag, "testufrag");
    EXPECT_EQ(result.pwd, "testpassword123456789012");
    ASSERT_EQ(result.candidates.size(), 1u);
    EXPECT_EQ(result.candidates[0].foundation, "abc123");
    EXPECT_EQ(result.candidates[0].componentId, 1);
    EXPECT_EQ(result.candidates[0].priority, 2130706431u);
    EXPECT_EQ(result.candidates[0].type, CandidateType::Host);
}

TEST(IceSdp, RoundTrip) {
    std::vector<IceCandidate> candidates;

    IceCandidate cand;
    cand.foundation = "xyz789";
    cand.componentId = 1;
    cand.protocol = TransportProtocol::UDP;
    cand.priority = 2130706431;
    cand.type = CandidateType::Host;
    sockaddr_set_ipport(&cand.addr, "172.16.0.5", 9090);
    candidates.push_back(cand);

    std::string sdp = IceSdp::generateAttributes("ufrag1", "pwd1pwd1pwd1pwd1pwd1pw", candidates, false, true);
    auto result = IceSdp::parseAttributes(sdp);

    EXPECT_EQ(result.ufrag, "ufrag1");
    EXPECT_EQ(result.pwd, "pwd1pwd1pwd1pwd1pwd1pw");
    ASSERT_EQ(result.candidates.size(), 1u);
    EXPECT_EQ(result.candidates[0].foundation, "xyz789");
    EXPECT_EQ(result.candidates[0].priority, 2130706431u);
    EXPECT_EQ(result.candidates[0].type, CandidateType::Host);
}

// ────────────────────────────────────────────────────────────
// IceAgent & IceSession Tests
// ────────────────────────────────────────────────────────────

class IceAgentTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.udpPort = 0; // ephemeral
        config_.gatherTcp = false;
    }

    IceConfig config_;
};

TEST_F(IceAgentTest, CreateAndStart) {
    IceAgent agent;
    agent.setConfig(config_);
    int ret = agent.start();
    EXPECT_EQ(ret, 0);
    EXPECT_GT(agent.udpPort(), 0);
    EXPECT_TRUE(agent.isRunning());
    agent.stop();
}

TEST_F(IceAgentTest, CreateSession) {
    IceAgent agent;
    agent.setConfig(config_);
    agent.start();

    auto session = agent.createSession();
    ASSERT_TRUE(session != nullptr);
    EXPECT_FALSE(session->localUfrag().empty());
    EXPECT_FALSE(session->localPwd().empty());
    EXPECT_EQ(session->role(), IceRole::Controlling); // default

    agent.destroySession(session);
    agent.stop();
}

TEST_F(IceAgentTest, MultipleSessions) {
    IceAgent agent;
    agent.setConfig(config_);
    agent.start();

    auto s1 = agent.createSession();
    auto s2 = agent.createSession();

    // Sessions should have different ufrags
    EXPECT_NE(s1->localUfrag(), s2->localUfrag());

    agent.destroySession(s1);
    agent.destroySession(s2);
    agent.stop();
}

TEST_F(IceAgentTest, SessionGathering) {
    IceAgent agent;
    config_.gatherTcp = false;
    agent.setConfig(config_);
    agent.start();

    auto session = agent.createSession();

    std::vector<IceCandidate> gatheredCandidates;
    session->onLocalCandidate = [&gatheredCandidates](const IceCandidate& cand) {
        gatheredCandidates.push_back(cand);
    };

    session->gatherCandidates();

    // Wait for gathering to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Should have at least one host candidate
    bool hasHost = false;
    for (const auto& c : gatheredCandidates) {
        if (c.type == CandidateType::Host) {
            hasHost = true;
            break;
        }
    }
    EXPECT_TRUE(hasHost);

    agent.destroySession(session);
    agent.stop();
}

TEST_F(IceAgentTest, TcpConnectedDoesNotDuplicateActiveLocalCandidate) {
    IceAgent agent;
    config_.gatherTcp = true;
    config_.gatherSrflx = false;
    config_.gatherRelay = false;
    agent.setConfig(config_);
    ASSERT_EQ(agent.start(), 0);

    auto session = agent.createSession();
    ASSERT_TRUE(session != nullptr);

    std::vector<IceCandidate> gathered;
    session->onLocalCandidate = [&gathered](const IceCandidate& cand) {
        gathered.push_back(cand);
    };

    session->gatherCandidates();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    bool hasTcpPassive = false;
    bool hasTcpActive = false;
    for (const auto& cand : gathered) {
        if (cand.protocol != TransportProtocol::TCP) continue;
        if (cand.tcpType == TcpType::Passive) hasTcpPassive = true;
        if (cand.tcpType == TcpType::Active) hasTcpActive = true;
    }

    EXPECT_TRUE(hasTcpPassive);
    // WebRTC-like lifecycle: active TCP candidate is connection/pair scoped,
    // not gathered into the static local candidate list.
    EXPECT_FALSE(hasTcpActive);

    agent.destroySession(session);
    agent.stop();
}

// ────────────────────────────────────────────────────────────
// State Transition Tests
// ────────────────────────────────────────────────────────────

TEST(IceSession, StateString) {
    EXPECT_STREQ(iceStateString(IceState::New), "new");
    EXPECT_STREQ(iceStateString(IceState::Gathering), "gathering");
    EXPECT_STREQ(iceStateString(IceState::Checking), "checking");
    EXPECT_STREQ(iceStateString(IceState::Connected), "connected");
    EXPECT_STREQ(iceStateString(IceState::Completed), "completed");
    EXPECT_STREQ(iceStateString(IceState::Failed), "failed");
    EXPECT_STREQ(iceStateString(IceState::Closed), "closed");
}

TEST(IceSession, RoleString) {
    EXPECT_STREQ(iceRoleString(IceRole::Controlling), "controlling");
    EXPECT_STREQ(iceRoleString(IceRole::Controlled), "controlled");
}

TEST(IceSession, PairStateString) {
    EXPECT_STREQ(pairStateString(PairState::Frozen), "frozen");
    EXPECT_STREQ(pairStateString(PairState::Waiting), "waiting");
    EXPECT_STREQ(pairStateString(PairState::InProgress), "in-progress");
    EXPECT_STREQ(pairStateString(PairState::Succeeded), "succeeded");
    EXPECT_STREQ(pairStateString(PairState::Failed), "failed");
}

// ────────────────────────────────────────────────────────────
// Packet Classification Tests
// ────────────────────────────────────────────────────────────

TEST(PacketClassification, StunPacket) {
    // STUN message starts with 0x00 or 0x01 in first byte (class 0x00-0x03)
    uint8_t stunData[] = { 0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42 };
    EXPECT_EQ(classifyPacket(stunData, sizeof(stunData)), PacketType::STUN);
}

TEST(PacketClassification, TurnChannelPacket) {
    // TURN ChannelData starts with 0x40-0x7F
    uint8_t channelData[] = { 0x40, 0x01, 0x00, 0x10 };
    EXPECT_EQ(classifyPacket(channelData, sizeof(channelData)), PacketType::TURN_CHANNEL);
}

TEST(PacketClassification, DataPacket) {
    // Regular data doesn't start with STUN or Channel markers
    uint8_t data[] = { 0x80, 0x01, 0x02, 0x03 };
    EXPECT_EQ(classifyPacket(data, sizeof(data)), PacketType::DATA);
}

// ────────────────────────────────────────────────────────────
// TurnState Tests
// ────────────────────────────────────────────────────────────

TEST(TurnState, ToString) {
    EXPECT_STREQ(turnStateString(TurnState::Idle), "Idle");
    EXPECT_STREQ(turnStateString(TurnState::Allocating), "Allocating");
    EXPECT_STREQ(turnStateString(TurnState::Allocated), "Allocated");
    EXPECT_STREQ(turnStateString(TurnState::Refreshing), "Refreshing");
    EXPECT_STREQ(turnStateString(TurnState::Failed), "Failed");
}

// ────────────────────────────────────────────────────────────
// RtpPacket Tests
// ────────────────────────────────────────────────────────────

TEST(RtpPacket, CreateBasic) {
    auto pkt = RtpPacket::create(CodecH264, 96, 0x12345678, 100, 5000, true);
    ASSERT_TRUE(pkt != nullptr);

    EXPECT_EQ(pkt->getVersion(), 2);
    EXPECT_EQ(pkt->getPayloadType(), 96);
    EXPECT_EQ(pkt->getSeq(), 100);
    EXPECT_EQ(pkt->getSSRC(), 0x12345678u);
    EXPECT_TRUE(pkt->getMarker());
    EXPECT_EQ(pkt->getPayloadSize(), 0u);
    EXPECT_EQ(pkt->getHeaderSize(), 12u);
    EXPECT_EQ(pkt->codec, CodecH264);
}

TEST(RtpPacket, CreateWithPayload) {
    uint8_t payload[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04 };
    auto pkt = RtpPacket::create(CodecOpus, 111, 0xAABBCCDD, 42, 1000, false,
                                  payload, sizeof(payload));
    ASSERT_TRUE(pkt != nullptr);

    EXPECT_EQ(pkt->getPayloadType(), 111);
    EXPECT_EQ(pkt->getSeq(), 42);
    EXPECT_FALSE(pkt->getMarker());
    EXPECT_EQ(pkt->getPayloadSize(), sizeof(payload));
    EXPECT_EQ(memcmp(pkt->getPayload(), payload, sizeof(payload)), 0);
    EXPECT_EQ(pkt->size(), 12 + sizeof(payload));
}

TEST(RtpPacket, ParseFromRaw) {
    // Build a minimal RTP packet: V=2, PT=96, seq=1, ts=160, ssrc=0x11223344
    uint8_t raw[20] = {};
    raw[0] = 0x80; // V=2, P=0, X=0, CC=0
    raw[1] = 96;   // M=0, PT=96
    raw[2] = 0x00; raw[3] = 0x01; // seq=1
    // timestamp = 160 (0x000000A0)
    raw[4] = 0x00; raw[5] = 0x00; raw[6] = 0x00; raw[7] = 0xA0;
    // ssrc = 0x11223344
    raw[8] = 0x11; raw[9] = 0x22; raw[10] = 0x33; raw[11] = 0x44;
    // payload
    raw[12] = 0xCA; raw[13] = 0xFE;

    RtpPacket pkt;
    ASSERT_TRUE(pkt.parse(raw, 14));

    EXPECT_EQ(pkt.getVersion(), 2);
    EXPECT_EQ(pkt.getPayloadType(), 96);
    EXPECT_EQ(pkt.getSeq(), 1);
    EXPECT_EQ(pkt.getTimestamp(), 160u);
    EXPECT_EQ(pkt.getSSRC(), 0x11223344u);
    EXPECT_EQ(pkt.getPayloadSize(), 2u);
}

TEST(RtpPacket, ParseRejectsInvalid) {
    // Too short
    uint8_t tiny[4] = { 0x80, 0x60, 0x00, 0x01 };
    RtpPacket pkt;
    EXPECT_FALSE(pkt.parse(tiny, sizeof(tiny)));

    // Wrong version (V=0)
    uint8_t bad[12] = {};
    bad[0] = 0x00; // V=0
    EXPECT_FALSE(pkt.parse(bad, 12));
}

TEST(RtpPacket, SettersAndGetters) {
    auto pkt = RtpPacket::create(CodecH264, 96, 0, 0, 0, false);
    pkt->setSeq(65535);
    pkt->setTimestamp(0xFFFFFFFF);
    pkt->setSSRC(0xDEADBEEF);
    pkt->setPayloadType(127);
    pkt->setMarker(true);

    EXPECT_EQ(pkt->getSeq(), 65535);
    EXPECT_EQ(pkt->getTimestamp(), 0xFFFFFFFFu);
    EXPECT_EQ(pkt->getSSRC(), 0xDEADBEEFu);
    EXPECT_EQ(pkt->getPayloadType(), 127);
    EXPECT_TRUE(pkt->getMarker());
}

TEST(RtpPacket, Extension) {
    auto pkt = RtpPacket::create(CodecH264, 96, 0x12345678, 1, 1000, false);
    EXPECT_FALSE(pkt->hasExtension());

    uint8_t extData[] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
    pkt->setExtension(0xBEDE, extData, sizeof(extData));

    EXPECT_TRUE(pkt->hasExtension());
    EXPECT_EQ(pkt->getHeaderSize(), 12 + 4 + 8);  // base + ext_header + ext_data
    EXPECT_EQ(pkt->getPayloadSize(), 0u);

    pkt->removeExtension();
    EXPECT_FALSE(pkt->hasExtension());
    EXPECT_EQ(pkt->getHeaderSize(), 12u);
}

TEST(RtpPacket, Padding) {
    uint8_t payload[] = { 0x01, 0x02, 0x03, 0x04 };
    auto pkt = RtpPacket::create(CodecH264, 96, 0, 1, 0, false, payload, 4);
    EXPECT_FALSE(pkt->hasPadding());

    pkt->setPadding(8);
    EXPECT_TRUE(pkt->hasPadding());
    // Total size = 12 (header) + 4 (payload) + 8 (padding)
    EXPECT_EQ(pkt->size(), 24u);
    // Payload size should exclude padding
    EXPECT_EQ(pkt->getPayloadSize(), 4u);

    pkt->removePadding();
    EXPECT_FALSE(pkt->hasPadding());
    EXPECT_EQ(pkt->size(), 16u);  // 12 + 4
}

TEST(RtpPacket, RtxEncodeDecode) {
    // Create a normal RTP packet
    uint8_t payload[] = { 0xAA, 0xBB, 0xCC, 0xDD };
    auto pkt = RtpPacket::create(CodecH264, 96, 0x11111111, 100, 5000, true,
                                  payload, sizeof(payload));
    ASSERT_TRUE(pkt != nullptr);

    // Encode as RTX
    pkt->RtxEncode(97, 0x22222222, 200);
    EXPECT_EQ(pkt->getPayloadType(), 97);
    EXPECT_EQ(pkt->getSSRC(), 0x22222222u);
    EXPECT_EQ(pkt->getSeq(), 200);

    // RTX prepends original seq (2 bytes) before the original payload
    EXPECT_EQ(pkt->getPayloadSize(), sizeof(payload) + 2);

    // Decode back
    bool ok = pkt->RtxDecode(96, 0x11111111);
    ASSERT_TRUE(ok);
    EXPECT_EQ(pkt->getPayloadType(), 96);
    EXPECT_EQ(pkt->getSSRC(), 0x11111111u);
    EXPECT_EQ(pkt->getSeq(), 100);
    EXPECT_EQ(pkt->getPayloadSize(), sizeof(payload));
    EXPECT_EQ(memcmp(pkt->getPayload(), payload, sizeof(payload)), 0);
}

// ────────────────────────────────────────────────────────────
// RtcpPacket Tests
// ────────────────────────────────────────────────────────────

TEST(RtcpPacket, TypeStrings) {
    EXPECT_STREQ(rtcpTypeString(RtcpType::SR), "SR");
    EXPECT_STREQ(rtcpTypeString(RtcpType::RR), "RR");
    EXPECT_STREQ(rtcpTypeString(RtcpType::SDES), "SDES");
    EXPECT_STREQ(rtcpTypeString(RtcpType::BYE), "BYE");
    EXPECT_STREQ(rtcpTypeString(RtcpType::RTPFB), "RTPFB");
    EXPECT_STREQ(rtcpTypeString(RtcpType::PSFB), "PSFB");
}

TEST(RtcpPacket, CreateSR) {
    auto buf = RtcpPacket::createSR(0xAABBCCDD, 0x11111111, 0x22222222,
                                     90000, 100, 50000);
    ASSERT_TRUE(buf != nullptr);
    auto *sr = reinterpret_cast<const RtcpSR *>(buf->data());
    EXPECT_TRUE(sr->header.isValid());
    EXPECT_EQ(sr->header.getType(), RtcpType::SR);
    EXPECT_EQ(sr->getSSRC(), 0xAABBCCDDu);
    EXPECT_EQ(sr->getNtpSec(), 0x11111111u);
    EXPECT_EQ(sr->getNtpFrac(), 0x22222222u);
    EXPECT_EQ(sr->getRtpTs(), 90000u);
    EXPECT_EQ(sr->getPacketCount(), 100u);
    EXPECT_EQ(sr->getOctetCount(), 50000u);
}

TEST(RtcpPacket, CreateRR) {
    RtcpReportBlock rb;
    rb.setSSRC(0x12345678);
    rb.fractionLost = 50;
    rb.setCumLost(10);
    rb.setHighestSeq(1000);
    rb.setJitter(200);

    auto buf = RtcpPacket::createRR(0xAABBCCDD, { rb });
    ASSERT_TRUE(buf != nullptr);
    auto *rr = reinterpret_cast<const RtcpRR *>(buf->data());
    EXPECT_TRUE(rr->header.isValid());
    EXPECT_EQ(rr->header.getType(), RtcpType::RR);
    EXPECT_EQ(rr->getSSRC(), 0xAABBCCDDu);
    EXPECT_EQ(rr->getReportCount(), 1);

    auto *block = rr->getFirstReportBlock();
    EXPECT_EQ(block->getSSRC(), 0x12345678u);
    EXPECT_EQ(block->fractionLost, 50);
    EXPECT_EQ(block->getCumLost(), 10u);
    EXPECT_EQ(block->getHighestSeq(), 1000u);
    EXPECT_EQ(block->getJitter(), 200u);
}

TEST(RtcpPacket, CreateSDES) {
    auto buf = RtcpPacket::createSDES(0x12345678, "test@cname.example");
    ASSERT_TRUE(buf != nullptr);
    auto *hdr = reinterpret_cast<const RtcpHeader *>(buf->data());
    EXPECT_TRUE(hdr->isValid());
    EXPECT_EQ(hdr->getType(), RtcpType::SDES);
}

TEST(RtcpPacket, CreateBYE) {
    auto buf = RtcpPacket::createBYE({ 0x11111111, 0x22222222 }, "leaving");
    ASSERT_TRUE(buf != nullptr);
    auto *hdr = reinterpret_cast<const RtcpHeader *>(buf->data());
    EXPECT_TRUE(hdr->isValid());
    EXPECT_EQ(hdr->getType(), RtcpType::BYE);
    EXPECT_EQ(hdr->getRC(), 2);  // two SSRCs

    auto *bye = reinterpret_cast<const RtcpBYE *>(buf->data());
    EXPECT_EQ(bye->getSSRC(), 0x11111111u);
    std::string reason = bye->getReason();
    EXPECT_EQ(reason, "leaving");
}

TEST(RtcpPacket, CreateNACK) {
    // Request retransmission of seqs: 100, 101, 102, 116
    std::vector<uint16_t> lostSeqs = { 100, 101, 102, 116 };
    auto buf = RtcpPacket::createNACK(0xAAAAAAAA, 0xBBBBBBBB, lostSeqs);
    ASSERT_TRUE(buf != nullptr);
    auto *hdr = reinterpret_cast<const RtcpHeader *>(buf->data());
    EXPECT_TRUE(hdr->isValid());
    EXPECT_EQ(hdr->getType(), RtcpType::RTPFB);
    EXPECT_EQ(hdr->getFMT(), static_cast<uint8_t>(RtcpFbFmt::NACK));

    auto *fb = reinterpret_cast<const RtcpFbHeader *>(buf->data());
    EXPECT_EQ(fb->getSenderSsrc(), 0xAAAAAAAAu);
    EXPECT_EQ(fb->getMediaSsrc(), 0xBBBBBBBBu);
}

TEST(RtcpPacket, CreatePLI) {
    auto buf = RtcpPacket::createPLI(0xAAAAAAAA, 0xBBBBBBBB);
    ASSERT_TRUE(buf != nullptr);
    auto *hdr = reinterpret_cast<const RtcpHeader *>(buf->data());
    EXPECT_TRUE(hdr->isValid());
    EXPECT_EQ(hdr->getType(), RtcpType::PSFB);
    EXPECT_EQ(hdr->getFMT(), static_cast<uint8_t>(RtcpFbFmt::PLI));
    // PLI = RtcpFbHeader (12 bytes): header(4) + senderSsrc(4) + mediaSsrc(4)
    EXPECT_EQ(hdr->getSize(), 12u);
}

TEST(RtcpPacket, CreateFIR) {
    auto buf = RtcpPacket::createFIR(0xAAAAAAAA, 0xBBBBBBBB, 42);
    ASSERT_TRUE(buf != nullptr);
    auto *hdr = reinterpret_cast<const RtcpHeader *>(buf->data());
    EXPECT_TRUE(hdr->isValid());
    EXPECT_EQ(hdr->getType(), RtcpType::PSFB);
    EXPECT_EQ(hdr->getFMT(), static_cast<uint8_t>(RtcpFbFmt::FIR));
}

TEST(RtcpPacket, CreateREMB) {
    auto buf = RtcpPacket::createREMB(0xAAAAAAAA, 2000000, { 0xBBBBBBBB });
    ASSERT_TRUE(buf != nullptr);
    auto *hdr = reinterpret_cast<const RtcpHeader *>(buf->data());
    EXPECT_TRUE(hdr->isValid());
    EXPECT_EQ(hdr->getType(), RtcpType::PSFB);
    EXPECT_EQ(hdr->getFMT(), static_cast<uint8_t>(RtcpFbFmt::REMB));
}

TEST(RtcpNackItem, GetLostSeqs) {
    RtcpNackItem item;
    item.setPid(100);
    // BLP: bit 0 = seq 101, bit 1 = seq 102, bit 5 = seq 106
    item.setBlp((1 << 0) | (1 << 1) | (1 << 5));

    auto seqs = item.getLostSeqs();
    ASSERT_EQ(seqs.size(), 4u);
    EXPECT_EQ(seqs[0], 100);
    EXPECT_EQ(seqs[1], 101);
    EXPECT_EQ(seqs[2], 102);
    EXPECT_EQ(seqs[3], 106);
}

TEST(RtcpPacket, ParseCompound) {
    // Build a compound packet: SR + RR
    auto srBuf = RtcpPacket::createSR(0x11111111, 0, 0, 90000, 10, 1000);
    auto rrBuf = RtcpPacket::createRR(0x22222222, {});

    std::vector<uint8_t> compound;
    auto *srData = static_cast<const uint8_t *>(srBuf->data());
    compound.insert(compound.end(), srData, srData + srBuf->size());
    auto *rrData = static_cast<const uint8_t *>(rrBuf->data());
    compound.insert(compound.end(), rrData, rrData + rrBuf->size());

    auto packets = RtcpPacket::parse(compound.data(), compound.size());
    ASSERT_GE(packets.size(), 2u);
    EXPECT_EQ(packets[0]->getType(), RtcpType::SR);
    EXPECT_EQ(packets[1]->getType(), RtcpType::RR);
}

// ────────────────────────────────────────────────────────────
// RtpExt Tests
// ────────────────────────────────────────────────────────────

TEST(RtpExt, TypeUrlMapping) {
    EXPECT_EQ(RtpExt::getExtType("urn:ietf:params:rtp-hdrext:ssrc-audio-level"),
              RtpExtType::ssrc_audio_level);
    EXPECT_EQ(RtpExt::getExtType("http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time"),
              RtpExtType::abs_send_time);
    EXPECT_EQ(RtpExt::getExtType("urn:ietf:params:rtp-hdrext:sdes:mid"),
              RtpExtType::sdes_mid);

    EXPECT_EQ(RtpExt::getExtUrl(RtpExtType::transport_cc),
              "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01");
}

TEST(RtpExt, NameString) {
    EXPECT_STREQ(RtpExt::getExtName(RtpExtType::abs_send_time), "abs_send_time");
    EXPECT_STREQ(RtpExt::getExtName(RtpExtType::transport_cc), "transport_cc");
    EXPECT_STREQ(RtpExt::getExtName(RtpExtType::sdes_mid), "sdes_mid");
}

// ────────────────────────────────────────────────────────────
// RtpMap Tests
// ────────────────────────────────────────────────────────────

TEST(RtpMap, H264Creation) {
    H264RtpMap map(96, 90000, PROFILE_H264_HIGH);
    EXPECT_EQ(map.getCodeName(), "H264");
    EXPECT_EQ(map.getPayload(), 96);
    EXPECT_EQ(map.getClockRate(), 90000u);
    EXPECT_EQ(map.getType(), TrackVideo);

    auto &fmtp = map.getFmtp();
    EXPECT_NE(fmtp.find("packetization-mode"), fmtp.end());
    EXPECT_EQ(fmtp.at("packetization-mode"), "1");
    EXPECT_NE(fmtp.find("profile-level-id"), fmtp.end());
}

TEST(RtpMap, H265Creation) {
    H265RtpMap map(98, 90000, PROFILE_H265_MAIN);
    EXPECT_EQ(map.getCodeName(), "H265");
    EXPECT_EQ(map.getPayload(), 98);
    EXPECT_EQ(map.getType(), TrackVideo);
    EXPECT_NE(map.getFmtp().find("profile-id"), map.getFmtp().end());
}

TEST(RtpMap, AudioRtpMap) {
    AudioRtpMap map("opus", 111, 48000);
    EXPECT_EQ(map.getCodeName(), "opus");
    EXPECT_EQ(map.getPayload(), 111);
    EXPECT_EQ(map.getClockRate(), 48000u);
    EXPECT_EQ(map.getType(), TrackAudio);
}

// ────────────────────────────────────────────────────────────
// Frame / Codec Tests
// ────────────────────────────────────────────────────────────

TEST(Frame, CodecIdAndName) {
    EXPECT_STREQ(getCodecName(CodecH264), "H264");
    EXPECT_STREQ(getCodecName(CodecH265), "H265");
    EXPECT_STREQ(getCodecName(CodecOpus), "opus");
    EXPECT_STREQ(getCodecName(CodecAAC), "mpeg4-generic");
    EXPECT_STREQ(getCodecName(CodecVP8), "VP8");
    EXPECT_STREQ(getCodecName(CodecVP9), "VP9");

    EXPECT_EQ(getCodecId("H264"), CodecH264);
    EXPECT_EQ(getCodecId("opus"), CodecOpus);
    EXPECT_EQ(getCodecId("VP8"), CodecVP8);
}

TEST(Frame, TrackType) {
    EXPECT_EQ(getTrackType(CodecH264), TrackVideo);
    EXPECT_EQ(getTrackType(CodecH265), TrackVideo);
    EXPECT_EQ(getTrackType(CodecOpus), TrackAudio);
    EXPECT_EQ(getTrackType(CodecAAC), TrackAudio);
    EXPECT_EQ(getTrackType(CodecG711A), TrackAudio);
    EXPECT_EQ(getTrackType(CodecVP9), TrackVideo);
}

TEST(Frame, RtpPayload) {
    // Well-known static payload types
    EXPECT_EQ(RtpPayload::getClockRate(PT_PCMU), 8000);
    EXPECT_EQ(RtpPayload::getClockRate(PT_PCMA), 8000);
    EXPECT_EQ(RtpPayload::getTrackType(PT_PCMU), TrackAudio);
    EXPECT_EQ(RtpPayload::getTrackType(PT_JPEG), TrackVideo);
    EXPECT_EQ(RtpPayload::getCodecId(PT_PCMA), CodecG711A);
    EXPECT_EQ(RtpPayload::getCodecId(PT_PCMU), CodecG711U);

    // Clock rate by codec
    EXPECT_GT(RtpPayload::getClockRateByCodec(CodecOpus), 0);
    EXPECT_GT(RtpPayload::getClockRateByCodec(CodecH264), 0);
}

// ────────────────────────────────────────────────────────────
// SDP (Sdp.h) Tests
// ────────────────────────────────────────────────────────────

TEST(SdpTest, DtlsRoleStrings) {
    EXPECT_STREQ(getDtlsRoleString(DtlsRole::active), "active");
    EXPECT_STREQ(getDtlsRoleString(DtlsRole::passive), "passive");
    EXPECT_STREQ(getDtlsRoleString(DtlsRole::actpass), "actpass");

    EXPECT_EQ(getDtlsRole("active"), DtlsRole::active);
    EXPECT_EQ(getDtlsRole("passive"), DtlsRole::passive);
    EXPECT_EQ(getDtlsRole("actpass"), DtlsRole::actpass);
}

TEST(SdpTest, RtpDirectionStrings) {
    EXPECT_STREQ(getRtpDirectionString(RtpDirection::sendonly), "sendonly");
    EXPECT_STREQ(getRtpDirectionString(RtpDirection::recvonly), "recvonly");
    EXPECT_STREQ(getRtpDirectionString(RtpDirection::sendrecv), "sendrecv");
    EXPECT_STREQ(getRtpDirectionString(RtpDirection::inactive), "inactive");

    EXPECT_EQ(getRtpDirection("sendonly"), RtpDirection::sendonly);
    EXPECT_EQ(getRtpDirection("recvonly"), RtpDirection::recvonly);
    EXPECT_EQ(getRtpDirection("sendrecv"), RtpDirection::sendrecv);
    EXPECT_EQ(getRtpDirection("inactive"), RtpDirection::inactive);
}

TEST(SdpTest, SdpOrigin) {
    SdpOrigin origin;
    origin.parse("- 12345 67890 IN IP4 127.0.0.1");
    EXPECT_EQ(origin.username, "-");
    EXPECT_EQ(origin.session_id, "12345");
    EXPECT_EQ(origin.session_version, "67890");
    EXPECT_EQ(origin.nettype, "IN");
    EXPECT_EQ(origin.addrtype, "IP4");
    EXPECT_EQ(origin.address, "127.0.0.1");

    std::string s = origin.toString();
    EXPECT_NE(s.find("12345"), std::string::npos);
    EXPECT_NE(s.find("67890"), std::string::npos);
}

TEST(SdpTest, SdpConnection) {
    SdpConnection conn;
    conn.parse("IN IP4 192.168.1.100");
    EXPECT_EQ(conn.nettype, "IN");
    EXPECT_EQ(conn.addrtype, "IP4");
    EXPECT_EQ(conn.address, "192.168.1.100");

    std::string s = conn.toString();
    EXPECT_NE(s.find("192.168.1.100"), std::string::npos);
}

TEST(SdpTest, SdpBandwidth) {
    SdpBandwidth bw;
    bw.parse("AS:512");
    EXPECT_EQ(bw.bwtype, "AS");
    EXPECT_EQ(bw.bandwidth, 512u);

    std::string s = bw.toString();
    EXPECT_NE(s.find("AS:512"), std::string::npos);
}

TEST(SdpTest, RtcSessionParseGenerate) {
    // A minimal WebRTC SDP offer
    std::string sdp =
        "v=0\r\n"
        "o=- 123456 2 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "a=group:BUNDLE 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=mid:0\r\n"
        "a=rtpmap:111 opus/48000/2\r\n"
        "a=sendrecv\r\n"
        "a=setup:actpass\r\n"
        "a=ice-ufrag:testufrag\r\n"
        "a=ice-pwd:testpassword123456789012\r\n"
        "a=fingerprint:sha-256 AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99\r\n";

    RtcSession session;
    session.loadFrom(sdp);

    ASSERT_FALSE(session.media.empty());
    auto &media = session.media[0];
    EXPECT_EQ(media.type, TrackAudio);
    EXPECT_EQ(media.mid, "0");
    EXPECT_EQ(media.ice_ufrag, "testufrag");
    EXPECT_EQ(media.ice_pwd, "testpassword123456789012");
    EXPECT_EQ(media.direction, RtpDirection::sendrecv);
    EXPECT_EQ(media.role, DtlsRole::actpass);

    // Should have opus codec plan
    auto *plan = media.getPlan("opus");
    ASSERT_TRUE(plan != nullptr);
    EXPECT_EQ(plan->sample_rate, 48000);

    // Round-trip: toString then loadFrom
    std::string generated = session.toString();
    EXPECT_FALSE(generated.empty());

    RtcSession session2;
    session2.loadFrom(generated);
    ASSERT_FALSE(session2.media.empty());
    EXPECT_EQ(session2.media[0].mid, "0");
}

TEST(SdpTest, RtcSessionVideoOffer) {
    std::string sdp =
        "v=0\r\n"
        "o=- 789 1 IN IP4 0.0.0.0\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96 97\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=mid:v0\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"
        "a=rtpmap:97 rtx/90000\r\n"
        "a=fmtp:97 apt=96\r\n"
        "a=sendonly\r\n"
        "a=setup:active\r\n"
        "a=ice-ufrag:videoufrag\r\n"
        "a=ice-pwd:videopassword12345678901\r\n"
        "a=fingerprint:sha-256 11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00\r\n";

    RtcSession session;
    session.loadFrom(sdp);

    ASSERT_FALSE(session.media.empty());
    auto &media = session.media[0];
    EXPECT_EQ(media.type, TrackVideo);
    EXPECT_EQ(media.mid, "v0");
    EXPECT_EQ(media.direction, RtpDirection::sendonly);
    EXPECT_EQ(media.role, DtlsRole::active);

    auto *h264Plan = media.getPlan("H264");
    ASSERT_TRUE(h264Plan != nullptr);
    EXPECT_EQ(h264Plan->sample_rate, 90000);

    auto *rtxPlan = media.getPlan("rtx");
    ASSERT_TRUE(rtxPlan != nullptr);
}

// ────────────────────────────────────────────────────────────
// mDNS Tests (RFC 6762)
// ────────────────────────────────────────────────────────────

TEST(MdnsTest, NameHelpers) {
    std::string name = mdnsGenerateName();
    // 36 char UUID v4 + ".local"
    EXPECT_EQ(name.size(), 36u + strlen(MDNS_SUFFIX));
    EXPECT_EQ(name[14], '4');
    EXPECT_TRUE(isMdnsName(name));
    EXPECT_EQ(mdnsNormalizeName(name + "."), name);

    EXPECT_TRUE(isMdnsName("Foo.Local"));
    EXPECT_FALSE(isMdnsName(".local"));
    EXPECT_FALSE(isMdnsName("192.168.1.10"));
    EXPECT_FALSE(isMdnsName("example.com"));

    EXPECT_EQ(mdnsNormalizeName("ABCD.Local."), "abcd.local");
}

TEST(MdnsTest, EncodeDecodeQuery) {
    MdnsMessage msg;
    MdnsQuestion question;
    question.name = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee.local";
    question.type = MDNS_TYPE_A;
    question.unicastResponse = true;
    msg.questions.push_back(question);

    auto encoded = msg.encode();
    MdnsMessage decoded;
    ASSERT_TRUE(MdnsMessage::decode(encoded.data(), encoded.size(), &decoded));
    EXPECT_FALSE(decoded.response);
    ASSERT_EQ(decoded.questions.size(), 1u);
    EXPECT_EQ(decoded.questions[0].name, question.name);
    EXPECT_EQ(decoded.questions[0].type, (uint16_t)MDNS_TYPE_A);
    EXPECT_TRUE(decoded.questions[0].unicastResponse);
    EXPECT_TRUE(decoded.answers.empty());
}

TEST(MdnsTest, EncodeDecodeAddressRecords) {
    sockaddr_u v4, v6;
    memset(&v4, 0, sizeof(v4));
    memset(&v6, 0, sizeof(v6));
    sockaddr_set_ipport(&v4, "192.168.1.10", 0);
    sockaddr_set_ipport(&v6, "fe80::1", 0);

    MdnsMessage msg;
    msg.response = true;
    msg.authoritative = true;
    const std::string name = mdnsGenerateName();
    for (const auto& addr : {v4, v6}) {
        MdnsRecord record;
        record.name = name;
        record.type = addr.sa.sa_family == AF_INET6 ? MDNS_TYPE_AAAA : MDNS_TYPE_A;
        record.addr = addr;
        record.setCacheFlush(true);
        msg.answers.push_back(record);
    }
    // a goodbye record must not be taken for an address
    MdnsRecord goodbye = msg.answers[0];
    goodbye.ttl = 0;
    msg.additionals.push_back(goodbye);

    auto encoded = msg.encode();
    MdnsMessage decoded;
    ASSERT_TRUE(MdnsMessage::decode(encoded.data(), encoded.size(), &decoded));
    EXPECT_TRUE(decoded.response);
    EXPECT_TRUE(decoded.authoritative);
    ASSERT_EQ(decoded.answers.size(), 2u);
    EXPECT_TRUE(decoded.answers[0].cacheFlush());

    sockaddr_u resolved;
    ASSERT_TRUE(decoded.findAddress(name, &resolved));
    EXPECT_EQ(mdnsAddrString(resolved), "192.168.1.10:0");
    EXPECT_FALSE(decoded.findAddress(mdnsGenerateName(), &resolved));
}

TEST(MdnsTest, DecodeNameCompressionPointer) {
    // A response whose answer name is a pointer to the question name (RFC 1035 4.1.4)
    std::vector<uint8_t> encoded = {
        0x00, 0x00, 0x84, 0x00,
        0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        4, 'h', 'o', 's', 't', 5, 'l', 'o', 'c', 'a', 'l', 0,
        0x00, 0x01, 0x00, 0x01,
        0xC0, 0x0C,
        0x00, 0x01, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x78,
        0x00, 0x04, 127, 0, 0, 1,
    };

    MdnsMessage decoded;
    ASSERT_TRUE(MdnsMessage::decode(encoded.data(), encoded.size(), &decoded));
    ASSERT_EQ(decoded.questions.size(), 1u);
    EXPECT_EQ(decoded.questions[0].name, "host.local");
    ASSERT_EQ(decoded.answers.size(), 1u);
    EXPECT_EQ(decoded.answers[0].name, "host.local");

    sockaddr_u resolved;
    // The lookup is case insensitive and tolerates the trailing dot of the wire format
    ASSERT_TRUE(decoded.findAddress("HOST.LOCAL.", &resolved));
    EXPECT_EQ(mdnsAddrString(resolved), "127.0.0.1:0");
}

TEST(MdnsTest, AnnounceAndResolve) {
    hv::EventLoopThread thread;
    thread.start();
    ASSERT_TRUE(thread.isRunning());
    auto loop = thread.loop();

    auto responder = std::make_shared<MdnsService>(loop);
    auto resolver = std::make_shared<MdnsService>(loop);

    const std::string name = mdnsGenerateName();
    sockaddr_u addr;
    memset(&addr, 0, sizeof(addr));
    ASSERT_EQ(sockaddr_set_ipport(&addr, "192.0.2.23", 0), 0);  // TEST-NET-1

    std::promise<std::string> result;
    auto resolved = result.get_future();
    responder->publish(name, addr);
    resolver->resolve(name, [&result](const std::string&, const sockaddr_u* ip) {
        result.set_value(ip ? mdnsAddrString(*ip) : "");
    }, 3000);

    auto status = resolved.wait_for(std::chrono::seconds(5));
    // The pending tasks are drained in order, so the services are stopped here for sure
    std::promise<void> stopped;
    responder->stop();
    resolver->stop();
    loop->runInLoop([&stopped]() { stopped.set_value(); });
    stopped.get_future().wait();
    thread.stop();
    thread.join();

    if (status != std::future_status::ready) {
        GTEST_SKIP() << "no mDNS traffic was delivered in this environment";
    }
    EXPECT_EQ(resolved.get(), "192.0.2.23:0");
}

TEST(MdnsTest, OneQueryAnswersEveryResolver) {
    hv::EventLoopThread thread;
    thread.start();
    auto loop = thread.loop();
    auto responder = std::make_shared<MdnsService>(loop);
    auto resolver = std::make_shared<MdnsService>(loop);

    const std::string name = mdnsGenerateName();
    sockaddr_u addr;
    memset(&addr, 0, sizeof(addr));
    ASSERT_EQ(sockaddr_set_ipport(&addr, "192.0.2.99", 0), 0);

    std::atomic<int> hits{0};
    auto cb = [&hits](const std::string&, const sockaddr_u* ip) {
        if (ip && mdnsAddrString(*ip) == "192.0.2.99:0") ++hits;
    };
    responder->publish(name, addr);
    // Two requests for the same name put one query on the wire but have to be answered both
    resolver->resolve(name, cb, 3000);
    resolver->resolve(name, cb, 3000);

    for (int i = 0; i < 500 && hits.load() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::promise<void> stopped;
    responder->stop();
    resolver->stop();
    loop->runInLoop([&stopped]() { stopped.set_value(); });
    stopped.get_future().wait();
    thread.stop();
    thread.join();

    if (hits.load() == 0) {
        GTEST_SKIP() << "no mDNS traffic was delivered in this environment";
    }
    EXPECT_EQ(hits.load(), 2);
}

TEST(IceCandidateTest, MdnsSdpRoundTrip) {
    IceCandidate cand;
    cand.foundation = "1";
    cand.componentId = 1;
    cand.protocol = TransportProtocol::UDP;
    cand.priority = 2130706431;
    cand.type = CandidateType::Host;
    sockaddr_set_ipport(&cand.addr, "192.168.1.10", 8443);
    cand.mdnsName = mdnsGenerateName();

    std::string sdp = cand.toSdp();
    EXPECT_NE(sdp.find(cand.mdnsName), std::string::npos);
    EXPECT_EQ(sdp.find("192.168.1.10"), std::string::npos);
    EXPECT_TRUE(cand.hasAddress());  // the local candidate does know its address

    IceCandidate parsed;
    ASSERT_TRUE(parsed.fromSdp(sdp));
    EXPECT_TRUE(parsed.isMdns());
    EXPECT_FALSE(parsed.hasAddress());  // hidden until the name is resolved
    EXPECT_EQ(parsed.mdnsName, cand.mdnsName);
    EXPECT_EQ(parsed.protocol, cand.protocol);
    EXPECT_EQ(parsed.priority, cand.priority);
    EXPECT_EQ(parsed.type, cand.type);

    // Resolution keeps the port advertised in SDP, only the address is filled in
    sockaddr_u resolved;
    memset(&resolved, 0, sizeof(resolved));
    sockaddr_set_ipport(&resolved, "10.0.0.5", 0);
    parsed.applyResolvedAddress(resolved);
    EXPECT_TRUE(parsed.hasAddress());
    EXPECT_EQ(parsed.addrString(), "10.0.0.5:8443");

    // A plain candidate is unaffected
    IceCandidate plain;
    ASSERT_TRUE(plain.fromSdp("1 1 udp 2130706431 192.168.1.10 8443 typ host"));
    EXPECT_FALSE(plain.isMdns());
    EXPECT_TRUE(plain.hasAddress());
    EXPECT_EQ(plain.sdpAddress(), "192.168.1.10");
}

static std::string ipOf(const sockaddr_u& addr) {
    char ip[INET6_ADDRSTRLEN] = {0};
    sockaddr_ip((sockaddr_u*)&addr, ip, sizeof(ip));
    return ip;
}

TEST_F(IceAgentTest, MdnsHidesHostCandidates) {
    config_.enableMdns = true;
    IceAgent agent;
    agent.setConfig(config_);
    ASSERT_EQ(agent.start(), 0);

    auto session = agent.createSession();
    EXPECT_TRUE(session->mdnsEnabled());

    std::vector<IceCandidate> gathered;
    session->onLocalCandidate = [&gathered](const IceCandidate& cand) {
        gathered.push_back(cand);
    };
    session->gatherCandidates();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    ASSERT_FALSE(gathered.empty());
    for (auto cand : gathered) {
        ASSERT_EQ(cand.type, CandidateType::Host);
        // SDP hides the address, the candidate itself keeps it for the connectivity checks
        EXPECT_TRUE(isMdnsName(cand.mdnsName));
        EXPECT_TRUE(cand.hasAddress());
        EXPECT_EQ(cand.sdpAddress(), cand.mdnsName);
        EXPECT_EQ(cand.toSdp().find(ipOf(cand.addr)), std::string::npos);
    }

    agent.stop();
}

TEST_F(IceAgentTest, MdnsDisabledKeepsLiteralAddresses) {
    config_.enableMdns = false;
    IceAgent agent;
    agent.setConfig(config_);
    ASSERT_EQ(agent.start(), 0);

    auto session = agent.createSession();
    EXPECT_FALSE(session->mdnsEnabled());

    std::vector<IceCandidate> gathered;
    session->onLocalCandidate = [&gathered](const IceCandidate& cand) {
        gathered.push_back(cand);
    };
    session->gatherCandidates();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    ASSERT_FALSE(gathered.empty());
    for (auto cand : gathered) {
        EXPECT_FALSE(cand.isMdns());
        EXPECT_EQ(cand.sdpAddress(), ipOf(cand.addr));
        EXPECT_NE(cand.toSdp().find(ipOf(cand.addr)), std::string::npos);
    }

    agent.stop();
}

// Reads a candidate vector of a session, in the event loop thread that owns it
using CandidatesGetter = std::function<const std::vector<IceCandidate>& (IceSession&)>;

static std::vector<IceCandidate> snapshotCandidates(const IceSessionPtr& session, const CandidatesGetter& getter) {
    std::promise<std::vector<IceCandidate>> promise;
    auto future = promise.get_future();
    session->loop()->runInLoop([&promise, session, getter]() {
        promise.set_value(getter(*session));
    });
    return future.get();
}

TEST_F(IceAgentTest, MdnsResolvesHiddenRemoteCandidates) {
    // NOTE: like MdnsTest.AnnounceAndResolve this needs mDNS multicast to be delivered
    // on the local link, which is the case whenever the candidates could be hidden.
    config_.enableMdns = true;
    IceAgent agent;
    agent.setConfig(config_);
    ASSERT_EQ(agent.start(), 0);

    auto s1 = agent.createSession();
    auto s2 = agent.createSession();

    // Exchange the candidates as SDP lines the way a signaling channel would: session2
    // never learns the address of session1 from the SDP, only its "<uuid>.local" name.
    s1->onLocalCandidate = [s2](const IceCandidate& cand) {
        IceCandidate remote;
        if (remote.fromSdp(cand.toSdp())) {
            s2->addRemoteCandidate(remote);
        }
    };
    s2->onLocalCandidate = [s1](const IceCandidate& cand) {
        IceCandidate remote;
        if (remote.fromSdp(cand.toSdp())) {
            s1->addRemoteCandidate(remote);
        }
    };

    auto locals = [](IceSession& s) -> const std::vector<IceCandidate>& { return s.localCandidates(); };
    auto remotes = [](IceSession& s) -> const std::vector<IceCandidate>& { return s.remoteCandidates(); };

    s1->setRemoteCredentials(s2->localUfrag(), s2->localPwd());
    s2->setRemoteCredentials(s1->localUfrag(), s1->localPwd());
    s1->gatherCandidates();
    s2->gatherCandidates();

    std::vector<IceCandidate> local, remote;
    bool resolved = false;
    for (int i = 0; i < 400 && !resolved; ++i) {
        local = snapshotCandidates(s1, locals);
        remote = snapshotCandidates(s2, remotes);
        // Candidates whose name is not resolved yet never reach remoteCandidates()
        resolved = !local.empty() && remote.size() >= local.size();
        if (!resolved) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(resolved) << "session2 did not resolve the hidden candidates of session1";

    for (const auto& hidden : local) {
        ASSERT_TRUE(isMdnsName(hidden.mdnsName));
        bool matched = false;
        for (const auto& cand : remote) {
            if (mdnsNormalizeName(cand.mdnsName) != mdnsNormalizeName(hidden.mdnsName)) continue;
            matched = true;
            EXPECT_TRUE(cand.hasAddress());
            EXPECT_EQ(cand.addrString(), hidden.addrString());
            EXPECT_EQ(cand.protocol, hidden.protocol);
            EXPECT_EQ(cand.componentId, hidden.componentId);
        }
        EXPECT_TRUE(matched) << "no candidate was queued for " << hidden.mdnsName.c_str();
    }

    agent.stop();
}


