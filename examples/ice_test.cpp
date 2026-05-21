#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// ICE library test
#include "../ice/ice.h"
#include "hstring.h"
using namespace ice;

int main(int argc, char* argv[]) {
    logger_enable_color(hlog, true);
    hlog_set_handler(stdout_logger);
    bool testTcp = false;
    for (int i =0; i < argc; ++i) {
        if (strcmp(argv[i], "--tcp") == 0) {
            testTcp = true;
        }
    }
    IceConfig config;
    config.udpPort = 0; // ephemeral
    config.gatherTcp = true;
    // stun server
    config.gatherSrflx = true;
    config.stunServers.push_back({"120.26.218.183", 3478});

    // turn server
    config.gatherRelay = false;
    TurnServerConfig turn;
    turn.addr.host = "127.0.0.1";
    turn.addr.port = 3478;
    turn.username = "ling";
    turn.password = "ling1234";
    turn.protocol = TurnServerConfig::TCP;
    config.turnServers.push_back(turn);

    IceAgent agent;
    agent.setConfig(config);
    int ret = agent.start();
    printf("  Agent start: %s (ret=%d)\n", ret == 0 ? "OK" : "FAILED", ret);
    if (ret != 0) {
        return -1;
    }

    // Create two sessions sharing the same port
    auto session1 = agent.createSession(IceMode::Full);
    auto session2 = agent.createSession(IceMode::Full);
    session1->onStateChange = [](IceState state) {
        printf("  Session1 state changed: %s\n", iceStateString(state));
    };
    session2->onStateChange = [](IceState state) {
        printf("  Session2 state changed: %s\n", iceStateString(state));
    };
    session1->onData = [](const void* data, size_t len) {
        printf("  Session1 recv data: %s\n", (const char*)data);
    };
    session2->onData = [](const void* data, size_t len) {
        printf("  Session2 recv data: %s\n", (const char*)data);
    };
    session1->onLocalCandidate = [session2, testTcp](const IceCandidate& candidate) {
        printf("  Session1 local candidate: %s\n", candidate.toSdp().c_str());
        if (testTcp && candidate.protocol != TransportProtocol::TCP) 
            return;
        session2->addRemoteCandidate(candidate);
    };
    session2->onLocalCandidate = [session1, testTcp](const IceCandidate& candidate) {
        printf("  Session2 local candidate: %s\n", candidate.toSdp().c_str());
        if (testTcp && candidate.protocol != TransportProtocol::TCP) 
            return;
        session1->addRemoteCandidate(candidate);
    };
    session1->onSelectedPair = [](const CandidatePair& pair) {
        printf("  Session1 selected pair: %s\n", pair.toString().c_str());
    };
    session2->onSelectedPair = [](const CandidatePair& pair) {
        printf("  Session2 selected pair: %s\n", pair.toString().c_str());
    };
    session1->setRemoteCredentials(session2->localUfrag(), session2->localPwd());
    session2->setRemoteCredentials(session1->localUfrag(), session1->localPwd());
    session1->gatherCandidates();
    session2->gatherCandidates();
    printf("press q to quit loop\n");
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        if (!strcasecmp(line, "q\n") || !strcasecmp(line, "quit\n")) {
            printf("user break loop\n");
            break;
        }
        int len = strlen(line);
        if (hv::startswith(line, "1 ")) {
            session1->send(line+2, len -2);
        }
        if (hv::startswith(line, "2 ")) {
            session2->send(line + 2, len - 2);
        }
    }
    agent.destroySession(session1);
    agent.destroySession(session2);
    agent.stop();
    printf("  Agent stopped cleanly\n");
    return 0;
}
