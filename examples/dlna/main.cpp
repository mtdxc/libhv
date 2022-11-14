#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include "UpnpServer.h"
#include "UpnpRender.h"
#include <string>

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
  upnp->addListener(&listener);
  upnp->search(USAVTransport);
  std::string cur_dev;
  std::string line;
  while (getline(std::cin, line)) {
    auto cmds = hv::split(line, ' ');
    if (cmds.empty())
      continue;
    std::string cmd = cmds[0];
    if (cmd == "quit" || cmd == "q")
      break;
    else if (cmd == "search") {
      int type = USAVTransport;
      bool cache = true;
      if(cmds.size() > 1)
        type = std::stoi(cmds[1]);
      if(cmds.size() > 2)
        type = std::stoi(cmds[2]);
      upnp->search(type, cache);
    }
    else if (cmd == "list" || cmd == "l") {
      auto devs = upnp->getDevices();
      PrintDevices(devs);
    }
    else if (cmd == "play" || cmd == "p") {
      if (cmds.size() < 3) continue;
      auto devs = upnp->getDevices();
      int pos = atoi(cmds[1].c_str());
      if (pos < 0 || pos >= devs.size()) {
        printf("pos %d error\n", pos);
        continue;
      }
      auto it = devs.begin();
      while (pos) {
        it++;
        pos--;
      }
      if (cur_dev.length())
        upnp->close(cur_dev.c_str());
      cur_dev = it->first;
      auto file = cmds[2];
      if (-1 == file.find("://")) {
        file = upnp->setCurFile(file.c_str());
      }
      upnp->openUrl(cur_dev.c_str(), file.c_str());
    }
    else if (cmd == "seek") {
      if (cur_dev.empty()) continue;
      upnp->seek(cur_dev.c_str(), std::stof(cmds[1]));
    }
    else if (cmd == "pause") {
      if (cur_dev.empty()) continue;
      upnp->pause(cur_dev.c_str());
    }
    else if (cmd == "resume") {
      if (cur_dev.empty()) continue;
      upnp->resume(cur_dev.c_str());
    }
    else if (cmd == "stop") {
      if (cur_dev.empty()) continue;
      upnp->close(cur_dev.c_str());
    }
    else if (cmd == "pos") {
      if (cur_dev.empty()) continue;
      upnp->getPosition(cur_dev.c_str(), [](int code, AVPositionInfo pos) {
        printf("goPos %d %f/%f\n", code, pos.relTime, pos.trackDuration);
      });
    }
    else if (cmd == "tinfo") {
      if (cur_dev.empty()) continue;
      upnp->getTransportInfo(cur_dev.c_str(), [](int code, TransportInfo info) {
        printf("goInfo %d %s %s %f\n", code, info.state, info.status, info.speed);
      });
    }
    else if (cmd == "minfo") {
      if (cur_dev.empty()) continue;
      upnp->getMediaInfo(cur_dev.c_str());
    }
    else if (cmd == "next") {
      if (cur_dev.empty()) continue;
      upnp->next(cur_dev.c_str());
    }
    else if (cmd == "prev") {
      if (cur_dev.empty()) continue;
      upnp->previous(cur_dev.c_str());
    }
    else if (cmd == "subscribe" || cmd == "subs") {
      if (cur_dev.empty()) continue;
      int type = USAVTransport;
      int timeout = 3600;
      if (cmds.size() > 1) {
        type = std::stoi(cmds[1]);
        if (cmd.size() > 2)
          timeout = std::stoi(cmds[2]);
      }
      upnp->subscribe(cur_dev.c_str(), type, timeout);
    }
    else if (cmd == "unsubscribe" || cmd == "unsubs") {
      if (cur_dev.empty()) continue;
      int type = USAVTransport;
      if (cmds.size() > 1) {
        type = std::stoi(cmds[1]);
      }
      upnp->unsubscribe(cur_dev.c_str(), type);
    }
    else if (cmd == "vol") {
      if (cur_dev.empty()) continue;
      upnp->getVolume(cur_dev.c_str(), [](int code, int vol) {
        printf("gotVolume %d %d\n", code, vol);
      });
    }
    else if (cmd == "setVol") {
      if (cur_dev.empty()) continue;
      int val = atoi(cmds[1].c_str());
      upnp->setVolume(cur_dev.c_str(), val);
    }
  }
  upnp->delListener(&listener);
  upnp->stop();
  return 0;
}