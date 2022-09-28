#include "UpnpRender.h"
#include <sstream>
#include "requests.h"
#include "pugixml.hpp"
using namespace pugi;
template<typename T>
bool create_attr_node(xml_node& node, const char_t* pAttrName, const T& attrVal) {
	pugi::xml_attribute attr = node.append_attribute(pAttrName);
	return (attr && attr.set_value(attrVal));
}
const char* unitREL_TIME = "REL_TIME";
const char* unitTRACK_NR = "TRACK_NR";

typedef std::map<std::string, std::string> ArgMap;
class UPnPAction {
    UpnpServiceType type_;
    std::string action_;
    xml_document doc_;
    xml_node ele_;
public:
    UPnPAction(const char* name) : action_(name) {
      type_ = USAVTransport;
      std::string t = "u:" + action_;
      auto ele = doc_.append_child("s:Envelope");
      create_attr_node(ele, "s:encodingStyle", "http://schemas.xmlsoap.org/soap/encoding/");
      create_attr_node(ele, "xmlns:s", "http://schemas.xmlsoap.org/soap/envelope/");
      ele_ = ele.append_child("s:Body").append_child(t.c_str());
    }

    void setArgs(const char* name, const char* value);

    void setServiceType(UpnpServiceType t) {
      type_ = t;
    }
    UpnpServiceType getServiceType() const {return type_;}

    std::string getSOAPAction() const;
    std::string getPostXML();

    typedef std::function<void(int, ArgMap&)> RpcCB;
    int invoke(Device::Ptr dev, RpcCB cb);
};

void UPnPAction::setArgs(const char* name, const char* value)
{
  auto node = ele_.append_child(name);
  node.text().set(value);
}

std::string UPnPAction::getSOAPAction() const
{
  char buff[256] = { 0 };
  sprintf(buff, "\"%s#%s\"", getServiceTypeStr(type_), action_.c_str());
  return buff;
}

std::string UPnPAction::getPostXML()
{
  auto eve = doc_.child("s:Envelope");
  create_attr_node(eve, "xmlns:u", getServiceTypeStr(type_));
  std::ostringstream stm;
  doc_.save(stm);
  return stm.str();
}

int UPnPAction::invoke(Device::Ptr dev, RpcCB cb)
{
  auto req = std::make_shared<HttpRequest>();
  req->method = HTTP_POST;
  req->url = dev->getPostUrl(type_);
  req->body = getPostXML();
  req->SetHeader("Content-Type", "text/xml");
  req->SetHeader("SOAPAction", getSOAPAction());
  static std::atomic<int> action_id(0);
  int id = action_id++;
  hlogd("soap %d> %s", id, req->body.c_str());
  std::string respTag = "u:" + action_ + "Response";
  auto cb1 = [id](int code, std::map<std::string, std::string>& args) {
    if (auto listener = Upnp::Instance()->getListener())
      listener->unppActionResponse(id, code, args);
  };
  if (!cb) {
    cb = cb1;
  }
  else {
    auto cb2 = cb;
    cb = [cb1, cb2](int code, std::map<std::string, std::string>& args) {
      cb1(code, args);
      cb2(code, args);
    };
  }

  requests::async(req, [cb, id, respTag](HttpResponsePtr resp) {
    ArgMap args;
    if (!resp) {
      args["error"] = "without response";
      cb(-1, args);
      return;
    }

    hlogd("soap %d< %d %s", id, resp->status_code, resp->body.c_str());
    if (resp->status_code != 200) {
      args["error"] = "response code error";
      if (resp->body.length())
        args["detail"] = resp->body;
      cb(resp->status_code, args);
      return;
    }

    xml_document doc;
    auto ret = doc.load_string(resp->body.c_str());
    if (ret.status) {
      args["error"] = "xml parse error";
      args["detail"] = ret.description();
      cb(-3, args);
      return;
    }
    /*
    <s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>
      <u:SetAVTransportURIResponse xmlns:u="urn:schemas-upnp-org:service:AVTransport:1"></u:SetAVTransportURIResponse>
    </s:Body></s:Envelope>
    */
    auto body = doc.child("s:Envelope").child("s:Body");
    if (!body) {
      hlogi("soapResp missing tag %s", respTag.c_str());
      args["error"] = "missing body tag";
      cb(-2, args);
    }
    else {
      auto resp = body.child(respTag.c_str());
      if (resp) {
        // xml to map
        for (auto child : resp.children()) {
          args[child.name()] = child.text().as_string("");
        }
        cb(0, args);
      }
      else {
        /* <s:Body><s:Fault>
<faultcode>s:Client</faultcode>
<faultstring>UPnPError</faultstring>
<detail>
<UPnPError xmlns="urn:schemas-upnp-org:control-1-0">
<errorCode>501</errorCode>
<errorDescription>Action Failed</errorDescription>
</UPnPError>
</detail>
</s:Fault></s:Body>*/
        auto fault = body.child("s:Fault");
        if (fault) {
          args["error"] = fault.child("faultstring").text().as_string();
          std::string code = fault.child("faultcode").text().as_string();
          std::ostringstream stm;
          fault.child("detail").first_child().print(stm, "");
          args["detail"] = stm.str();
        }
        else {
          args["error"] = "missing fault tag";
        }
        cb(-4, args);
      }

    }
  });
  return id;
}

int UpnpRender::setAVTransportURL(const char* urlStr, RpcCB cb)
{
  url_ = urlStr;
  UPnPAction action("SetAVTransportURI");
  action.setArgs("InstanceID", "0");
  action.setArgs("CurrentURI", urlStr);
  action.setArgs("CurrentURIMetaData", "");
  return action.invoke(model_, [cb, this](int code, ArgMap& args) {
    if(cb) cb(code, args["error"]);
    if (code) return;
    getTransportInfo([this](int code, TransportInfo ti) {
      if (code) return;
      printf("state=%s, status=%s, speed=%f\n", ti.currentTransportState.c_str(), ti.currentTransportStatus.c_str(), ti.currentSpeed);
      if (ti.currentTransportState != "PLAYING" && ti.currentTransportState != "TRANSITIONING")
        play(nullptr);
      getPositionInfo([this](int code, AVPositionInfo pos) {
        printf("%d duration=%f, curTime=%f\n", code, pos.trackDuration, pos.absTime);
        this->duration_ = pos.trackDuration;
      });
    });
  });
}

int UpnpRender::play(RpcCB cb)
{
  UPnPAction action("Play");
  action.setArgs("InstanceID", "0");
  return action.invoke(model_, [cb](int code, ArgMap& args) {
    if (cb) cb(code, args["error"]);
  });
}

int UpnpRender::pause(RpcCB cb)
{
  UPnPAction action("Pause");
  action.setArgs("InstanceID", "0");
  return action.invoke(model_, [cb](int code, ArgMap& args) {
    if (cb) cb(code, args["error"]);
  });
}

int UpnpRender::stop(RpcCB cb)
{
  UPnPAction action("Stop");
  action.setArgs("InstanceID", "0");
  return action.invoke(model_, [cb](int code, ArgMap& args) {
    if (cb) cb(code, args["error"]);
  });
}

int UpnpRender::seek(float relTime, RpcCB cb)
{
  char szTime[64];
  int sec = (int)relTime % 3600;
  sprintf(szTime, "%02d:%02d:%02d", (int)relTime / 3600, sec / 60, sec % 60);
  return seekToTarget(szTime, unitREL_TIME, cb);
}

int UpnpRender::seekToTarget(const char* target, const char* unit, RpcCB cb)
{
  UPnPAction action("Seek");
  action.setArgs("InstanceID", "0");
  action.setArgs("Unit", unit);
  action.setArgs("Target", target);
  return action.invoke(model_, [cb](int code, ArgMap& args) {
    if (cb) cb(code, args["error"]);
  });
}

float strToDuraton(std::string str) {
  auto list = hv::split(str, ':');
  if (list.size() < 3)
    return -1.0f;
  float ret = 0;
  for (int i=0; i< list.size(); i++)
  {
    int val = std::stoi(list[i]);
    switch (i)
    {
    case 0:
      ret += val * 3600;
      break;
    case 1:
      ret += val * 60;
      break;
    case 2:
      ret += val;
      break;
    case 3:
      ret += val * 0.1;
    default:
      break;
    }
  }
  return ret;
}

int UpnpRender::getPositionInfo(std::function<void(int, AVPositionInfo)> cb)
{
  UPnPAction action("GetPositionInfo");
  action.setArgs("InstanceID", "0");
  return action.invoke(model_, [cb, this](int code, ArgMap& args) {
    if (!cb) return;
    AVPositionInfo pos;
    if (code == 0) {
      pos.trackDuration = strToDuraton(args["TrackDuration"]);
      if (pos.trackDuration > 0)
        this->duration_ = pos.trackDuration;
      pos.relTime = strToDuraton(args["RelTime"]);
      pos.absTime = strToDuraton(args["AbsTime"]);
    }
    cb(code, pos);
  });
}

int UpnpRender::getTransportInfo(std::function<void(int, TransportInfo)> cb)
{
  UPnPAction action("GetTransportInfo");
  action.setArgs("InstanceID", "0");
  return action.invoke(model_, [cb](int code, ArgMap& args) {
    if (!cb) return;
    TransportInfo ti;
    if (code == 0) {
      ti.currentTransportState = args["CurrentTransportState"];
      ti.currentTransportStatus = args["CurrentTransportStatus"];
      ti.currentSpeed = std::stof(args["CurrentSpeed"]);
    }
    cb(code, ti);
  });
}

int UpnpRender::getVolume(std::function<void(int, int)> cb)
{
  UPnPAction action("GetVolume");
  action.setServiceType(USRenderingControl);
  action.setArgs("InstanceID", "0");
  action.setArgs("Channel", "Master");
  return action.invoke(model_, [cb](int code, ArgMap& args) {
    if (!cb) return;
    int vol = std::stoi(args["CurrentVolume"]);
    cb(code, vol);
  });
}

int UpnpRender::setVolume(int value, RpcCB cb)
{
  char buff[20];
  sprintf(buff, "%d", value);
  return setVolumeWith(buff, cb);
}

int UpnpRender::setVolumeWith(const char* value, RpcCB cb)
{
  UPnPAction action("SetVolume");
  action.setServiceType(USRenderingControl);
  action.setArgs("InstanceID", "0");
  action.setArgs("Channel", "Master");
  action.setArgs("DesiredVolume", value);
  return action.invoke(model_, [cb](int code, ArgMap& args) {
    if (cb) cb(code, args["error"]);
  });
}
