#include "UpnpServer.h"
#include "hlog.h"
#include "htime.h"
#include <strstream>
#include "UdpClient.h"
#include "requests.h"
#include "pugixml.hpp"
#include "UpnpRender.h"

const char* ssdpAddres = "239.255.255.250";
const unsigned short ssdpPort = 1900;

const char* getServiceTypeStr(UpnpServiceType t) {
  switch (t)
  {
#define XX(type, id, str) case type : return str;
    SERVICE_MAP(XX)
#undef XX
  default:
    return "";
  }
}

const char* getServiceIdStr(UpnpServiceType t) {
  switch (t)
  {
#define XX(type, id, str) case type : return id;
    SERVICE_MAP(XX)
#undef XX
  default:
    return "";
  }
}

UpnpServiceType getServiceType(const std::string& str)
{
#define XX(type, id, str) {str, type},
  static std::map<std::string, UpnpServiceType, hv::StringCaseLess> codec_map = { SERVICE_MAP(XX) };
#undef XX
  auto it = codec_map.find(str);
  return it == codec_map.end() ? USInvalid : it->second;
}

UpnpServiceType getServiceId(const std::string& str)
{
#define XX(type, id, str) {id, type},
  static std::map<std::string, UpnpServiceType, hv::StringCaseLess> codec_map = { SERVICE_MAP(XX) };
#undef XX
  auto it = codec_map.find(str);
  return it == codec_map.end() ? USInvalid : it->second;
}

std::string concatUrl(const std::string& prefix, const std::string& tail) {
	if (prefix.empty())
		return tail;
	if (tail.empty())
		return prefix;
	if (*prefix.rbegin() != '/' && *tail.begin() != '/')
		return prefix + "/" + tail;
	return prefix + tail;
}

bool ServiceDesc::parseScpd(const std::string& url)
{
	if (url.empty())
		return false;

  auto req = std::make_shared<HttpRequest>();
  req->url = url;
  Upnp::Instance()->httpClient()->send(req, [this, url](HttpResponsePtr resp) {
    if (!resp || resp->status_code != HTTP_STATUS_OK)
      return;

	this->actions_.clear();
	this->stateVals_.clear();
	pugi::xml_document doc;
	doc.load_string(resp->body.c_str());
	auto root = doc.first_child(); // scpd
	auto actions = root.select_nodes("actionList/action");
	hlogi("%s got %d actions", url.c_str(), actions.size());
	for (int j = 0; j < actions.size(); j++) {
		/* <action><name>SelectPreset</name><argumentList><argument/>...</argumentList></action> */
		auto action = actions[j].node();
		auto action_name = action.child("name").child_value();
		hlogi("action%d> %s", j, action_name);
		auto& act = this->actions_[action_name];
		auto args = action.select_nodes("argumentList/argument");
		for (int i = 0; i < args.size(); i++) {
/*
<argument>
	<name>InstanceID</name>
	<direction>in</direction>
	<relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable>
</argument>
*/
			auto arg = args[i].node();
			auto dir = arg.child("direction").child_value();
			auto name = arg.child("name").child_value();
			auto ref = arg.child("relatedStateVariable").child_value();
			hlogi("[%d] %s %s ref=%s", i, dir, name, ref);
			act[name] = std::string(strcasecmp(dir, "in") ? "0" : "1") + ref;
		}
	}

	auto vals = root.select_nodes("serviceStateTable/stateVariable");
	hlogi("%s got %d stateVariables", url.c_str(), vals.size());
	for (int i =0; i<vals.size(); i++)
	{
		auto val = vals[i].node();
/* 
<stateVariable sendEvents="no">
	<name>GreenVideoGain</name>
	<dataType>ui2</dataType>
	<allowedValueRange>
		<minimum>0</minimum>
		<maximum>100</maximum>
		<step>1</step>
	</allowedValueRange>
	<allowedValueList>
	  <allowedValue>Master</allowedValue>
	  <allowedValue>LF</allowedValue>
	  <allowedValue>RF</allowedValue>
	</allowedValueList>
</stateVariable> 
*/
		auto name = val.child("name").child_value();
		auto type = val.child("dataType").child_value();
		std::ostringstream stm;
		auto range = val.child("allowedValueRange");
		if (range){
			stm << "[" << range.child("minimum").child_value() << "-" << range.child("maximum").child_value() 
				<< "/" << range.child("step").child_value() << "]";
		}
		else if(auto list = val.child("allowedValueList")) {
			stm << "[";
			for (auto node = list.first_child(); node; node = node.next_sibling()){
				stm << node.child_value() << ",";
			}
			stm << "]";
		}
		hlogi("[%d] %s %s%s", i, name, type, stm.str().c_str());
		stateVals_[name] = type;
	}
  });
	return true;
}

const char* ServiceDesc::findActionArg(const char* name, const char* arg) const
{
	auto it = actions_.find(name);
	if (it != actions_.end()) {
		auto it2 = it->second.find(arg);
		if (it2 != it->second.end()) {
			return it2->second.c_str();
		}
	}
	return nullptr;
}

void Device::set_location(const std::string& loc)
{
  this->location = loc;
  if (URLHeader.empty()) {
    auto pos = location.find("://");
    if (pos != -1) {
      pos += 3;
      pos = location.find('/', pos);
    }
    else {
      pos = location.find('/');
    }
    if (pos != -1) {
      URLHeader = location.substr(0, pos);
    }
  }
}

std::string Device::getControlUrl(UpnpServiceType t) const {
  auto model = getService(t);
  if (!model || model->controlURL.empty())
    return "";
  return concatUrl(URLHeader, model->controlURL);
}

std::string Device::getEventUrl(UpnpServiceType t) const {
  auto model = getService(t);
  if (!model || model->eventSubURL.empty())
    return "";
  return concatUrl(URLHeader, model->eventSubURL);
}

ServiceModel::Ptr Device::getService(UpnpServiceType t) const
{
  auto it = services_.find(t);
  if (it == services_.end())
    return nullptr;
  return it->second;
}

std::string Device::description() const
{
  std::ostringstream stm;
  stm << "\nuuid:" << uuid
    << "\nlocation:" << location
    << "\nURLHeader:" << URLHeader
    << "\nfriendlyName:" << friendlyName
    << "\nmodelName:" << modelName
    << "\n";
  return stm.str();
}

//////////////////////////////////////////////////
// Upnp
Upnp* Upnp::Instance()
{
  static Upnp upnp;
  return &upnp;
}

Upnp::~Upnp()
{
  stop();
}

void Upnp::addSidListener(const std::string& sid, UpnpSidListener* l)
{
  sid_maps_[sid] = l;
}

void Upnp::delSidListener(const std::string& sid)
{
  sid_maps_.erase(sid);
}

const char* Upnp::getUrlPrefix()
{
  if (!url_prefix_[0]) {
    detectLocalIP();
  }
  return url_prefix_;
}

std::string Upnp::getUrl(const char* loc)
{
  std::string ret(getUrlPrefix());
  if (loc && loc[0]) {
    if (loc[0] != '/')
      ret += '/';
    ret += loc;
  }
  return ret;
}

std::string Upnp::setFile(const char* path, const char* loc) {
  std::string ret;
  if (!loc || !loc[0]) {
    loc = strrchr(path, '/');
    if (!loc) loc = strrchr(path, '\\');
    if (!loc || !loc[0]) return ret;
  }
  if (path && path[0]) {
    file_maps_[loc] = path;
    ret = getUrl(loc);
  }
  else {
    file_maps_.erase(loc);
  }
  return ret;
}

void Upnp::addListener(UpnpListener* l)
{
  auto it = std::find(_listeners.begin(), _listeners.end(), l);
  if (it == _listeners.end()) {
    _listeners.push_back(l);
  }
}

void Upnp::delListener(UpnpListener* l)
{
  auto it = std::find(_listeners.begin(), _listeners.end(), l);
  if (it != _listeners.end()) {
    _listeners.erase(it);
  }
}

void Upnp::detectLocalIP()
{
  int sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_u addr;
  sockaddr_set_ipport(&addr, "8.8.8.8", 553);
  socklen_t addrlen = sockaddr_len(&addr);
  int ret = connect(sock_fd, &addr.sa, addrlen);
  if (-1 == ret) {
    hlogw("connect error %d", ret);
    closesocket(sock_fd);
    return;
  }
  getsockname(sock_fd, &addr.sa, &addrlen);
  closesocket(sock_fd);

  char tmp[32];
  sockaddr_ip(&addr, tmp, sizeof(tmp));
  sprintf(url_prefix_, "http://%s:%d", tmp, _http_server.port);
  hlogi("got url_prefix: %s", url_prefix_);
}

void Upnp::start()
{
  int fd = _socket.createsocket(ssdpPort, ssdpAddres);
  ip_mreq m_membership;
  m_membership.imr_multiaddr.s_addr = inet_addr(ssdpAddres);
  m_membership.imr_interface.s_addr = htons(INADDR_ANY);
  if (0 != setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&m_membership, sizeof(m_membership)))
  {
    stop();
    return;
  }
  _socket.onMessage = [this](const hv::SocketChannelPtr& channel, hv::Buffer* buf) {
    this->onUdpRecv((char*)buf->data(), buf->size());
  };
  _socket.start();
  _http_client = std::make_shared<hv::AsyncHttpClient>(loop());
  startHttp();
  return;
}

void Upnp::startHttp()
{
  // start local httpServer @todo
  _http_service.GET("/live.flv", [](const HttpContextPtr& ctx) {
    return 0;
  });

  _http_service.GET("/*", [this](const HttpContextPtr& ctx) {
    auto it = file_maps_.find(ctx->request->path);
    if (it == file_maps_.end())
      return 404;
    ctx->writer->ResponseFile(it->second.c_str(), ctx->request.get(), _http_service.limit_rate);
    return 0;
  });
  _http_service.Handle("NOTIFY", "/", [this](const HttpContextPtr& ctx) {
    /*
    NOTIFY delivery_path HTTP/1.1
    HOST:delivery_host:delivery_port
    CONTENT-TYPE:  text/xml
    CONTENT-LENGTH: Bytes in body
    NT: upnp:event
    NTS: upnp:propchange
    SID: uuid:subscription-UUID
    SEQ: event key

    <e:propertyset xmlns:e="urn:schemas-upnp-org:event-1-0">
    <e:property>
    <variableName>new value</variableName>
    <e:property>
    Other variable names and values (if any) go here.
    </e:propertyset>
    */
    auto req = ctx->request;
    auto sid = req->GetHeader("SID");
    auto it = sid_maps_.find(sid);
    if (it!=sid_maps_.end()) {
      it->second->onSidMsg(sid, req->body);
    }
    return 200;
  });
  _http_server.registerHttpService(&_http_service);
  _http_server.bindHttp(0);
  _http_server.start();
  detectLocalIP();
}

void Upnp::stop()
{
  _devices.clear();
  _renders.clear();
  _socket.stop();
  _http_server.stop();
  _http_client = nullptr;
}

void Upnp::search(int type, bool use_cache)
{
  auto oldSize = _devices.size();
  if (use_cache) {
    unsigned int now = gettick_ms();
    auto it = _devices.begin();
    while (it!=_devices.end()) {
      if (now > it->second->tick + DEVICE_TIMEOUT)
        it = _devices.erase(it);
      else
        it++;
    }
  }
  else
    _devices.clear();
  if (oldSize != _devices.size())
    onChange();
  const char* sType = getServiceTypeStr(USAVTransport);
  if (!sType || !sType[0])
    sType = "unpn::rootdevice";
  char line[1024];
  int size = sprintf(line, "M-SEARCH * HTTP/1.1\r\nHOST: %s:%d\r\nMAN: \"ssdp:discover\"\r\nMX: 3\r\nST: %s\r\nUSER-AGENT: iOS UPnP/1.1 Tiaooo/1.0\r\n\r\n",
    ssdpAddres, ssdpPort, sType);
  sockaddr_u dst;
  sockaddr_set_ipport(&dst, ssdpAddres, ssdpPort);
  // printf("udp< %s", line);
  _socket.sendto(line, size, &dst.sa);
}

int Upnp::subscribe(const char* id, int type, int sec)
{
  int ret = 0;
  if (auto render = getRender(id))
    render->subscribe(type, sec);
  return ret;
}

int Upnp::unsubscribe(const char* id, int type)
{
  int ret = 0;
  if (auto render = getRender(id))
    render->unsubscribe(type);
  return ret;
}

Device::Ptr Upnp::getDevice(const char* usn)
{
  auto it = _devices.find(usn);
  if (it != _devices.end()) {
    return it->second;
  }
  return nullptr;
}

UpnpRender::Ptr Upnp::getRender(const char* usn)
{
  UpnpRender::Ptr ret;
  auto it = _renders.find(usn);
  if (_renders.end() == it) {
    auto dev = getDevice(usn);
    if (!dev)
      return nullptr;
    ret = std::make_shared<UpnpRender>(dev);
    _renders[usn] = ret;
  }
  else {
    ret = it->second;
  }
  return ret;
}

int Upnp::openUrl(const char* id, const char* url, RpcCB cb)
{
  int ret = 0;
  if (auto render = getRender(id))
    ret = render->setAVTransportURL(url, cb);
  return ret;
}

int Upnp::pause(const char* id, RpcCB cb)
{
  int ret = 0;
  if (auto render = getRender(id))
    ret = render->pause(cb);
  return ret;
}

int Upnp::resume(const char* id, RpcCB cb)
{
  int ret = 0;
  if (auto render = getRender(id))
    ret = render->play(render->speed(), cb);
  return ret;
}

int Upnp::seek(const char* id, float val, RpcCB cb)
{
  int ret = 0;
  if (auto render = getRender(id))
    ret = render->seek(val, cb);
  return ret;
}

int Upnp::setVolume(const char* id, int val, RpcCB cb)
{
  int ret = 0;
  if (auto render = getRender(id))
    ret = render->setVolume(val, cb);
  return ret;
}

int Upnp::getVolume(const char* id, std::function<void(int, int)> cb)
{
  int ret = 0;
  if (auto render = getRender(id))
    ret = render->getVolume(cb);
  return ret;
}

int Upnp::getPosition(const char* id, std::function<void(int, AVPositionInfo)> cb)
{
  int ret = 0;
  if (auto render = getRender(id))
    ret = render->getPositionInfo(cb);
  return ret;
}

int Upnp::getTransportInfo(const char* id, std::function<void(int, TransportInfo)> cb)
{
  int ret = 0;
  if (auto render = getRender(id))
    ret = render->getTransportInfo(cb);
  return ret;
}

int Upnp::getMediaInfo(const char* id) {
  int ret = 0;
  if (auto render = getRender(id))
    ret = render->getMediaInfo(nullptr);
  return ret;  
}

int Upnp::setPlayMode(const char* id, const char* mode, RpcCB cb) {
  int ret = 0;
  if (auto render = getRender(id))
    ret = render->setPlayMode(mode, cb);
  return ret;  
}

int Upnp::next(const char* id, RpcCB cb) {
  int ret = 0;
  if (auto render = getRender(id))
    ret = render->Next(cb);
  return ret;  
}

int Upnp::previous(const char* id, RpcCB cb) {
  int ret = 0;
  if (auto render = getRender(id))
    ret = render->Previous(cb);
  return ret;  
}

int Upnp::setSpeed(const char* id, float speed, RpcCB cb)
{
  int ret = 0;
  if (auto render = getRender(id))
    ret = render->play(speed, cb);
  return ret;
}

float Upnp::getSpeed(const char* id)
{
  float ret = 1.0f;
  if (auto render = getRender(id))
    ret = render->speed();
  return ret;
}

float Upnp::getDuration(const char* id)
{
  float ret = 0.0f;
  if (auto render = getRender(id))
    ret = render->duration();
  return ret;
}

int Upnp::close(const char* id, RpcCB cb)
{
  int ret = 0;
  auto it = _renders.find(id);
  if (_renders.end() != it) {
    auto render = it->second;
    _renders.erase(it);
    if (render) {
      ret = render->stop(cb);
    }
  }
  return ret;
}

void parseHttpHeader(char* buff, int size, http_headers& header) {
  int nline = 0;
  char line[512] = { 0 };
  std::istrstream resp_stm(buff, size);
  while (resp_stm.getline(line, sizeof line)) {
    if (nline++ == 0) {
      int temp_pos;
      unsigned int vmajor, vminor, temp_scode;
      if (sscanf(line, "HTTP %u%n",
        &temp_scode, &temp_pos) == 1) {
        // This server's response has no version. :( NOTE: This happens for every
        // response to requests made from Chrome plugins, regardless of the server's
        // behaviour.
      }
      else if ((sscanf(line, "HTTP/%u.%u %u%n",
        &vmajor, &vminor, &temp_scode, &temp_pos) == 3)
        && (vmajor == 1)) {
        // This server's response does have a version.
      }
      else {

      }
      //resp_code = temp_scode;
    }
    else {
      char* p = ::strchr(line, ':');
      if (p) {
        *p++ = 0;
        std::string key = hv::trim(line);
        if (key.length())
          header[key] = hv::trim(p);
      }
    }
  }
}

void Upnp::onUdpRecv(char* buff, int size)
{
  char* p = buff;
  http_headers header;
  // printf("udp> %.*s", buffer->size(), p);
  if (!strncasecmp(p, "NOTIFY", 6)) {
    parseHttpHeader(buff, size, header);
    auto serviceType = getServiceType(header["NT"].c_str());
    if (serviceType == USAVTransport) {
      std::string location = header["Location"];
      std::string usn = header["USN"];
      std::string ssdp = header["NTS"];
      if (location.empty() || usn.empty() || ssdp.empty()) {
        hlogi("skip ");
        return;
      }
      if (ssdp == "ssdp:alive") {
        if (!_devices.count(usn)) {
          loadDeviceWithLocation(location, usn);
        }
      }
      else if (ssdp == "ssdp:byebye") {
        delDevice(usn);
      }
    }
  }
  else if (!strncasecmp(p, "HTTP/1.1", 8)) {
    parseHttpHeader(buff, size, header);
    std::string location = header["Location"];
    std::string usn = header["USN"];
    if (location.empty() || usn.empty()) {
      return;
    }
    loadDeviceWithLocation(location, usn);
  }
}

void Upnp::delDevice(const std::string& usn)
{
  auto it = _devices.find(usn);
  if (it != _devices.end()) {
    _devices.erase(it);
  }
  onChange();
}

void Upnp::loadDeviceWithLocation(std::string loc, std::string usn)
{
  auto req = std::make_shared<HttpRequest>();
  req->url = loc;
  _http_client->send(req, [this, loc, usn](HttpResponsePtr resp) {
    if (!resp || resp->status_code != HTTP_STATUS_OK)
      return;

    pugi::xml_document doc;
    doc.load_string(resp->Body().c_str());
    auto root = doc.first_child();
    auto dev = root.child("device");
    if (dev) {
      std::string uuid;
      if (dev.child("UDN")) {
        uuid = dev.child("UDN").child_value();
      }
      else {
        auto pos = usn.find("::");// "::urn:");
        if (-1 != pos && pos > 0)
          uuid = usn.substr(0, pos);
        else
          uuid = usn;
      }

      Device::Ptr device = _devices[uuid];
      if (!device) {
        device = std::make_shared<Device>();
        device->uuid = uuid;
        _devices[uuid] = device;
      }
      device->tick = gettick_ms();
      device->location = loc;
      device->friendlyName = dev.child("friendlyName").child_value();
      device->modelName = dev.child("modelName").child_value();
      if (auto url = root.child("URLBase")) {
        device->URLHeader = hv::rtrim(url.child_value(), "/");
      }
      device->set_location(loc);
      for (auto service : dev.child("serviceList").children("service")) {
        /*
<service>
<serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>
<serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>
<SCPDURL>/upnp/rendertransportSCPD.xml</SCPDURL>
<controlURL>/upnp/control/rendertransport1</controlURL>
<eventSubURL>/upnp/event/rendertransport1</eventSubURL>
</service>
        */
        auto serviceType = service.child("serviceType").child_value();
        auto serviceId = service.child("serviceId").child_value();
        UpnpServiceType type = getServiceId(serviceId);
        if (type == USInvalid)
          type = getServiceType(serviceType);
        if (type == USInvalid) {
          hlogi("skip unkonwn type %s %s", serviceType, serviceId);
          continue;
        }

        auto controlURL = service.child("controlURL").child_value();
        auto eventSubURL = service.child("eventSubURL").child_value();
        auto scpdURL = service.child("SCPDURL").child_value();
        auto sm = device->getService(type);
        if (!sm) {
          sm = std::make_shared<ServiceModel>();
          device->services_[type] = sm;
        }
        sm->serviceType = serviceType;
        sm->serviceId = serviceId;
        sm->controlURL = controlURL;
        sm->eventSubURL = eventSubURL;
        if (sm->scpdURL != scpdURL) {
          std::string url = concatUrl(device->URLHeader, scpdURL);
          if (sm->desc.parseScpd(url)) {
            sm->scpdURL = scpdURL;
          }
        }
      }
      hlogi("gotDevice %s", device->description().c_str());
      onChange();
      //addDevice(device);
    }
  });
}

void Upnp::addDevice(Device::Ptr device)
{
  hlogi("addDevice %s", device->description().c_str());
  _devices[device->uuid] = device;
  onChange();
}

void Upnp::onChange() {
  for (auto l : _listeners) {
    l->upnpSearchChangeWithResults(_devices);
  }
}
