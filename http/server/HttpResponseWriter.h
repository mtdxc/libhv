#ifndef HV_HTTP_RESPONSE_WRITER_H_
#define HV_HTTP_RESPONSE_WRITER_H_

#include "Channel.h"
#include "HttpMessage.h"
#include "hfile.h"
namespace hv {

class HttpResponseWriter : public SocketChannel {
public:
    HttpResponsePtr response;
    enum State {
        SEND_BEGIN,
        SEND_HEADER,
        SEND_BODY,
        SEND_CHUNKED,
        SEND_CHUNKED_END,
        SEND_END,
    } state;
    HttpResponseWriter(hio_t* io, const HttpResponsePtr& resp)
        : SocketChannel(io)
        , response(resp)
        , state(SEND_BEGIN)
    {}
    ~HttpResponseWriter() { endFile(); }

    // Begin -> End
    // Begin -> WriteResponse -> End
    // Begin -> WriteStatus -> WriteHeader -> WriteBody -> End
    // Begin -> EndHeaders("Content-Type", "text/event-stream") -> write -> write -> ... -> close
    // Begin -> EndHeaders("Content-Length", content_length) -> WriteBody -> WriteBody -> ... -> End
    // Begin -> EndHeaders("Transfer-Encoding", "chunked") -> WriteChunked -> WriteChunked -> ... -> End

    int Begin() {
        state = SEND_BEGIN;
        endFile();
        return 0;
    }

    int WriteStatus(http_status status_codes) {
        response->status_code = status_codes;
        return 0;
    }

    int WriteHeader(const char* key, const char* value) {
        response->headers[key] = value;
        return 0;
    }

    template<typename T>
    int WriteHeader(const char* key, T num) {
        response->headers[key] = hv::to_string(num);
        return 0;
    }

    int EndHeaders(const char* key = NULL, const char* value = NULL) {
        if (state != SEND_BEGIN) return -1;
        if (key && value) {
            response->headers[key] = value;
        }
        std::string headers = response->Dump(true, false);
        state = SEND_HEADER;
        return write(headers);
    }

    template<typename T>
    int EndHeaders(const char* key, T num) {
        std::string value = hv::to_string(num);
        return EndHeaders(key, value.c_str());
    }

    int WriteChunked(const char* buf, int len = -1) {
        int ret = 0;
        if (len == -1) len = strlen(buf);
        if (state == SEND_BEGIN) {
            EndHeaders("Transfer-Encoding", "chunked");
        }
        char chunked_header[64];
        int chunked_header_len = snprintf(chunked_header, sizeof(chunked_header), "%x\r\n", len);
        write(chunked_header, chunked_header_len);
        if (buf && len) {
            ret = write(buf, len);
            state = SEND_CHUNKED;
        } else {
            state = SEND_CHUNKED_END;
        }
        write("\r\n", 2);
        return ret;
    }

    int WriteChunked(const std::string& str) {
        return WriteChunked(str.c_str(), str.size());
    }

    int EndChunked() {
        return WriteChunked(NULL, 0);
    }

    int WriteBody(const char* buf, int len = -1) {
        if (response->IsChunked()) {
            return WriteChunked(buf, len);
        }

        if (len == -1) len = strlen(buf);
        if (state == SEND_BEGIN) {
            response->body.append(buf, len);
            return len;
        } else {
            state = SEND_BODY;
            return write(buf, len);
        }
    }

    int WriteBody(const std::string& str) {
        return WriteBody(str.c_str(), str.size());
    }

    int WriteResponse(HttpResponse* resp) {
        if (resp == NULL) {
            response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
            return 0;
        }
        bool is_dump_headers = (state == SEND_BEGIN);
        std::string msg = resp->Dump(is_dump_headers, true);
        state = SEND_BODY;
        return write(msg);
    }

    int End(const char* buf = NULL, int len = -1) {
        if (state == SEND_END) return 0;
        if (!isConnected()) {
            state = SEND_END;
            return -1;
        }

        int ret = 0;
        if (state == SEND_CHUNKED) {
            if (buf) {
                ret = WriteChunked(buf, len);
            }
            if (state == SEND_CHUNKED) {
                EndChunked();
            }
        } else {
            if (buf) {
                ret = WriteBody(buf, len);
            }
            bool is_dump_headers = (state == SEND_BEGIN);
            bool is_dump_body = (state != SEND_BODY);
            if (is_dump_body) {
                std::string msg = response->Dump(is_dump_headers, is_dump_body);
                ret = write(msg);
            }
        }

        state = SEND_END;
        if (!response->IsKeepAlive()) {
            close(true);
        }
        return ret;
    }

    int End(const std::string& str) {
        return End(str.c_str(), str.size());
    }

    int ResponseFile(const char* path, HttpRequest* req, int speed) {
        endFile();
        if (file.open(path, "rb") != 0) {
            return HTTP_STATUS_NOT_FOUND;
        }
        long total = file.size();
        // fill content-type by ext
        response->File(path, false);
        // Range:
        long from = 0, to = 0;
        if (req && req->GetRange(from, to)) {
            if (to == 0 || to >= total) to = total - 1;
            response->content_length = to - from + 1;
            // resp range
            response->SetRange(from, to, total);
            // response 206
            response->status_code = HTTP_STATUS_PARTIAL_CONTENT;
            // seek to from
            file.seek(from);
        }
        else {
            response->status_code = HTTP_STATUS_OK;
            response->content_length = total;
            to = total - 1;
        }

        if (response->content_length < (1<<21)) {
            // range with memory body
            int nread = file.readrange(response->body, from, to);
            file.close();
            if (nread != response->content_length) {
                response->content_length = 0;
                response->Reset();
                return HTTP_STATUS_INTERNAL_SERVER_ERROR;
            }
            return response->status_code;
        }
        else { // range with file cache
            // response header
            EndHeaders();
            state = SEND_BODY;
            if (speed > 0) {
                int send_ms = 1000;
                speed *= send_ms;
                // use heartbeat to send data every 1s
                setHeartbeat(send_ms, std::bind(&HttpResponseWriter::writeFile, this, speed));
            }
            else {
                speed = -1;
                this->onwrite = [this](Buffer*){
                    if (file.isopen() && wait_writable && isWriteComplete()) {
                        wait_writable = false;
                        writeFile(-1);
                    }
                };
            }
            writeFile(speed);
            return 0;
        }
    }

private:
    HFile file;
    bool wait_writable;
    void endFile(){
        file.close();
        wait_writable = false;
        onwrite = NULL;
        setHeartbeat(0, NULL);
    }

    void writeFile(int maxSize) {
        if (!file.isopen()) return ;

        char buf[4096];
        while (response->content_length && maxSize)
        {
            int len = sizeof(buf);
            if (len > response->content_length)
                len = response->content_length;
            if (maxSize > 0 && len > maxSize)
                len = maxSize;

            len = file.read(buf, len);
            if (!len) {
                // file reach end
                close(true);
                break;
            }

            int n = write(buf, len);
            if (n<0) {
                // network write error
                endFile();
                break;
            }

            response->content_length -= len;
            if (maxSize>0) {
                maxSize -= len;
                if (writeBufsize())
                    return ;
            }
            else if (n != len) // writeBufsize())
            { // no limmit speed wait for next writeable event
                wait_writable = true;
                return;
            }
        }

        if (!response->content_length) {
            endFile();
            state = SEND_END;
            if (!response->IsKeepAlive()) {
                close(true);
            }
        }
    }

};

}

typedef std::shared_ptr<hv::HttpResponseWriter> HttpResponseWriterPtr;

#endif // HV_HTTP_RESPONSE_WRITER_H_
