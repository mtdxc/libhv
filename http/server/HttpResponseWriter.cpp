#include "HttpResponseWriter.h"

namespace hv {

int HttpResponseWriter::EndHeaders(const char* key /* = NULL */, const char* value /* = NULL */) {
    if (state != SEND_BEGIN) return -1;
    if (key && value) {
        response->SetHeader(key, value);
    }
    std::string headers = response->Dump(true, false);
    // erase Content-Length: 0\r\n
    std::string content_length_0("Content-Length: 0\r\n");
    auto pos = headers.find(content_length_0);
    if (pos != std::string::npos) {
        headers.erase(pos, content_length_0.size());
    }
    state = SEND_HEADER;
    return write(headers);
}

int HttpResponseWriter::WriteChunked(const char* buf, int len /* = -1 */) {
    int ret = 0;
    if (len == -1) len = strlen(buf);
    if (state == SEND_BEGIN) {
        EndHeaders("Transfer-Encoding", "chunked");
    }
    char chunked_header[64];
    int chunked_header_len = snprintf(chunked_header, sizeof(chunked_header), "%x\r\n", len);
    write(chunked_header, chunked_header_len);
    if (buf && len) {
        state = SEND_CHUNKED;
        ret = write(buf, len);
    } else {
        state = SEND_CHUNKED_END;
    }
    write("\r\n", 2);
    return ret;
}

int HttpResponseWriter::WriteBody(const char* buf, int len /* = -1 */) {
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

int HttpResponseWriter::WriteResponse(HttpResponse* resp) {
    if (resp == NULL) {
        response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        return 0;
    }
    bool is_dump_headers = state == SEND_BEGIN ? true : false;
    std::string msg = resp->Dump(is_dump_headers, true);
    state = SEND_BODY;
    return write(msg);
}

int HttpResponseWriter::SSEvent(const std::string& data, const char* event /* = "message" */) {
    if (state == SEND_BEGIN) {
        EndHeaders("Content-Type", "text/event-stream");
    }
    std::string msg;
    if (event) {
        msg = "event: "; msg += event; msg += "\n";
    }
    msg += "data: ";  msg += data;  msg += "\n\n";
    state = SEND_BODY;
    return write(msg);
}

int HttpResponseWriter::End(const char* buf /* = NULL */, int len /* = -1 */) {
    if (end == SEND_END) return 0;
    end = SEND_END;

    if (!isConnected()) {
        return -1;
    }

    int ret = 0;
    bool keepAlive = response->IsKeepAlive();
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
        bool is_dump_headers = true;
        bool is_dump_body = true;
        if (state == SEND_HEADER) {
            is_dump_headers = false;
        } else if (state == SEND_BODY) {
            is_dump_headers = false;
            is_dump_body = false;
        }
        if (is_dump_body) {
            std::string msg = response->Dump(is_dump_headers, is_dump_body);
            state = SEND_BODY;
            ret = write(msg);
        }
    }

    if (!keepAlive) {
        close(true);
    }
    return ret;
}

int HttpResponseWriter::ResponseFile(const char* path, HttpRequest* req, int speed) {
    endFile();
    if (file.open(path, "rb") != 0) {
        return HTTP_STATUS_NOT_FOUND;
    }
    long total = file.size();
    // fill content-type by ext
    response->SetContentTypeByFilename(path);
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

    if (response->content_length < (1 << 21)) {
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
            this->onwrite = [this](Buffer*) {
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

void HttpResponseWriter::endFile() {
    file.close();
    wait_writable = false;
    onwrite = NULL;
    setHeartbeat(0, NULL);
}

void HttpResponseWriter::writeFile(int maxSize) {
    if (!file.isopen()) return;

    char buf[4096];
    while (response->content_length && maxSize) {
        int len = sizeof(buf);
        if (len > response->content_length) len = response->content_length;
        if (maxSize > 0 && len > maxSize) len = maxSize;

        len = file.read(buf, len);
        if (!len) {
            // file reach end
            close(true);
            break;
        }

        int n = write(buf, len);
        if (n < 0) {
            // network write error
            endFile();
            break;
        }

        response->content_length -= len;
        if (maxSize > 0) {
            maxSize -= len;
            if (writeBufsize()) return;
        }
        else if (n != len) // writeBufsize())
        {                  // no limmit speed wait for next writeable event
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

}
