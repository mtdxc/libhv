#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include "UpnpServer.h"

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
  upnp->search();
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