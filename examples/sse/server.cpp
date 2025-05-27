
#include "HttpServer.h"
#include "EventLoop.h"
#include "SSEMgr.h"
#include "htime.h"
#include "hasync.h"     // import hv::async
//@see https://blog.csdn.net/CAir2/article/details/145759141
//@see https://cloud.tencent.com/developer/article/1999650

using namespace hv;

int main(int argc, char** argv) {
    HV_MEMCHECK;

    int port = 0;
    if (argc > 1) {
        port = atoi(argv[1]);
    }
    if (port == 0) port = 8080;

    HttpService router;

    // curl -v http://ip:port/get?env=1
    router.GET("/sse", [](const HttpContextPtr& ctx) {
        // SSEvent(message) every 1s
        hv::setInterval(10000, [ctx](hv::TimerID timerID) {
            static int ncount = 0;
            if (ctx->writer->isConnected()) {
                char szTime[DATETIME_FMT_BUFLEN] = {0};
                datetime_t now = datetime_now();
                datetime_fmt(&now, szTime);
                ctx->writer->SSEvent(szTime);
                //增加SSE链接管理，支持数据订阅推送
                static LONGLONG ids = 0;
                SSEMgr::instance().add(::InterlockedIncrement64(&ids), ctx);
                if (++ncount >= 10) {
                    //hv::killTimer(timerID);
                    ctx->writer->close();
                    ncount = 0;
                }
            } else {
                hv::killTimer(timerID);
            }
        });
        return HTTP_STATUS_UNFINISHED;
    });

    HttpServer server;
    server.service = &router;
    server.port = port;
#if TEST_HTTPS
    server.https_port = 8443;
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

    // uncomment to test multi-processes
    // server.setProcessNum(4);
    // uncomment to test multi-threads
    // server.setThreadNum(4);

    server.start();

    // press Enter to stop
    while (getchar() != '\n');
    hv::async::cleanup();
    return 0;
}