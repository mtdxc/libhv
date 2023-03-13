#include "HttpClient.h"
#include "hbase.h"
using namespace hv;

int main(int argc, char** argv) {
    HttpClient cli;
    HttpRequest req;
    HttpResponse resp;
    const char* filename = "logo.png";
    if (argc > 1) {
        filename = argv[1];
    }
    req.SetUrl("https://replayvar.com/internal/asset/uploadSignature");
    req.SetHeader("X-Auth-Token", "123123");
    req.SetHeader("stadiumId", "2");
    req.SetParam("dir", "avatar");
    req.SetParam("filename", hv_basename(filename));
    int ret = cli.send(&req, &resp);
    std::cout << "uploadSignature return " << ret << ": " << resp.Body() << std::endl;

    std::string url = resp.GetString("endpoint");
    if (std::string::npos == url.find("://"))
        url = "http://" + url;
    std::cout << "begin upload with url " << url << ", file=" << filename << std::endl;
    HttpRequest upl;
    upl.SetMethod("POST");
    upl.SetUrl(url.c_str());
    upl.SetFormData("OSSAccessKeyId", resp.GetString("accessKeyId"));
    upl.SetFormData("policy", resp.GetString("policy"));
    upl.SetFormData("Signature", resp.GetString("signature"));
    upl.SetFormData("key", resp.GetString("key"));
    upl.SetFormData("success_action_status", "201");
    upl.SetFormFile("file", filename);
    upl.SetTimeout(600);
    resp.Reset();
    // close old http
    cli.close();
    ret = cli.send(&upl, &resp);
    std::cout << "upload return " << ret << ": " << resp.Body() << std::endl;    
}