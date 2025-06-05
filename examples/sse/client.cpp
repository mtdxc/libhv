#include "HttpClient.h"
//@see https://blog.csdn.net/CAir2/article/details/145759141
//@see https://cloud.tencent.com/developer/article/1999650
typedef std::function<void(const std::string& sid, const std::string& sevent, const std::string& sdata, const unsigned int retry_ms)> sse_msg_cb;
HV_INLINE int sse(http_method method, const char* url, const sse_msg_cb& msg_cb, 
    const http_body& body = NoBody, 
    const http_headers& headers = DefaultHeaders,
    const unsigned int timeout_s = -1) {
    hv::HttpClient cli;
    HttpRequest req;
    HttpResponse resp;

    req.url = url; //
    req.method = method;
    req.timeout = timeout_s; // 不超时
    if (&body != &NoBody) {
        req.body = body;
    }
    if (&headers != &DefaultHeaders) {
        req.headers = headers;
    }

    bool bstream = false;
    req.http_cb = [msg_cb, &bstream](HttpMessage* resp, http_parser_state state, const char* data, size_t size) {
        if (state == HP_HEADERS_COMPLETE) {
            if (resp->headers["Content-Type"] == "text/event-stream") {
                bstream = true;
                return 0;
            }
        }
        else if (state == HP_BODY) {
            /*binary body should check data*/
            // printf("%s", std::string(data, size).c_str());
            resp->body.append(data, size);
            if (!bstream) return 0;
            /*/n/n获取message*/
            size_t ifind = std::string::npos;
            while ((ifind = resp->body.find("\n\n")) != std::string::npos) {
                std::string msg = resp->body.substr(0, ifind + 2);
                resp->body.erase(0, ifind + 2);

                /*解析body,暂时不考虑多data
                id:xxx\n
                event:xxx\n
                data:xxx\n
                data:xxx\n
                data:xxx\n
                retry:10000\n
                */
                auto kvs = hv::splitKV(msg, '\n', ':');
                if (msg_cb && (kvs.count("id") || kvs.count("event") || kvs.count("data") || kvs.count("retry")))
                    msg_cb(kvs.count("id") ? kvs["id"] : "", kvs.count("event") ? kvs["event"] : "", kvs.count("data") ? kvs["data"] : "",
                           kvs.count("retry") ? atoi(kvs["retry"].c_str()) : 0);
            }
        }
        return 0;
    };
    return cli.send(&req, &resp);
}

using ollma_cb = std::function<bool(const hv::Json& j)>;
int ollama(const char* url, const char* prompt, const ollma_cb& msg_cb, const http_headers& headers = DefaultHeaders, const unsigned int timeout_s = -1) {
    HttpRequestPtr req = std::make_shared<HttpRequest>();
    req->url = url; //
    req->method = HTTP_POST;
    req->timeout = timeout_s; // 不超时
    if (&headers != &DefaultHeaders) {
        req->headers = headers;
    }
    req->SetHeader("Content-Type", "application/json");
    req->Set("stream", true);
    req->Set("model", "llama3:8b");
    req->Set("prompt", prompt);
    bool bstream = false;
    req->http_cb = [msg_cb, req, &bstream](HttpMessage* resp, http_parser_state state, const char* data, size_t size) {
        if (state == HP_HEADERS_COMPLETE) {
            if (resp->headers["Content-Type"] == "text/event-stream") {
                bstream = true;
            }
        }
        else if (state == HP_BODY) {
            /*binary body should check data*/
            // printf("%s", std::string(data, size).c_str());
            resp->body.append(data, size);
            if (!bstream) {
                size_t ifind = std::string::npos;
                while ((ifind = resp->body.find("\n")) != std::string::npos) {
                    std::string msg = resp->body.substr(0, ifind + 1);
                    hv::Json j;
                    try {
                        j = hv::Json::parse(msg);
                        if (!msg_cb(j)) {
                            req->Cancel();
                        }
                        if (j["context"]) {
                            req->Set("context", j["context"]);
                        }
                    } catch (const std::exception& e) {
                        //fprintf(stderr, "JSON parse error: %s\n", e.what());
                    }
                    resp->body.erase(0, ifind + 1);
                }
            }
            else {
                /*/n/n获取message*/
                size_t ifind = std::string::npos;
                while ((ifind = resp->body.find("\n\n")) != std::string::npos) {
                    std::string msg = resp->body.substr(0, ifind + 2);
                    resp->body.erase(0, ifind + 2);

                    /*解析body,暂时不考虑多data
                    id:xxx\n
                    event:xxx\n
                    data:xxx\n
                    data:xxx\n
                    data:xxx\n
                    retry:10000\n
                    */
                    auto kvs = hv::splitKV(msg, '\n', ':');
                    if (!msg_cb(hv::Json(kvs))) {
                        req->Cancel();
                    }
                }
            }
        }
    };
    hv::HttpClient cli;
    HttpResponsePtr resp = std::make_shared<HttpResponse>();
    return cli.send(req.get(), resp.get());
}

int main(int argc, char** argv) {
    ollama("http://localhost:11434/api/generate", "hello world", [](hv::Json val) {
        printf("ollama: %s\n", val.dump().c_str());
        return true;
    });
    return 0;

    if (argc < 2) {
        printf("Usage: %s url\n", argv[0]);
        return -1;
    }

    const char* url = argv[1];
    sse_msg_cb msg_cb = [](const std::string& sid, const std::string& sevent, const std::string& sdata, const unsigned int retry_ms) {
        printf("sid: %s, event: %s, data: %s, retry: %u\n", sid.c_str(), sevent.c_str(), sdata.c_str(), retry_ms);
        return true;
    };

    int ret = sse(HTTP_GET, url, msg_cb);
    if (ret != 0) {
        fprintf(stderr, "sse failed: %d\n", ret);
        return -1;
    }

    return 0;
}   
