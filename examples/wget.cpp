/*
 * @build: make examples
 * @server bin/httpd -s restart -d
 * @client bin/wget http://127.0.0.1:8080/
 */

#include "HttpClient.h"
#include "htime.h"
using namespace hv;

typedef std::function<void(size_t received_bytes, size_t total_bytes)> wget_progress_cb;

static int wget(const char* url, const char* filepath, int range_bytes = 0, wget_progress_cb progress_cb = NULL) {
    int ret = 0;
    HttpClient cli;
    HttpRequest req;
    HttpResponse resp;

    // HEAD
    req.method = HTTP_HEAD;
    req.url = url;
    ret = cli.send(&req, &resp);
    if (ret != 0) {
        fprintf(stderr, "HEAD request error: %d\n", ret);
        return ret;
    }
    printd("%s", resp.Dump(true, false).c_str());
    if (resp.status_code == HTTP_STATUS_NOT_FOUND) {
        fprintf(stderr, "404 Not Found\n");
        return 404;
    }

    // use Range?
    bool use_range = false;
    long from = 0, to = 0;
    size_t content_length = hv::from_string<size_t>(resp.GetHeader("Content-Length"));
    if (range_bytes) {
        std::string accept_ranges = resp.GetHeader("Accept-Ranges");
        // use Range if server accept_ranges and content_length > 1M
        if (resp.status_code == 200 &&
            accept_ranges == "bytes" &&
            content_length > range_bytes) {
            printf("wget use_range range_bytes=%d content_length=%d\n", range_bytes, content_length);
            use_range = true;
        }
    }

    // open file
    std::string filepath_download(filepath);
    filepath_download += ".download";
    HFile file;
    if (use_range) {
        ret = file.open(filepath_download.c_str(), "ab");
        from = file.size();
    } else {
        ret = file.open(filepath_download.c_str(), "wb");
    }
    if (ret != 0) {
        fprintf(stderr, "Failed to open file %s\n", filepath_download.c_str());
        return ret;
    }
    printf("Save file to %s ...\n", filepath);

    // GET
    req.method = HTTP_GET;
    req.timeout = 3600; // 1h
    if (!use_range) {
        size_t received_bytes = 0;
        req.http_cb = [&file, &content_length, &received_bytes, &progress_cb]
            (HttpMessage* resp, http_parser_state state, const char* data, size_t size) {
            if (!resp->headers["Location"].empty()) return;
            if (state == HP_HEADERS_COMPLETE) {
                content_length = hv::from_string<size_t>(resp->GetHeader("Content-Length"));
                printd("%s", resp->Dump(true, false).c_str());
            } else if (state == HP_BODY) {
                if (data && size) {
                    file.write(data, size);
                    received_bytes += size;

                    if (progress_cb) {
                        progress_cb(received_bytes, content_length);
                    }
                }
            }
        };
        ret = cli.send(&req, &resp);
        if (ret != 0) {
            fprintf(stderr, "GET request error: %d\n", ret);
            goto error;
        }
        goto success;
    }

    // Range: bytes=from-to
    while (from < content_length) {
        to = from + range_bytes - 1;
        if (to >= content_length) to = content_length - 1;
        req.SetRange(from, to);
        printd("%s", req.Dump(true, false).c_str());
        ret = cli.send(&req, &resp);
        if (ret != 0) {
            fprintf(stderr, "GET Range: bytes=%ld-%ld request error: %d\n", from, to, ret);
            goto error;
        }
        printd("%s", resp.Dump(true, false).c_str());
        file.write(resp.body.data(), resp.body.size());
        // fix: resp.body.size != range_bytes on some server
        // from = to + 1;
        from += resp.body.size();

        if (progress_cb) {
            progress_cb(from, content_length);
        }
    }

success:
    file.close();
    ret = file.rename(filepath);
    if (ret != 0) {
        fprintf(stderr, "mv %s => %s failed: %s:%d\n", filepath_download.c_str(), filepath, strerror(ret), ret);
    }
    return ret;
error:
    file.close();
    // file.remove();
    return ret;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s [--range 1024] url [filepath]\n", argv[0]);
        return -10;
    }
    int idx = 1;
    int range_bytes = 0;
    if (strcmp(argv[idx], "--range") == 0) {
        ++idx;
        range_bytes = 1024 * atoi(argv[idx]);
        ++idx;
    }
    const char* url = argv[idx++];
    const char* filepath = "index.html";
    if (argv[idx]) {
        filepath = argv[idx];
    } else {
        const char* path = strrchr(url, '/');
        if (path && path[1]) {
            filepath = path + 1;
        }
    }

    unsigned int start_time = gettick_ms();
    int last_progress = 0;
    wget(url, filepath, range_bytes, [&last_progress](size_t received_bytes, size_t total_bytes) {
        // print progress
        if (total_bytes == 0) {
            printf("\rprogress: %lu/? = ?", (unsigned long)received_bytes);
        } else {
            int cur_progress = received_bytes * 100 / total_bytes;
            if (cur_progress > last_progress) {
                printf("\rprogress: %lu/%lu = %d%%", (unsigned long)received_bytes, (unsigned long)total_bytes, (int)cur_progress);
                last_progress = cur_progress;
            }
        }
        fflush(stdout);
    });
    unsigned int end_time = gettick_ms();
    unsigned int cost_time = end_time - start_time;
    printf("\ncost time %u ms\n", cost_time);
    // 1B/ms = 1KB/s = 8Kbps
    printf("download rate = %lu KB/s\n", (unsigned long)hv_filesize(filepath) / cost_time);

    return 0;
}
