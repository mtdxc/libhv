#include <thread>
#include <chrono>
#include <memory>
#include <cstring>
#include <gtest/gtest.h>
#include "ice/ice.h"

using namespace ice;

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

    auto session = agent.createSession(IceMode::Full);
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

    auto s1 = agent.createSession(IceMode::Full);
    auto s2 = agent.createSession(IceMode::Full);

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

    auto session = agent.createSession(IceMode::Full);

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
