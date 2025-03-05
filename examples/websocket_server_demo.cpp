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

const int mp3_chunk = 72;
std::vector<uint8_t> mp3_data;

class MyContext {
    hv::WebSocketChannel* sock_ = nullptr;

public:
    MyContext(hv::WebSocketChannel* s) : sock_(s) {
        timerID = INVALID_TIMER_ID;
    }
    ~MyContext() {
        closeFile();
        killTimer();
    }
    void killTimer() {
        if (timerID != INVALID_TIMER_ID) {
            ::killTimer(timerID);
            timerID = INVALID_TIMER_ID;
        }
    }
    void closeFile() {
        if (fp_) {
            fclose(fp_);
            fp_ = nullptr;
        }
    }
    void handleMessage(const std::string& msg, enum ws_opcode opcode) {
        if (opcode == WS_OPCODE_TEXT) {
            printf("onText: %.*s\n", (int)msg.size(), msg.data());
            if (msg == "start") {
                killTimer();
                // 开始发送mp3
                timerID = setInterval(72, [this](TimerID id) {
                    if (sock_->isConnected() && sock_->isWriteComplete()) {
                        int size = mp3_data.size() - mp3_pos;
                        if (size > mp3_chunk) {
                            size = mp3_chunk;
                        }
                        sock_->send((const char*)&mp3_data[mp3_pos], size, WS_OPCODE_BINARY, true);
                        mp3_pos += mp3_chunk;
                        if (mp3_pos >= mp3_data.size())
                            mp3_pos = 0;
                    }
                });
            }
            // 停止接收mp3
            else if (msg == "stop") {
                killTimer();
            }
            else if(-1!=msg.find("hello")) {
                sock_->send(msg, opcode);
            }
        }
        else{
            printf("onBin: %d\n", (int)msg.size());
            // 回显mp3
            sock_->send(msg, opcode);
            return ;
            if (msg.size() < 1) {
                closeFile();
                char buff[128];
                int n = snprintf(buff, sizeof(buff), "recv %d samples\n", binCount);
                sock_->send(buff, n, WS_OPCODE_TEXT);
                printf("%s", buff);
                binCount = 0;
                return;
            }
            // 追加文件
            if (!fp_) {
                char buff[256];
                sprintf(buff, "%d.mp3", count++);
                fp_ = fopen(buff, "wb");
            }
            fwrite(msg.data(), msg.size(), 1, fp_);
            binCount++;
        }
    }
    int mp3_pos = 0;
    int count = 0;
    FILE* fp_ = nullptr;
    int binCount = 0;
    TimerID timerID = 0;
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
        channel->setContextPtr(std::make_shared<MyContext>(channel.get()));
    };
    ws.onmessage = [](const WebSocketChannelPtr& channel, const std::string& msg) {
        auto ctx = channel->getContextPtr<MyContext>();
        ctx->handleMessage(msg, channel->opcode);
    };
    ws.onclose = [](const WebSocketChannelPtr& channel) {
        printf("onclose\n");
        channel->deleteContextPtr();
    };
    FILE* fp = fopen("resp.mp3", "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        int size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        mp3_data.resize(size);
        printf("read %d mp3 chunk=%d\n", size, mp3_chunk);
        fread(mp3_data.data(), size, 1, fp);
        fclose(fp);
        fp = nullptr;
    }
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
