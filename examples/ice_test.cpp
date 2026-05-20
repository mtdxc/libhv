#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// ICE library test
#include "../ice/ice.h"

using namespace ice;

int main() {
    hlog_set_handler(stdout_logger);
    
    IceConfig config;
    config.udpPort = 0; // ephemeral
    config.gatherTcp = true;
    // stun server
    config.gatherSrflx = true;
    config.stunServers.push_back({"120.26.218.183", 3478});

    // turn server
    config.gatherRelay = true;
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
    auto session = agent.createSession(IceMode::Full);
    session->gatherCandidates();
    printf("press q to quit loop\n");
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        if (!strcasecmp(line, "q") || strcasecmp(line, "quit"))
            break;
    }
    agent.destroySession(session);
    agent.stop();
    printf("  Agent stopped cleanly\n");
    return 0;
}
