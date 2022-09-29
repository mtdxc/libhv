#pragma once
#include <map>
#include <string>
#include <memory>
#include "UdpClient.h"
#include "AsyncHttpClient.h"
#include "HttpServer.h"
extern const char* ssdpAddres;
extern const unsigned short ssdpPort;

enum UpnpServiceType {
  USInvalid = -1,
  USAVTransport,
  USRenderingControl,
};
#define SERVICE_MAP(XX) \
    XX(USAVTransport, "urn:upnp-org:serviceId:AVTransport", "urn:schemas-upnp-org:service:AVTransport:1") \
    XX(USRenderingControl, "urn:upnp-org:serviceId:RenderingControl", "urn:schemas-upnp-org:service:RenderingControl:1")  \

const char* getServiceTypeStr(UpnpServiceType t);
const char* getServiceIdStr(UpnpServiceType t);
UpnpServiceType getServiceType(const std::string& p);
UpnpServiceType getServiceId(const std::string& p);

struct ServiceModel {
  std::string serviceType, serviceId;
  std::string controlURL, eventSubURL, SCPDURL;
  typedef std::shared_ptr<ServiceModel> Ptr;
};

struct Device {
  std::string uuid;
  std::string location, URLHeader;
  std::string friendlyName;
  std::string modelName;

  std::map<UpnpServiceType, ServiceModel::Ptr> services_;
  void set_location(const std::string& loc);
  std::string description() const;
  std::string getPostUrl(UpnpServiceType t) const;
  ServiceModel::Ptr getService(UpnpServiceType t) const;
  typedef std::shared_ptr<Device> Ptr;
};
typedef std::map<std::string, Device::Ptr> MapDevices;

class UpnpListener {
public:
  virtual void upnpSearchChangeWithResults(const MapDevices& devs) = 0;
  // 所有UPnPAction调用时都会返回一个唯一id, 可通过hook此方法来获取所有soap调用返回
  virtual void unppActionResponse(int id, int code, const std::map<std::string, std::string>& args) {}
};

class Upnp
{
  MapDevices _devices; //已经发现的设备
  hv::UdpClient _socket;
  std::shared_ptr<hv::AsyncHttpClient> _http_client;
  void onUdpRecv(char* buf, int size);

  void loadDeviceWithLocation(std::string loc, std::string usn);
  void addDevice(Device::Ptr device);
  void delDevice(const std::string& usn);
  void onChange();
  UpnpListener* _listener = nullptr;
  Upnp() = default;
public:
  void setListener(UpnpListener* l) { _listener = l; }
  UpnpListener* getListener() const {return _listener;}
  static Upnp* Instance();
  ~Upnp();

  hv::EventLoopPtr loop() { return _socket.loop(); }
  hv::AsyncHttpClient* httpClient() { return _http_client.get(); }
  void start();

  void startHttp();

  void stop();
  void search();

  MapDevices getDevices() { return _devices; }
  Device::Ptr getDevice(const std::string& usn);

  void setCurFile(const std::string& path) { _cur_file = path; }
  std::string getCurFile() const { return _cur_file; }
  std::string getCurUrl() const;

  void detectLocalIP();

private:
  std::string _cur_file;
  hv::HttpServer _http_server;
  hv::HttpService _http_service;
  std::string local_ip;
};

