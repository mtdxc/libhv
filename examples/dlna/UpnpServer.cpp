#include "UpnpServer.h"
#include "hlog.h"
#include <strstream>
#include "UdpClient.h"
#include "requests.h"
#include "pugixml.hpp"

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

std::string Device::getPostUrl(UpnpServiceType t) const {
  auto model = getService(t);
  if(!model || model->controlURL.empty())
    return "";
  std::string ret = URLHeader;
  if (model->controlURL[0] != '/')
      ret += "/";
  return ret + model->controlURL;
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


Upnp* Upnp::Instance()
{
  static Upnp upnp;
  return &upnp;
}

Upnp::~Upnp()
{
  stop();
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
}

void Upnp::stop()
{
  _socket.stop();
  _http_client = nullptr;
}

void Upnp::search()
{
  _devices.clear();
  onChange();

  char line[1024];
  int size = sprintf(line, "M-SEARCH * HTTP/1.1\r\nHOST: %s:%d\r\nMAN: \"ssdp:discover\"\r\nMX: 3\r\nST: %s\r\nUSER-AGENT: iOS UPnP/1.1 Tiaooo/1.0\r\n\r\n", 
    ssdpAddres, ssdpPort, getServiceTypeStr(USAVTransport));
  sockaddr_u dst;
  sockaddr_set_ipport(&dst, ssdpAddres, ssdpPort);
  // printf("udp< %s", line);
  _socket.sendto(line, size, &dst.sa);
}

Device::Ptr Upnp::getDevice(const std::string& usn)
{
  auto it = _devices.find(usn);
  if (it != _devices.end()) {
    return it->second;
  }
  return nullptr;
}

void parseHttpHeader(char* buff, int size, http_headers& header, std::string* fline) {
  int nline = 0;
  char line[512] = { 0 };
  std::istrstream resp_stm(buff, size);
  while (resp_stm.getline(line, sizeof line)) {
    if (nline++ == 0) {
      if (fline)
        *fline = line;
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
  // printf("udp> %.*s", buffer->size(), p);
  if (!strncasecmp(p, "NOTIFY", 6)) {
    http_headers header;
    parseHttpHeader(buff, size, header, nullptr);
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
    http_headers header;
    parseHttpHeader(buff, size, header, nullptr);
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
  _http_client->send(req, [this,loc, usn](HttpResponsePtr resp) {
    if (!resp || resp->status_code != HTTP_STATUS_OK)
      return;

    pugi::xml_document doc;
    doc.load_string(resp->Body().c_str());
    auto root = doc.first_child();
    auto dev = root.child("device");
    if (dev) {
      Device::Ptr device = std::make_shared<Device>();
      device->location = loc;
      device->uuid = usn;
      device->friendlyName = dev.child("friendlyName").child_value();
      device->modelName = dev.child("modelName").child_value();
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
        auto sm = std::make_shared<ServiceModel>();
        sm->serviceType = service.child("serviceType").child_value();
        sm->serviceId = service.child("serviceId").child_value();
        sm->controlURL = service.child("controlURL").child_value();
        sm->eventSubURL = service.child("eventSubURL").child_value();
        sm->SCPDURL = service.child("SCPDURL").child_value();
        auto type1 = getServiceId(sm->serviceId);
        auto type2 = getServiceType(sm->serviceType);
        if (type1 != USInvalid) {
          device->services_[type1] = sm;
        }
        else if (type2 != USInvalid) {
          device->services_[type1] = sm;
        }
      }
      if (auto url = root.child("URLBase")) {
        device->URLHeader = hv::rtrim(url.child_value(), "/");
      }
      device->set_location(loc);
      addDevice(device);
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
  if (_listener) {
    _listener->upnpSearchChangeWithResults(_devices);
  }
}

