﻿#include "HttpHandler.h"

#include "hbase.h"
#include "herr.h"
#include "hlog.h"
#include "hasync.h" // import hv::async for http_async_handler
#include "http_page.h"

#include "htime.h"
bool HttpHandler::SwitchWebSocket(hio_t* io, ws_session_type type) {
    if(!io || !ws_service) return false;
    protocol = WEBSOCKET;
    ws_parser.reset(new WebSocketParser);
    ws_channel.reset(new hv::WebSocketChannel(io, type));
    ws_parser->onMessage = [this](int opcode, const std::string& msg){
        switch(opcode) {
        case WS_OPCODE_CLOSE:
            ws_channel->close(true);
            break;
        case WS_OPCODE_PING:
            // printf("recv ping\n");
            // printf("send pong\n");
            ws_channel->sendPong();
            break;
        case WS_OPCODE_PONG:
            // printf("recv pong\n");
            this->last_recv_pong_time = gethrtime_us();
            break;
        case WS_OPCODE_TEXT:
        case WS_OPCODE_BINARY:
            // onmessage
            if (ws_service && ws_service->onmessage) {
                ws_service->onmessage(ws_channel, msg);
            }
            break;
        default:
            break;
        }
    };
    // NOTE: cancel keepalive timer, judge alive by heartbeat.
    hio_set_keepalive_timeout(io, 0);
    if (ws_service && ws_service->ping_interval > 0) {
        int ping_interval = MAX(ws_service->ping_interval, 1000);
        ws_channel->setHeartbeat(ping_interval, [this](){
            if (last_recv_pong_time < last_send_ping_time) {
                hlogw("[%s:%d] websocket no pong!", ip, port);
                ws_channel->close(true);
            } else {
                // printf("send ping\n");
                ws_channel->sendPing();
                last_send_ping_time = gethrtime_us();
            }
        });
    }
    // onopen
    WebSocketOnOpen();
    return true;
}

int HttpHandler::customHttpHandler(const http_handler& handler) {
    return invokeHttpHandler(&handler);
}

int HttpHandler::invokeHttpHandler(const http_handler* handler) {
    int status_code = HTTP_STATUS_NOT_IMPLEMENTED;
    if (handler->sync_handler) {
        // NOTE: sync_handler run on IO thread
        status_code = handler->sync_handler(req.get(), resp.get());
    } else if (handler->async_handler) {
        // NOTE: async_handler run on hv::async threadpool
        hv::async(std::bind(handler->async_handler, req, writer));
        status_code = HTTP_STATUS_UNFINISHED;
    } else if (handler->ctx_handler) {
        HttpContextPtr ctx(new hv::HttpContext);
        ctx->service = service;
        ctx->request = req;
        ctx->response = resp;
        ctx->writer = writer;
        // NOTE: ctx_handler run on IO thread, you can easily post HttpContextPtr to your consumer thread for processing.
        status_code = handler->ctx_handler(ctx);
        if (writer && writer->state != hv::HttpResponseWriter::SEND_BEGIN) {
            status_code = HTTP_STATUS_UNFINISHED;
        }
    }
    return status_code;
}

int HttpHandler::HandleHttpRequest() {
    // preprocessor -> processor -> postprocessor
    int status_code = HTTP_STATUS_OK;
    HttpRequest* pReq = req.get();
    HttpResponse* pResp = resp.get();

    pReq->scheme = ssl ? "https" : "http";
    pReq->client_addr.ip = ip;
    pReq->client_addr.port = port;
    pReq->Host();
    pReq->ParseUrl();
    // NOTE: Not all users want to parse body, we comment it out.
    // pReq->ParseBody();

preprocessor:
    state = HANDLE_BEGIN;
    if (service->preprocessor) {
        status_code = customHttpHandler(service->preprocessor);
        if (status_code != 0) {
            goto postprocessor;
        }
    }

processor:
    if (service->processor) {
        status_code = customHttpHandler(service->processor);
    } else {
        status_code = defaultRequestHandler();
    }

postprocessor:
    if (status_code >= 100 && status_code < 600) {
        pResp->status_code = (http_status)status_code;
    }
    if (pResp->status_code >= 400 && pResp->body.size() == 0 && pReq->method != HTTP_HEAD) {
        if (service->errorHandler) {
            customHttpHandler(service->errorHandler);
        } else {
            defaultErrorHandler();
        }
    }
    if (fc) {
        pResp->content = fc->filebuf.base;
        pResp->content_length = fc->filebuf.len;
        pResp->headers["Content-Type"] = fc->content_type;
        pResp->headers["Last-Modified"] = fc->last_modified;
        pResp->headers["Etag"] = fc->etag;
    }
    if (service->postprocessor) {
        customHttpHandler(service->postprocessor);
    }

    if (status_code == 0) {
        state = HANDLE_CONTINUE;
    } else {
        state = HANDLE_END;
        parser->SubmitResponse(resp.get());
    }
    return status_code;
}

int HttpHandler::defaultRequestHandler() {
    int status_code = HTTP_STATUS_OK;
    http_handler* handler = NULL;

    if (service->api_handlers.size() != 0) {
        service->GetApi(req.get(), &handler);
    }

    if (handler) {
        status_code = invokeHttpHandler(handler);
    }
    else if (req->method == HTTP_GET || req->method == HTTP_HEAD) {
        // static handler
        if (service->staticHandler) {
            status_code = customHttpHandler(service->staticHandler);
        }
        else if (service->document_root.size() != 0) {
            status_code = defaultStaticHandler();
        }
        else {
            status_code = HTTP_STATUS_NOT_FOUND;
        }
    }
    else {
        // Not Implemented
        status_code = HTTP_STATUS_NOT_IMPLEMENTED;
    }

    return status_code;
}

int HttpHandler::defaultStaticHandler() {
    // file service
    int status_code = HTTP_STATUS_OK;
    std::string path = req->Path();
    const char* req_path = path.c_str();
    // path safe check
    if (req_path[0] != '/' || strstr(req_path, "/../")) {
        return HTTP_STATUS_BAD_REQUEST;
    }
    std::string filepath = service->document_root + path;
    if (req_path[1] == '\0') {
        filepath += service->home_page;
    }
    bool is_dir = filepath[filepath.size()-1] == '/';
    bool is_index_of = false;
    if (service->index_of.size() != 0 && hv_strstartswith(req_path, service->index_of.c_str())) {
        is_index_of = true;
    }
    if (is_dir && !is_index_of) { // unsupport dir without index
        return HTTP_STATUS_NOT_FOUND;
    }

    FileCache::OpenParam param;
	bool has_range = req->headers.find("Range") != req->headers.end();
    if (has_range) {
        if (service->largeFileHandler) 
            status_code = customHttpHandler(service->largeFileHandler);
        else
            status_code = writer->ResponseFile(filepath.c_str(), req.get(), service->limit_rate);
        return status_code;
    }

    param.need_read = req->method != HTTP_HEAD;
    param.path = req_path;
    fc = files->Open(filepath.c_str(), &param);
    if (fc == NULL) {
        status_code = HTTP_STATUS_NOT_FOUND;
        if (param.error == ERR_OVER_LIMIT) {
            if (service->largeFileHandler) 
                status_code = customHttpHandler(service->largeFileHandler);
            else
                status_code = writer->ResponseFile(filepath.c_str(), NULL, service->limit_rate);
        }
    }
    else {
        // Not Modified
        auto iter = req->headers.find("if-not-match");
        if (iter != req->headers.end() &&
            strcmp(iter->second.c_str(), fc->etag) == 0) {
            status_code = HTTP_STATUS_NOT_MODIFIED;
            fc = NULL;
        }
        else {
            iter = req->headers.find("if-modified-since");
            if (iter != req->headers.end() &&
                strcmp(iter->second.c_str(), fc->last_modified) == 0) {
                status_code = HTTP_STATUS_NOT_MODIFIED;
                fc = NULL;
            }
        }
    }
    return status_code;
}

int HttpHandler::defaultErrorHandler() {
    // error page
    if (service->error_page.size() != 0) {
        std::string filepath = service->document_root + '/' + service->error_page;
        FileCache::OpenParam param;
        // load error page from file cache..
        fc = files->Open(filepath.c_str(), &param);
    }
    // status page
    if (fc == NULL && resp->body.size() == 0) {
        resp->content_type = TEXT_HTML;
        make_http_status_page(resp->status_code, resp->body);
    }
    return 0;
}

int HttpHandler::FeedRecvData(const char* data, size_t len) {
    int nfeed = 0;
    if (protocol == HttpHandler::WEBSOCKET) {
        nfeed = ws_parser->FeedRecvData(data, len);
        if (nfeed != len) {
            hloge("[%s:%d] websocket parse error!", ip, port);
        }
    } else {
        if (state != WANT_RECV) {
            Reset();
        }
        nfeed = parser->FeedRecvData(data, len);
        if (nfeed != len) {
            hloge("[%s:%d] http parse error: %s", ip, port, parser->StrError(parser->GetError()));
        }
    }
    return nfeed;
}

int HttpHandler::GetSendData(char** data, size_t* len) {
    if (state == HANDLE_CONTINUE) {
        return 0;
    }

    HttpRequest* pReq = req.get();
    HttpResponse* pResp = resp.get();

    if (protocol == HTTP_V1) {
        switch(state) {
        case WANT_RECV:
            if (parser->IsComplete()) state = WANT_SEND;
            else return 0;
        case HANDLE_END:
             state = WANT_SEND;
        case WANT_SEND:
            state = SEND_HEADER;
        case SEND_HEADER:
        {
            // HEAD
            if (pReq->method == HTTP_HEAD) {
                if (fc) {
                    pResp->headers["Accept-Ranges"] = "bytes";
                    pResp->headers["Content-Length"] = hv::to_string(fc->st.st_size);
                } else {
                    pResp->headers["Content-Type"] = "text/html";
                    pResp->headers["Content-Length"] = "0";
                }
                state = SEND_DONE;
                pResp->content_length = 0;
                goto return_header;
            }
            // File service
            if (fc) {
                // FileCache
                // NOTE: no copy filebuf, more efficient
                header = pResp->Dump(true, false);
                fc->prepend_header(header.c_str(), header.size());
                *data = fc->httpbuf.base;
                *len = fc->httpbuf.len;
                state = SEND_DONE;
                return *len;
            }
            // API service
            if (const char* content = (const char*)pResp->Content()) {
                int content_length = pResp->ContentLength();
                if (content_length > (1 << 20)) {
                    state = SEND_BODY;
                } else {
                    // NOTE: header+body in one package if <= 1M
                    header = pResp->Dump(true, false);
                    header.append(content, content_length);
                    state = SEND_DONE;
                }
            } else {
                state = SEND_DONE;
            }
return_header:
            if (header.empty()) header = pResp->Dump(true, false);
            *data = (char*)header.c_str();
            *len = header.size();
            return *len;
        }
        case SEND_BODY:
        {
            if (body.empty()) {
                *data = (char*)pResp->Content();
                *len = pResp->ContentLength();
            } else {
                *data = (char*)body.c_str();
                *len = body.size();
            }
            state = SEND_DONE;
            return *len;
        }
        case SEND_DONE:
        {
            // NOTE: remove file cache if > 16M
            if (fc && fc->filebuf.len > FILE_CACHE_MAX_SIZE) {
                files->Close(fc);
            }
            fc = NULL;
            header.clear();
            body.clear();
            return 0;
        }
        default:
            return 0;
        }
    } else if (protocol == HTTP_V2) {
        return parser->GetSendData(data, len);
    }
    return 0;
}
