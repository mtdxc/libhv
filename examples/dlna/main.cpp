#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include "hlog.h"
#include "UpnpServer.h"
#include "UpnpRender.h"

void PrintDevices(MapDevices devs)
{
  int pos = 0;
  for (auto dev : devs) {
    std::cout << "[" << pos << "] " << dev.second->friendlyName << std::endl;
    pos++;
  }
}

class MyListener : public UpnpListener {
  std::shared_ptr<UpnpRender> render;
public:
  virtual void upnpSearchChangeWithResults(const MapDevices& devs) override
  {
    PrintDevices(devs);
  }
};

int main(int argc, char* argv[]) {
  hlog_set_handler(stdout_logger);

  MyListener listener;
  Upnp* upnp = Upnp::Instance();
  upnp->start();
  upnp->setListener(&listener);
  upnp->search();

  std::unique_ptr<UpnpRender> render;
  std::string line;
  while (getline(std::cin, line)) {
    auto cmds = hv::split(line, ' ');
    if (cmds.empty())
      continue;
    std::string cmd = cmds[0];
    if (cmd == "quit" || cmd == "q")
      break;
    else if (cmd == "search") {
      upnp->search();
    }
    else if (cmd == "list") {
      auto devs = upnp->getDevices();
      PrintDevices(devs);
    }
    else if (cmd == "play" || cmd == "p") {
      if (cmds.size() < 3) continue;
      auto devs = upnp->getDevices();
      int pos = atoi(cmds[1].c_str());
      if (pos < 0 || pos > devs.size()) {
        continue;
      }
      auto it = devs.begin();
      while (pos) {
        it++;
        pos--;
      }
      if (render)
        render->stop();
      render.reset(new UpnpRender(it->second));
      auto file = cmds[2];
      if (hv::startswith(file, "http"))
        render->setAVTransportURL(cmds[2].c_str());
      else {
        upnp->setCurFile(file);
        render->setAVTransportURL(upnp->getCurUrl().c_str());
      }
    }
    else if (cmd == "seek") {
      if (!render) continue;
      render->seekToTarget(cmds[1].c_str(), "REL_TIME");
    }
    else if (cmd == "pause") {
      if (!render) continue;
      render->pause();
    }
    else if (cmd == "resume") {
      if (!render) continue;
      render->play();
    }
    else if (cmd == "stop") {
      if (!render) continue;
      render->stop();
    }
    else if (cmd == "pos") {
      if (!render) continue;
      render->getPositionInfo([](int code, AVPositionInfo pos) {
        printf("goPos %d %f/%f\n", code, pos.relTime, pos.trackDuration);
      });
    }
    else if (cmd == "vol") {
      if (!render) continue;
      render->getVolume([](int code, int vol) {
        printf("gotVolume %d %d\n", code, vol);
      });
    }
    else if (cmd == "setVol") {
      if (!render) continue;
      int val = atoi(cmds[1].c_str());
      render->setVolume(val);
    }
  }
  upnp->setListener(nullptr);
  upnp->stop();
  return 0;
}