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
  std::shared_ptr<UpnpRender> render;

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
    else if (cmd == "o" || cmd == "open" || cmd == "opensub") {
      if (cmds.size() < 2) continue;
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
      const char* usn = it->first.c_str();
      printf("use device %s\n", usn);
      render = Upnp::Instance()->getRender(usn);
      if (cmd == "opensub") {
        int type = USAVTransport;
        int timeout = 3600;
        if (cmds.size() > 2) {
          type = std::stoi(cmds[2]);
          if (cmd.size() > 3)
            timeout = std::stoi(cmds[3]);
        }
        render->subscribe(type, timeout);
      }
    }
    else if (cmd == "close") {
      if (render) {
        Upnp::Instance()->close(render->devId());
        render = nullptr;
      }
    }
    else if (cmd == "subscribe" || cmd == "sub") {
      if (!render) continue;
      int type = USAVTransport;
      int timeout = 3600;
      if (cmds.size() > 1) {
        type = std::stoi(cmds[1]);
        if (cmd.size() > 2)
          timeout = std::stoi(cmds[2]);
      }
      render->subscribe(type, timeout);
    }
    else if (cmd == "unsubscribe" || cmd == "unsub") {
      if (!render) continue;
      int type = USAVTransport;
      if (cmds.size() > 1) {
        type = std::stoi(cmds[1]);
      }
      render->unsubscribe(type);
    }
    else if (cmd == "cap") {
      if(render)
        render->getDeviceCapabilities(nullptr);
    }
    else if (cmd == "seturl" || cmd == "url") {
      if (cmds.size() < 2 || !render) continue;
      auto file = cmds[1];
      if (-1 == file.find("://")) {
        file = upnp->setCurFile(file.c_str());
      }
      render->setAVTransportURL(file.c_str());
    }
    else if (cmd == "seek") {
      if (render) render->seek(std::stof(cmds[1]));
    }
    else if (cmd == "pause") {
      if (render) render->pause();
    }
    else if (cmd == "resume") {
      if (render) render->play();
    }
    else if (cmd == "play") {
      float speed = 1.0f;
      if (cmds.size() > 1)
        speed = std::stof(cmds[1]);
      if (render) render->play(speed);
    }
    else if (cmd == "pos") {
      if (!render) continue;
      render->getPositionInfo([](int code, AVPositionInfo pos) {
        printf("goPos %d %f/%f\n", code, pos.relTime, pos.trackDuration);
      });
    }
    else if (cmd == "tinfo") {
      if (!render) continue;
      render->getTransportInfo([](int code, TransportInfo info) {
        printf("goInfo %d %s %s %f\n", code, info.state, info.status, info.speed);
      });
    }
    else if (cmd == "minfo") {
      if (!render) continue;
      render->getMediaInfo(nullptr);
    }
    else if (cmd == "next") {
      if (render) render->Next(nullptr);
    }
    else if (cmd == "prev") {
      if (render) render->Previous(nullptr);
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
    else {
      printf("unknown cmd %s, supported is\n", cmd.c_str());
      printf(" quit,q \t quit app\n");
      printf(" search, \t search devices\n");
      printf(" list,l \t list devices\n");
      printf(" open,o \t open device\n");
      printf(" opensub \t open and subscribe device\n");
      printf(" close \t close device\n");
      printf(" subscribe,sub \t subscribe device\n");
      printf(" unsubscribe,unsub \t unsubscribe device\n");
      printf(" seturl,url \t set url on opened device\n");
      printf(" seek \t seek player to a pos\n");
      printf(" pause \t pause player\n");
      printf(" resume \t resume player\n");
      printf(" play \t play with speed\n");
      printf(" pos \t get player pos\n");
      printf(" tinfo \t get player transport info\n");
      printf(" minfo \t get player media info\n");
      printf(" next \t play next file\n");
      printf(" prev \t play prev file\n");
      printf(" vol \t get device volume\n");
      printf(" setVol \t set device volume\n");
    }
  }
  upnp->delListener(&listener);
  upnp->stop();
  return 0;
}