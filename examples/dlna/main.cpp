#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hlog.h"
#include "UpnpServer.h"
#include "UpnpRender.h"
class MyListener : public UpnpListener {
  std::shared_ptr<UpnpRender> render;
public:
  virtual void upnpSearchChangeWithResults(const MapDevices& devs) override
  {
    if (!render && devs.size()) {
      auto it = devs.begin();
      // 限制只投某个设备
      while (it!=devs.end()) {
        if (-1 != it->second->friendlyName.find("3D"))
          break;
        it++;
      }
      if (it==devs.end()) return;
      render = std::make_shared<UpnpRender>(it->second);
      render->setAVTransportURL("https://janus.97kid.com/264.flv", [this](int code, std::string err) {
        printf("setAVTransportURL return %d:%s\n", code, err.c_str());
        if (code) return;
        render->getVolume([](int code, int val) {
          printf("%d volume=%d\n", code, val);
        });
      });
    }
  }

};

int main(int argc, char* argv[]) {
  hlog_set_handler(stdout_logger);

  MyListener listener;
  Upnp* upnp = Upnp::Instance();
  upnp->start();
  upnp->setListener(&listener);
  upnp->search();

  printf("press any key to quit\n");
  getchar();

  upnp->setListener(nullptr);
  upnp->stop();
  return 0;
}