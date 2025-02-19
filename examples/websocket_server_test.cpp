/*
 * websocket server
 *
 * @build   make examples
 * @server  bin/websocket_server_test 9999
 * @client  bin/websocket_client_test ws://127.0.0.1:9999/
 * @python  scripts/websocket_server.py
 * @js      html/websocket_client.html
 *
 */

#include "WebSocketServer.h"
#include "EventLoop.h"
#include "htime.h"
#include "hlog.h"
using namespace hv;

/*
 * #define TEST_WSS 1
 *
 * @build   ./configure --with-openssl && make clean && make
 *
 * @server  bin/websocket_server_test 9999
 *
 * @client  bin/websocket_client_test ws://127.0.0.1:9999/
 *          bin/websocket_client_test wss://127.0.0.1:10000/
 *
 */
#define TEST_WSS 0

using namespace hv;
#include "protoo.h"
void testProtoo(){
    WebSocketService ws;
    ws.onopen = [](const WebSocketChannelPtr& channel, const HttpRequestPtr& req) {
        printf("onopen: GET %s\n", req->url.c_str());
        Protoo* ctx = channel->newContext<Protoo>();
        ctx->sendMsg = [channel](const std::string& msg){
            channel->send(msg);
        };
    };
    ws.onmessage = [](const WebSocketChannelPtr& channel, const std::string& msg) {
        Protoo* ctx = channel->getContext<Protoo>();
        ctx->recvMsg(msg);
    };
    ws.onclose = [](const WebSocketChannelPtr& channel) {
        printf("onclose\n");
        Protoo* ctx = channel->getContext<Protoo>();
        ctx->clearRequests();
        channel->deleteContext<Protoo>();
    };
}

class MyContext {
public:
    MyContext() {
        printf("MyContext::MyContext()\n");
        timerID = INVALID_TIMER_ID;
    }
    ~MyContext() {
        printf("MyContext::~MyContext()\n");
    }

    int handleMessage(const std::string& msg, enum ws_opcode opcode) {
        if (opcode == WS_OPCODE_TEXT) {
            printf("onText: %.*s\n", (int)msg.size(), msg.data());
        }
        else{
            printf("onBin: %d\n", (int)msg.size());
            binCount++;
        }
        return msg.size();
    }
    int binCount = 0;
    TimerID timerID;
};

int main(int argc, char** argv) {
    int port = 8000;
    if (argc < 2) {
        printf("Usage: %s port\n", argv[0]);
        // return -10;
    }
    else {
        port = atoi(argv[1]);
    }
    hlog_set_handler(stdout_logger);

    HttpService http;
    http.GET("/ping", [](const HttpContextPtr& ctx) {
        return ctx->send("pong");
    });
    http.document_root = "html";
    http.index_of = "/";

    WebSocketService ws;
    // ws.setPingInterval(10000);
    ws.onopen = [](const WebSocketChannelPtr& channel, const HttpRequestPtr& req) {
        printf("onopen: GET %s\n", req->Path().c_str());
        auto ctx = channel->newContextPtr<MyContext>();
        /* send(time) every 1s
        ctx->timerID = setInterval(1000, [channel](TimerID id) {
            if (channel->isConnected() && channel->isWriteComplete()) {
                char str[DATETIME_FMT_BUFLEN] = {0};
                datetime_t dt = datetime_now();
                datetime_fmt(&dt, str);
                channel->send(str);
            }
        });
        */
    };
    ws.onmessage = [](const WebSocketChannelPtr& channel, const std::string& msg) {
        auto ctx = channel->getContextPtr<MyContext>();
        ctx->handleMessage(msg, channel->opcode);
        if (channel->opcode == WS_OPCODE_BINARY) {
            channel->send(msg, channel->opcode);
            if (!msg.size()) {
                char buff[128];
                int n = snprintf(buff, sizeof(buff), "recv %d samples\n", ctx->binCount);
                channel->send(buff, n, WS_OPCODE_TEXT);
                printf("%s", buff);
                ctx->binCount = 0;
            }
        }
    };
    ws.onclose = [](const WebSocketChannelPtr& channel) {
        printf("onclose\n");
        auto ctx = channel->getContextPtr<MyContext>();
        if (ctx->timerID != INVALID_TIMER_ID) {
            killTimer(ctx->timerID);
            ctx->timerID = INVALID_TIMER_ID;
        }
        // channel->deleteContextPtr();
    };

    WebSocketServer server;
    server.port = port;
    server.worker_processes = 0;
    server.worker_threads = 1;
#if TEST_WSS
    server.https_port = port + 1;
    hssl_ctx_opt_t param;
    memset(&param, 0, sizeof(param));
    param.crt_file = "cert/server.crt";
    param.key_file = "cert/server.key";
    param.endpoint = HSSL_SERVER;
    if (server.newSslCtx(&param) != 0) {
        fprintf(stderr, "new SSL_CTX failed!\n");
        return -20;
    }
#endif
    server.registerHttpService(&http);
    server.registerWebSocketService(&ws);

    server.start();

    // press Enter to stop
    while (getchar() != '\n');
    return 0;
}
