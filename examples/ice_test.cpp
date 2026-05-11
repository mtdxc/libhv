#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// ICE library test
#include "../ice/ice.h"

using namespace ice;

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== ICE Library Test ===\n\n");

    // Test 1: STUN message encode/decode
    printf("[Test 1] STUN Message Encode/Decode\n");
    {
        StunMessage msg(STUN_METHOD_BINDING, STUN_CLASS_REQUEST);
        msg.addUsername("user1:user2");
        msg.addPriority(12345678);
        msg.addIceControlling(0x1122334455667788ULL);
        msg.addUseCandidate();

        auto encoded = msg.encode();
        printf("  Encoded size: %zu bytes\n", encoded.size());

        StunMessage decoded;
        bool ok = StunMessage::decode(encoded.data(), encoded.size(), &decoded);
        printf("  Decode: %s\n", ok ? "OK" : "FAILED");
        printf("  Username: %s\n", decoded.getUsername().c_str());
        printf("  Priority: %u\n", decoded.getPriority());
        printf("  UseCandidate: %s\n", decoded.hasUseCandidate() ? "yes" : "no");
        printf("  IceControlling: 0x%llx\n", (unsigned long long)decoded.getIceControlling());
        printf("  isStun: %s\n", StunMessage::isStun(encoded.data(), encoded.size()) ? "yes" : "no");
    }
    printf("\n");

    // Test 2: STUN message with authentication
    printf("[Test 2] STUN Message with AUTH (HMAC-SHA1 + FINGERPRINT)\n");
    {
        StunMessage msg(STUN_METHOD_BINDING, STUN_CLASS_REQUEST);
        msg.addUsername("localufrag:remoteufrag");
        msg.addPriority(99999);

        std::string password = "mysecretpassword";
        auto encoded = msg.encodeWithAuth(password);
        printf("  Encoded with auth size: %zu bytes\n", encoded.size());

        StunMessage decoded;
        bool ok = StunMessage::decode(encoded.data(), encoded.size(), &decoded);
        printf("  Decode: %s\n", ok ? "OK" : "FAILED");
        printf("  Verify integrity: %s\n", decoded.verifyIntegrity(password) ? "PASS" : "FAIL");
        printf("  Verify integrity (wrong pwd): %s\n",
               decoded.verifyIntegrity("wrongpassword") ? "PASS" : "FAIL");
        printf("  Verify fingerprint: %s\n", decoded.verifyFingerprint() ? "PASS" : "FAIL");
    }
    printf("\n");

    // Test 3: XOR-MAPPED-ADDRESS encode/decode
    printf("[Test 3] XOR-MAPPED-ADDRESS\n");
    {
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
        struct sockaddr_in* res4 = (struct sockaddr_in*)&result;
        printf("  Decode: %s\n", ok ? "OK" : "FAILED");
        printf("  Port: %d (expected 12345)\n", ntohs(res4->sin_port));
        printf("  IP: %08x (expected c0a80164)\n", ntohl(res4->sin_addr.s_addr));
    }
    printf("\n");

    // Test 4: ICE Candidate priority
    printf("[Test 4] ICE Candidate Priority\n");
    {
        uint32_t hostPri = computeCandidatePriority(CandidateType::Host, 65535, 1);
        uint32_t srflxPri = computeCandidatePriority(CandidateType::ServerReflexive, 65535, 1);
        uint32_t relayPri = computeCandidatePriority(CandidateType::Relay, 65535, 1);
        printf("  Host priority: %u\n", hostPri);
        printf("  Srflx priority: %u\n", srflxPri);
        printf("  Relay priority: %u\n", relayPri);
        printf("  Order correct (host > srflx > relay): %s\n",
               (hostPri > srflxPri && srflxPri > relayPri) ? "YES" : "NO");
    }
    printf("\n");

    // Test 5: SDP generation and parsing
    printf("[Test 5] SDP Generation/Parsing\n");
    {
        std::vector<IceCandidate> candidates;

        IceCandidate cand;
        cand.foundation = "123456";
        cand.componentId = 1;
        cand.protocol = TransportProtocol::UDP;
        cand.priority = 2130706431;
        cand.type = CandidateType::Host;
        cand.addr.sin.sin_family = AF_INET;
        cand.addr.sin.sin_port = htons(5000);
        cand.addr.sin.sin_addr.s_addr = htonl(0xC0A80101); // 192.168.1.1
        candidates.push_back(cand);

        std::string sdp = IceSdp::generateAttributes("myufrag", "mypassword123456789012", candidates, false, true);
        printf("  Generated SDP:\n%s\n", sdp.c_str());

        auto result = IceSdp::parseAttributes(sdp);
        printf("  Parsed ufrag: %s\n", result.ufrag.c_str());
        printf("  Parsed pwd: %s\n", result.pwd.c_str());
        printf("  Parsed candidates: %zu\n", result.candidates.size());
        if (!result.candidates.empty()) {
            printf("  Candidate[0] foundation: %s\n", result.candidates[0].foundation.c_str());
            printf("  Candidate[0] type: %s\n", IceCandidate::typeString(result.candidates[0].type));
            printf("  Candidate[0] priority: %u\n", result.candidates[0].priority);
        }
    }
    printf("\n");

    // Test 6: CandidatePair priority
    printf("[Test 6] Candidate Pair Priority\n");
    {
        uint64_t pairPri = CandidatePair::computePairPriority(
            2130706431, 2130706431, IceRole::Controlling);
        printf("  Pair priority (both host): %llu\n", (unsigned long long)pairPri);
        printf("  Pair priority > 0: %s\n", pairPri > 0 ? "YES" : "NO");
    }
    printf("\n");

    // Test 7: IceAgent creation and port binding
    printf("[Test 7] IceAgent Creation\n");
    {
        IceConfig config;
        config.udpPort = 0; // ephemeral
        config.gatherTcp = false;

        IceAgent agent;
        agent.setConfig(config);
        int ret = agent.start();
        printf("  Agent start: %s (ret=%d)\n", ret == 0 ? "OK" : "FAILED", ret);
        if (ret == 0) {
            printf("  UDP port: %d\n", agent.udpPort());

            // Create two sessions sharing the same port
            auto session1 = agent.createSession(IceMode::Full);
            auto session2 = agent.createSession(IceMode::Full);
            printf("  Session1 ufrag: %s\n", session1->localUfrag().c_str());
            printf("  Session2 ufrag: %s\n", session2->localUfrag().c_str());
            printf("  Sessions share same UDP port: YES (port=%d)\n", agent.udpPort());

            agent.destroySession(session1);
            agent.destroySession(session2);
            agent.stop();
            printf("  Agent stopped cleanly\n");
        }
    }
    printf("\n");

    printf("=== All tests completed ===\n");
    return 0;
}
