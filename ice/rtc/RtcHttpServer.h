
#ifndef RTC_HTTP_SERVER_H_
#define RTC_HTTP_SERVER_H_

#include <memory>
#include <string>
#include "Frame.h"
#include "agent/ice_agent.h"
#include "http/server/WebSocketServer.h"

struct RtcHttpConfig {
  ice::IceConfig ice;
  int16_t http_port;
  int16_t https_port;
  std::string cert_file;
  std::string key_file;
};

class RtcHttpServer
{
    std::shared_ptr<ice::IceAgent> agent_;
    std::unique_ptr<hv::WebSocketServer> http_;
    RtcHttpConfig config_;
public:
    RtcHttpServer(const RtcHttpConfig& config);
    ~RtcHttpServer();

    static FrameDispatcher::Ptr getDispatcher(const std::string &stream);
    static void setDispatcher(std::string name, FrameDispatcher::Ptr dispatcher);

    void start();
    void stop();
};

#endif // RTC_HTTP_SERVER_H_