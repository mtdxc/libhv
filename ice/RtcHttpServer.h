
#ifndef RTC_HTTP_SERVER_H_
#define RTC_HTTP_SERVER_H_

#include "agent/ice_agent.h"
#include "http/server/WebSocketServer.h"
class RtcHttpServer
{
    std::shared_ptr<ice::IceAgent> agent_;
    std::unique_ptr<hv::WebSocketServer> http_;
public:
    RtcHttpServer();
    ~RtcHttpServer();

    void start(int port);
    void stop();
};

#endif // RTC_HTTP_SERVER_H_