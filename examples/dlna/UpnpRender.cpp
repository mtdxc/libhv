#include "UpnpRender.h"
#include <sstream>
#include "requests.h"
#include "pugixml.hpp"
#include "htime.h"

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
  UpnpServiceType getServiceType() const { return type_; }

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

void dumpArgs(const ArgMap& map){
  for (auto it : map) {
    hlogi(" %s=%s", it.first.c_str(), it.second.c_str());
  }
}

int UPnPAction::invoke(Device::Ptr dev, RpcCB cb)
{
  auto req = std::make_shared<HttpRequest>();
  req->method = HTTP_POST;
  req->url = dev->getControlUrl(type_);
  req->body = getPostXML();
  req->SetHeader("Content-Type", "text/xml");
  req->SetHeader("SOAPAction", getSOAPAction());
  static std::atomic<int> action_id(0);
  int id = action_id++;
  std::string respTag = "u:" + action_ + "Response";
  auto cb1 = [id, dev](int code, std::map<std::string, std::string>& args) {
    if (code) {
      hlogw("soap %d error %d,%s detail=%s", id, code, args["error"].c_str(), args["detail"].c_str());
      dev->tick -= DEVICE_TIMEOUT / 3;
    }
    else{
      dev->tick = gettick_ms();
    }
    for (auto listener : Upnp::Instance()->getListeners())
      listener->upnpActionResponse(id, code, args);
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

  auto http = Upnp::Instance()->httpClient();
  if (!http) {
    hlogw("skip soap %d> %s", id, req->body.c_str());
    return 0;
  }
  hlogd("soap %d> %s", id, req->body.c_str());
  http->send(req, [cb, id, respTag](HttpResponsePtr resp) {
    ArgMap args;
    if (!resp) {
      args["error"] = "without response";
      cb(-1, args);
      return;
    }

    hlogd("soap %d< %d %s", id, resp->status_code, resp->body.c_str());
    if (resp->status_code != 200) {
      args["error"] = "response code error";
      args["detail"] = resp->body;
    }

    xml_document doc;
    auto ret = doc.load_string(resp->body.c_str());
    if (ret.status) {
      args["error"] = std::string("xml parse error: ") + ret.description();
      args["detail"] = resp->body;
      cb(ret.status, args);
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
      args["detail"] = resp->body;
      cb(-2, args);
    }
    else {
      if (auto rnode = body.child(respTag.c_str())) {
        // xml to map
        for (auto child : rnode.children()) {
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
        std::string code;
        auto fault = body.child("s:Fault");
        if (fault) {
          auto error = fault.child("faultstring").child_value();
          if (strcasecmp(error, "UPnPError")) {
            code = fault.child("faultcode").child_value();
            std::ostringstream stm;
            fault.child("detail").first_child().print(stm, "");
            args["detail"] = stm.str();
            args["error"] = error;
          }
          else {
            auto node = fault.select_node("detail/UPnPError").node();
            code = node.child("errorCode").child_value();
            args["error"] = node.child("errorDescription").child_value();
          }
        }
        else {
          args["error"] = "missing fault tag";
        }
        int ncode = atoi(code.c_str());
        if (!ncode) ncode = resp->status_code;
        cb(ncode, args);
      }
    }
  });
  return id;
}

UpnpRender::UpnpRender(Device::Ptr dev) :model_(dev) {
  if (auto service = model_->getService(USAVTransport)) {
    support_speed_ = service->desc.hasActionArg("Play", "Speed");
    hlogi("support_speed=%d", support_speed_);
  }
}

UpnpRender::~UpnpRender()
{
  hlogi("%s", devId());
  if (!url_.empty())
    stop();
  auto sids = sid_map_;
  for (auto it : sids)
    unsubscribe(it.first);
}

const char* UpnpRender::devId() const {
  return model_->uuid.c_str();
}

const char* UpnpRender::devName() const {
  return model_->friendlyName.c_str();
}

void UpnpRender::subscribe(int type, int sec)
{
  auto url = model_->getEventUrl((UpnpServiceType)type);
  if (url.empty())
    return;

  /*
  SUBSCRIBE publisher_path HTTP/1.1
  HOST: publisher_host:publisher_port
  CALLBACK: <delivery URL>
  NT: upnp:event
  TIMEOUT: second-[requested subscription duration]
  */
  auto req = std::make_shared<HttpRequest>();
  req->method = HTTP_SUBSCRIBE;
  req->url = url;

  auto it = sid_map_.find(type);
  if (it == sid_map_.end()) { // 全新订阅
    req->SetHeader("NT", "upnp:event");
    req->SetHeader("CALLBACK", std::string("<") + Upnp::Instance()->getUrlPrefix() + "/>");
  }
  else { // 续订
    req->SetHeader("SID", it->second);
  }
  char timeout[64];
  if (sec > 0) {
    sprintf(timeout, "Second-%d", sec);
  }
  else {
    strcpy(timeout, "Second-infinite");
  }
  req->SetHeader("TIMEOUT", timeout);

  std::weak_ptr<UpnpRender> weak_ptr = shared_from_this();
  Upnp::Instance()->httpClient()->send(req, [weak_ptr, type](HttpResponsePtr res){
    if (!res) {
      hlogi("subscribe error");
      return;
    }
    if (res->status_code != 200) {
      hlogi("subscribe error %d, %s", res->status_code, res->body.c_str());
      return;
    }
    /*
    HTTP/1.1 200 OK
    DATE: when response was generated
    SERVER: OS/version UPnP/1.0 product/version
    SID: uuid:subscription-UUID
    TIMEOUT: second-[actual subscription duration]
    */
    std::string sid = res->GetHeader("SID");
    hlogi("subscribe return %s, timeout=%s", sid.c_str(), res->GetHeader("TIMEOUT").c_str());
    auto strong_ptr = weak_ptr.lock();
    if (!strong_ptr) return;
    auto oldsid = strong_ptr->sid_map_[type];
    if (oldsid != sid) {
      if (oldsid.length())
        Upnp::Instance()->delSidListener(oldsid);
      strong_ptr->sid_map_[type] = sid;
      Upnp::Instance()->addSidListener(sid, strong_ptr.get());
    }
  });
}

void UpnpRender::unsubscribe(int type)
{
  auto it = sid_map_.find(type);
  if (it == sid_map_.end())
    return;
  std::string sid = it->second;
  sid_map_.erase(it);
  Upnp::Instance()->delSidListener(sid);

  auto url = model_->getEventUrl((UpnpServiceType)type);
  if (url.empty())
    return;
  /*
  UNSUBSCRIBE publisher_path HTTP/1.1
  HOST:  publisher_host:publisher_port
  SID: uuid: subscription UUID
  */
  auto req = std::make_shared<HttpRequest>();
  req->method = HTTP_UNSUBSCRIBE;
  req->url = url;
  req->SetHeader("SID", sid);
  Upnp::Instance()->httpClient()->send(req, [=](HttpResponsePtr res) {
    if (!res) {
      hlogi("unsubscribe error");
    }
    else if (res->status_code != 200) {
      hlogi("unsubscribe error %d, %s", res->status_code, res->body.c_str());
    }
    else {
      hlogi("unsubscribe %s ok", sid.c_str());
    }
  });
}

void UpnpRender::onPropChange(const std::string& name, const std::string& value)
{
  hlogi("%s onPropChange %s=%s", model_->uuid.c_str(), name.c_str(), value.c_str());
  if (name == "CurrentMediaDuration") {
    duration_ = strToDuraton(value.c_str());
  }
  for (auto l : Upnp::Instance()->getListeners()) {
    l->upnpPropChanged(model_->uuid.c_str(), name.c_str(), value.c_str());
  }
}

void UpnpRender::onSidMsg(const std::string& sid, const std::string& body)
{
  // hlogi("%s onSidMsg %s", model_->uuid.c_str(), body.c_str());
  xml_document doc;
  auto ret = doc.load_string(body.c_str());
  if (ret.status) {
    hlogi("onSidMsg xml parse %s error %s", body.c_str(), ret.description());
    return;
  }
  /*
  <e:propertyset xmlns:e="urn:schemas-upnp-org:event-1-0"><e:property>
  <LastChange>&lt;Event xmlns = &quot;urn:schemas-upnp-org:metadata-1-0/AVT/&quot;&gt;&lt;/Event&gt;</LastChange>
  </e:property></e:propertyset>
  */
  for (xml_node prop = doc.first_child(); prop; prop = prop.next_sibling()) {
    // property
    for (xml_node change = prop.first_child(); change; change = change.next_sibling()) {
      for (auto child : change.children()) {
        std::string name = child.name();
        std::string value = child.text().as_string();
        if (name == "LastChange") {
          if (value.empty()) return;
          /*
          <Event xmlns = "urn:schemas-upnp-org:metadata-1-0/AVT/"><InstanceID val="0">
          <TransportState val="PLAYING"/><TransportStatus val="OK"/>
          </InstanceID></Event>
          */
          xml_document event;
          auto ret = event.load_string(value.c_str());
          if (ret.status) {
            hlogi("onSidMsg xml parse %s error %s", value.c_str(), ret.description());
            continue;
          }
          for (auto inst : event.first_child().first_child()) {
            std::string name = inst.name();
            std::string value = inst.attribute("val").as_string();
            onPropChange(name, value);
          }
        }
        else {
          onPropChange(name, value);
        }
      }
    }
  }
}

int UpnpRender::setAVTransportURL(const char* urlStr, RpcCB cb){
  std::string url = urlStr;
  hlogi("%s url %s", devId(), urlStr);
  UPnPAction action("SetAVTransportURI");
  action.setArgs("InstanceID", "0");
  action.setArgs("CurrentURI", urlStr);
  action.setArgs("CurrentURIMetaData", "");
  std::weak_ptr<UpnpRender> weak_ptr = shared_from_this();
  return action.invoke(model_, [cb, url](int code, ArgMap& args) {
    if (cb)
      cb(code, code ? args["error"] : url);
  });
}

int UpnpRender::play(const char* urlStr, RpcCB cb)
{
  subscribe(USAVTransport, 3600);
  hlogi("%s url %s", devId(), urlStr);
  std::string url = urlStr;
  std::weak_ptr<UpnpRender> weak_ptr = shared_from_this();
  return setAVTransportURL(urlStr, [cb, weak_ptr, url](int code, std::string resp){
    auto strong_ptr = weak_ptr.lock();
    if (!strong_ptr) return;
    if (code) { // return error
      if (cb) cb(code, resp);
      return;
    }
    strong_ptr->url_ = url;
    strong_ptr->getTransportInfo([weak_ptr](int code, TransportInfo ti) {
      if (code) return;
      auto strong_ptr = weak_ptr.lock();
      if (!strong_ptr) return;
      printf("state=%s, status=%s, speed=%f\n", ti.state, ti.status, ti.speed);
      strong_ptr->speed_ = ti.speed;
      std::string state = ti.state;
      if (state != "PLAYING" && state != "TRANSITIONING")
        strong_ptr->play();
    });
    strong_ptr->getPositionInfo([weak_ptr, cb](int code, AVPositionInfo pos) {
      auto strong_ptr = weak_ptr.lock();
      if (!strong_ptr) return;
      printf("%d duration=%f, curTime=%f\n", code, pos.trackDuration, pos.absTime);
      strong_ptr->duration_ = pos.trackDuration;
      if (cb) cb(code, "");
    });
  });
}

int UpnpRender::play(float speed, RpcCB cb)
{
  hlogi("%s speed=%f", devId(), speed);
  UPnPAction action("Play");
  action.setArgs("InstanceID", "0");
  if (support_speed_) {
    char buf[32];
    sprintf(buf, "%f", speed);
    action.setArgs("Speed", buf);
  }
  //this->speed_ = speed;
  std::weak_ptr<UpnpRender> weak_ptr = shared_from_this();
  return action.invoke(model_, [=](int code, ArgMap& args) {
    auto strong_ptr = weak_ptr.lock();
    if (!strong_ptr) return ;
    if (code) {
      if (strong_ptr->support_speed_ && -1 != args["error"].find("Speed not support")) {
        hlogi("reset support_speed and try");
        strong_ptr->support_speed_ = false;
        strong_ptr->play(1.0f, cb);
        return;
      }
    }
    else
      strong_ptr->speed_ = speed;
    if (cb) cb(code, args["error"]);
  });
}

int UpnpRender::pause(RpcCB cb)
{
  hlogi("%s", devId());
  UPnPAction action("Pause");
  action.setArgs("InstanceID", "0");
  return action.invoke(model_, [cb](int code, ArgMap& args) {
    if (cb) cb(code, args["error"]);
  });
}

int UpnpRender::stop(RpcCB cb)
{
  hlogi("%s", devId());
  UPnPAction action("Stop");
  action.setArgs("InstanceID", "0");
  url_.clear();
  duration_ = 0;
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
  hlogi("%s %s", devId(), target);
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
  for (int i = 0; i < list.size(); i++)
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
  std::weak_ptr<UpnpRender> weak_ptr = shared_from_this();
  return action.invoke(model_, [cb, weak_ptr](int code, ArgMap& args) {
    hlogi("getPositionInfo return %d", code);
    dumpArgs(args);
    auto strong_ptr = weak_ptr.lock();
    if (!cb || !strong_ptr) return;
    AVPositionInfo pos;
    if (code == 0) {
      pos.trackDuration = strToDuraton(args["TrackDuration"]);
      if (pos.trackDuration > 0)
        strong_ptr->duration_ = pos.trackDuration;
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
    hlogi("getTransportInfo return %d", code);
    dumpArgs(args);
    if (!cb) return;
    TransportInfo ti;
    if (code == 0) {
      ti.setState(args["CurrentTransportState"].c_str());
      ti.setStatus(args["CurrentTransportStatus"].c_str());
      ti.speed = std::stof(args["CurrentSpeed"]);
    }
    cb(code, ti);
  });
}

int UpnpRender::getMediaInfo(std::function<void(int, std::map<std::string, std::string>)> cb) {
  UPnPAction action("GetMediaInfo");
  action.setArgs("InstanceID", "0");
  return action.invoke(model_, [cb](int code, ArgMap& args) {
    hlogi("getMediaInfo return %d", code);
    dumpArgs(args);
    if(cb) cb(code, args);
  });
}

int UpnpRender::getDeviceCapabilities(std::function<void(int, DeviceCap)> cb) {
  UPnPAction action("GetDeviceCapabilities");
  action.setArgs("InstanceID", "0");
  return action.invoke(model_, [cb](int code, ArgMap& args) {
    hlogi("getDeviceCapabilities return %d", code);
    dumpArgs(args);
    if(!cb) return;
    DeviceCap cap;
    if (!code) {
      cap.PlayMedia = args["PlayMedia"];
      cap.RecMedia = args["RecMedia"];
      cap.RecQualityModes = args["RecQualityModes"];
    }
  });
}

int UpnpRender::setPlayMode(const char* mode, RpcCB cb) {
  UPnPAction action("SetPlayMode");
  action.setArgs("InstanceID", "0");
  action.setArgs("NewPlayMode", mode);
  return action.invoke(model_, [cb](int code, ArgMap& args) {
    if(cb) cb(code, args["error"]);
  });
}

int UpnpRender::Previous(RpcCB cb){
  UPnPAction action("Previous");
  action.setArgs("InstanceID", "0");
  return action.invoke(model_, [cb](int code, ArgMap& args) {
    if(cb) cb(code, args["error"]);
  });
}

int UpnpRender::Next(RpcCB cb) {
  UPnPAction action("Next");
  action.setArgs("InstanceID", "0");
  return action.invoke(model_, [cb](int code, ArgMap& args) {
    if(cb) cb(code, args["error"]);
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
  hlogi("%s %s", devId(), value);
  UPnPAction action("SetVolume");
  action.setServiceType(USRenderingControl);
  action.setArgs("InstanceID", "0");
  action.setArgs("Channel", "Master");
  action.setArgs("DesiredVolume", value);
  return action.invoke(model_, [cb](int code, ArgMap& args) {
    if (cb) cb(code, args["error"]);
  });
}

TransportInfo::TransportInfo()
{
  state[0] = status[0] = 0; speed = 1.0f;
}

void TransportInfo::setState(const char* v)
{
  if (v)
    strncpy(state, v, sizeof(state));
}

void TransportInfo::setStatus(const char* v)
{
  if (v)
    strncpy(status, v, sizeof(status));
}
