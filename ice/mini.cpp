/*
 * Copyright (c) 2015 r-lyeh (https://github.com/r-lyeh)
 * Copyright (c) 2016-2019 xiongziliang <771730766@qq.com>
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 * 
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 * 
 * 1. The origin of this software must not be misrepresented; you must not
 * claim that you wrote the original software. If you use this software
 * in a product, an acknowledgment in the product documentation would be
 * appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 * misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

#include "mini.h"
#include "hstring.h"
using namespace std;

#if defined(_WIN32)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
extern "C" const IMAGE_DOS_HEADER __ImageBase;
#ifndef PATH_MAX
#define PATH_MAX 1024
#endif // !PATH_MAX
#else
#include <limits.h>

#if defined(__MACH__) || defined(__APPLE__)
#include <mach-o/dyld.h> /* _NSGetExecutablePath */

int uv_exepath(char *buffer, int *size) {
    /* realpath(exepath) may be > PATH_MAX so double it to be on the safe side. */
    char abspath[PATH_MAX * 2 + 1];
    char exepath[PATH_MAX + 1];
    uint32_t exepath_size;
    size_t abspath_size;

    if (buffer == nullptr || size == nullptr || *size == 0)
        return -EINVAL;

    exepath_size = sizeof(exepath);
    if (_NSGetExecutablePath(exepath, &exepath_size))
        return -EIO;

    if (realpath(exepath, abspath) != abspath)
        return -errno;

    abspath_size = strlen(abspath);
    if (abspath_size == 0)
        return -EIO;

    *size -= 1;
    if ((size_t) *size > abspath_size)
        *size = abspath_size;

    memcpy(buffer, abspath, *size);
    buffer[*size] = '\0';

    return 0;
}
#endif //defined(__MACH__) || defined(__APPLE__)
#endif

string exePath(bool isExe /*= true*/) {
    char buffer[PATH_MAX * 2 + 1] = {0};
    int n = -1;
#if defined(_WIN32)
    n = GetModuleFileNameA(isExe?nullptr:(HINSTANCE)&__ImageBase, buffer, sizeof(buffer));
#elif defined(__MACH__) || defined(__APPLE__)
    n = sizeof(buffer);
    if (uv_exepath(buffer, &n) != 0) {
        n = -1;
    }
#elif defined(__linux__)
    n = readlink("/proc/self/exe", buffer, sizeof(buffer));
#endif

    string filePath;
    if (n <= 0) {
        filePath = "./";
    } else {
        filePath = buffer;
    }

#if defined(_WIN32)
    //windows下把路径统一转换层unix风格，因为后续都是按照unix风格处理的  [AUTO-TRANSLATED:33d86ad3]
    //Convert paths to Unix style under Windows, as subsequent processing is done in Unix style
    for (auto &ch : filePath) {
        if (ch == '\\') {
            ch = '/';
        }
    }
#endif //defined(_WIN32)

    return filePath;
}

string exeDir(bool isExe /*= true*/) {
    auto path = exePath(isExe);
    return path.substr(0, path.rfind('/') + 1);
}

string exeName(bool isExe /*= true*/) {
    auto path = exePath(isExe);
    return path.substr(path.rfind('/') + 1);
}

template<>
mINI_basic<variant> &mINI_basic<variant>::Instance(){
    static mINI_basic<variant> instance;
    return instance;
}

template <>
bool variant::as<bool>() const {
    if (empty() || isdigit(front())) {
        //数字开头  [AUTO-TRANSLATED:e4266329]
        //Starts with a number
        return as_default<bool>();
    }
    if (strcasecmp(c_str(), "true") == 0) {
        return true;
    }
    if (strcasecmp(c_str(), "false") == 0) {
        return false;
    }
    //未识别字符串  [AUTO-TRANSLATED:b8037f51]
    //Unrecognized string
    return as_default<bool>();
}

template<>
uint8_t variant::as<uint8_t>() const {
    return 0xFF & as_default<int>();
}


bool loadIniConfig(const char *ini_path) {
    string ini;
    if (ini_path && ini_path[0] != '\0') {
        ini = ini_path;
    } else {
        ini = exePath() + ".ini";
    }
    try {
        mINI tmp;
        tmp.parseFile(ini);

        auto &ref = mINI::Instance();
        for (auto &pr : tmp) {
            if (ref.find(pr.first) == ref.end()) {
                // 新增键
                //WarnL << "unknow config: " << pr.first << " = " << pr.second;
                ref.emplace(pr);
            } else {
                // 更新键
                ref[pr.first] = pr.second;
            }
        }
        // 更新注释和排序
        ref.updateFrom(tmp);
        // NOTICE_EMIT(BroadcastReloadConfigArgs, Broadcast::kBroadcastReloadConfig);
        return true;
    } catch (std::exception &) {
        //InfoL << "dump ini file to:" << ini;
        mINI::Instance().dumpFile(ini);
        return false;
    }
}