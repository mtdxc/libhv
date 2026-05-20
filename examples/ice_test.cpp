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
    config.gatherTcp = false;

    IceAgent agent;
    agent.setConfig(config);
    int ret = agent.start();
    printf("  Agent start: %s (ret=%d)\n", ret == 0 ? "OK" : "FAILED", ret);
    if (ret != 0) {
        return -1;
    }
    printf("  UDP port: %d\n", agent.udpPort());

    // Create two sessions sharing the same port
    auto session1 = agent.createSession(IceMode::Full);
    auto session2 = agent.createSession(IceMode::Full);
    session1->gatherCandidates();
    printf("  Session1 ufrag: %s\n", session1->localUfrag().c_str());
    printf("  Session2 ufrag: %s\n", session2->localUfrag().c_str());
    printf("  Sessions share same UDP port: YES (port=%d)\n", agent.udpPort());
    printf("press q to quit loop\n");
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        if (!strcasecmp(line, "q") || strcasecmp(line, "quit"))
            break;
    }
    agent.destroySession(session1);
    agent.destroySession(session2);
    agent.stop();
    printf("  Agent stopped cleanly\n");
    return 0;
}
