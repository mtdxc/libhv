#ifndef HV_HTTP_RESPONSE_WRITER_H_
#define HV_HTTP_RESPONSE_WRITER_H_

#include "Channel.h"
#include "HttpMessage.h"
#include "hfile.h"
namespace hv {

class HV_EXPORT HttpResponseWriter : public SocketChannel {
public:
    HttpResponsePtr response;
    enum State {
        SEND_BEGIN, // 初始化，什么都没写
        SEND_HEADER,// 已写出头部
        SEND_BODY,  // 已写入部分body，可调用多次 WriteBody
        // content-type=chunked
        SEND_CHUNKED, // 写入部分chunk，可调用多次 WriteChunked
        SEND_CHUNKED_END, // 写入最后一个chunk
        SEND_END, // 已写完，调用End后，如不支持keepalive，则关闭连接
    } state: 8, end: 8;
    // HTTP/2: set by HttpHandler when the connection is h2. The raw HTTP/1
    // serialization in this writer cannot produce h2 frames, so on h2 the
    // one-shot response (End / WriteResponse) is funneled through this hook,
    // which submits the response via the handler's nghttp2 session on the IO
    // loop thread. Streaming (WriteChunked/SSE) over h2 is not supported.
    std::function<void()> submitHttp2Response;

    HttpResponseWriter(hio_t* io, const HttpResponsePtr& resp)
        : SocketChannel(io)
        , response(resp)
        , state(SEND_BEGIN)
        , end(SEND_BEGIN)
    {}
    ~HttpResponseWriter() { endFile(); }

    bool isHttp2() const { return (bool)submitHttp2Response; }

    // isBegin(): nothing has been written yet (still at SEND_BEGIN).
    // isEnd():   the response has been fully handed off, i.e. End() was called
    //            (End() is the terminal call in every usage sequence below).
    bool isBegin() const { return state == SEND_BEGIN; }
    bool isEnd() const { return end == SEND_END; }

    // Begin -> End
    // Begin -> WriteResponse -> End
    // Begin -> WriteStatus -> WriteHeader -> WriteBody -> End
    // Begin -> EndHeaders("Content-Type", "text/event-stream") -> write -> write -> ... -> close
    // Begin -> EndHeaders("Content-Length", content_length) -> WriteBody -> WriteBody -> ... -> End
    // Begin -> EndHeaders("Transfer-Encoding", "chunked") -> WriteChunked -> WriteChunked -> ... -> End

    int Begin() {
        state = end = SEND_BEGIN;
        endFile();
        return 0;
    }

    int WriteStatus(http_status status_codes) {
        response->status_code = status_codes;
        return 0;
    }

    int WriteHeader(const char* key, const char* value) {
        response->SetHeader(key, value);
        return 0;
    }

    template<typename T>
    int WriteHeader(const char* key, T num) {
        response->SetHeader(key, hv::to_string(num));
        return 0;
    }

    int WriteCookie(const HttpCookie& cookie) {
        response->cookies.push_back(cookie);
        return 0;
    }

    int EndHeaders(const char* key = NULL, const char* value = NULL);

    template<typename T>
    int EndHeaders(const char* key, T num) {
        std::string value = hv::to_string(num);
        return EndHeaders(key, value.c_str());
    }

    int WriteChunked(const char* buf, int len = -1);

    int WriteChunked(const std::string& str) {
        return WriteChunked(str.c_str(), str.size());
    }

    int EndChunked() {
        return WriteChunked(NULL, 0);
    }

    int WriteBody(const char* buf, int len = -1);

    int WriteBody(const std::string& str) {
        return WriteBody(str.c_str(), str.size());
    }

    int WriteResponse(HttpResponse* resp);

    int SSEvent(const std::string& data, const char* event = "message");

    int End(const char* buf = NULL, int len = -1);

    int End(const std::string& str) {
        return End(str.c_str(), str.size());
    }

    int ResponseFile(const char* path, HttpRequest* req, int speed);

private:
    HFile file;
    bool wait_writable;
    void endFile();

    void writeFile(int maxSize);

};

}

typedef std::shared_ptr<hv::HttpResponseWriter> HttpResponseWriterPtr;

#endif // HV_HTTP_RESPONSE_WRITER_H_
