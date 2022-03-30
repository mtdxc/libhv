#ifndef HV_FILE_CACHE_H_
#define HV_FILE_CACHE_H_

#include <memory>
#include <map>
#include <string>
#include <mutex>

#include "hbuf.h"
#include "hstring.h"

#define HTTP_HEADER_MAX_LENGTH      1024        // 1K
#define FILE_CACHE_MAX_SIZE         (1 << 22)   // 4M
// 文件或目录列表缓存
typedef struct file_cache_s {
    std::string filepath;
    struct stat st;
    time_t      open_time;
    // 最后访问时间
    time_t      stat_time;
    /*
    buf(owner): |<HTTP_HEADER_MAX_LENGTH|                              >|
    filebuf:                            |<          filebuf            >|
    httpbuf:                 |< http hdr|           filebuf            >|
    */
    HBuf        buf; // http_header + file_content
    hbuf_t      filebuf;
    hbuf_t      httpbuf;

    char        last_modified[64];
    char        etag[64];
    std::string content_type;

    file_cache_s() {
        last_modified[0] = etag[0] = 0;
    }

    bool is_modified() {
        // 判断前后两次的修改时间是否相同
        time_t mtime = st.st_mtime;
        stat(filepath.c_str(), &st);
        return mtime != st.st_mtime;
    }

    bool is_complete() {
        if(S_ISDIR(st.st_mode)) return filebuf.len > 0;
        return filebuf.len == st.st_size;
    }

    void resize_buf(int filesize) {
        buf.resize(HTTP_HEADER_MAX_LENGTH + filesize);
        filebuf.base = buf.base + HTTP_HEADER_MAX_LENGTH;
        filebuf.len = filesize;
    }
    // 更新http头部
    void prepend_header(const char* header, int len) {
        if (len > HTTP_HEADER_MAX_LENGTH) return;
        httpbuf.base = filebuf.base - len;
        httpbuf.len = len + filebuf.len;
        memcpy(httpbuf.base, header, len);
    }
} file_cache_t;

typedef std::shared_ptr<file_cache_t>           file_cache_ptr;
// filepath => file_cache_ptr
typedef std::map<std::string, file_cache_ptr>   FileCacheMap;

class FileCache {
public:
    FileCacheMap    cached_files;
    std::mutex      mutex_;
    // 用于判断文件是否修改，要重新读
    int             stat_interval;
    // 判断文件缓存是否过期，太久没访问/Open的文件会过期
    int             expired_time;

    FileCache();

    struct OpenParam {
        // [in] 是否重读文件
        bool need_read;
        // [in] 缓存文件的最大尺寸：64M
        int  max_read;
        // [in] url path, 用于生成文件列表页面标题: Index of Path
        const char* path;
        // [out] 文件实际大小
        size_t  filesize;
        // [out] 出错是的错误码
        int  error;

        OpenParam() {
            need_read = true;
            max_read = FILE_CACHE_MAX_SIZE;
            path = "/";
            filesize = 0;
            error = 0;
        }
    };
    file_cache_ptr Open(const char* filepath, OpenParam* param);
    bool Close(const char* filepath);
    bool Close(const file_cache_ptr& fc);
    // 删除过期的文件缓存(LRU)
    void RemoveExpiredFileCache();

protected:
    file_cache_ptr Get(const char* filepath);
};

#endif // HV_FILE_CACHE_H_
