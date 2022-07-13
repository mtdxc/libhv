﻿/*
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
#ifndef UTIL_MINI_H
#define UTIL_MINI_H

#include <cctype>
#include <map>
#include <string>
#include <vector>
#include <fstream>
#include "util.h"
namespace toolkit {

template<typename key, typename variant>
class mINI_basic : public std::map<key, variant> {
    // Public API : existing map<> interface plus following methods
public:
    void parse(const std::string &text) {
        // reset, split lines and parse
        std::vector<std::string> lines = tokenize(text, "\n");
        std::string symbol, tag;
        for (auto &line : lines) {
            // trim blanks
            line = trim(line);
            // split line into tokens and parse tokens
            if (line.empty() || line.front() == ';' || line.front() == '#') {
                continue;
            }
            if (line.size() >= 3 && line.front() == '[' && line.back() == ']') {
                tag = trim(line.substr(1, line.size() - 2));
            } else {
                auto at = line.find('=');
                symbol = trim(tag + "." + line.substr(0, at));
                (*this)[symbol] = (at == std::string::npos ? std::string() : trim(line.substr(at + 1)));
            }
        }
    }

    void parseFile(const std::string &fileName = exePath() + ".ini") {
        std::ifstream in(fileName, std::ios::in | std::ios::binary | std::ios::ate);
        if (!in.good()) {
            std::stringstream ss;
            ss << "invalid ini file:" << fileName;
            throw std::invalid_argument(ss.str());
        }
        auto size = in.tellg();
        in.seekg(0, std::ios::beg);
        std::string buf;
        buf.resize(size);
        in.read((char *) buf.data(), size);
        parse(buf);
    }

    std::string dump(const std::string &header = "; auto-generated by mINI class {",
                     const std::string &footer = "; } ---") const {
        std::string output(header + (header.empty() ? "" : "\r\n")), tag;
        for (auto &pr : *this) {
            std::vector<std::string> kv = tokenize(pr.first, ".");
            if (tag != kv[0]) {
                output += "\r\n[" + (tag = kv[0]) + "]\r\n";
            }
            output += kv[1] + "=" + pr.second + "\r\n";
        }
        return output + "\r\n" + footer + (footer.empty() ? "" : "\r\n");
    }

    void dumpFile(const std::string &fileName = exePath() + ".ini") {
        std::ofstream out(fileName, std::ios::out | std::ios::binary | std::ios::trunc);
        auto dmp = dump();
        out.write(dmp.data(), dmp.size());
    }

    static mINI_basic &Instance();

private:
    std::vector<std::string> tokenize(const std::string &self, const std::string &chars) const {
        std::vector<std::string> tokens(1);
        std::string map(256, '\0');
        for (char ch : chars) {
            map[(uint8_t) ch] = '\1';
        }
        for (char ch : self) {
            if (!map.at((uint8_t) ch)) {
                tokens.back().push_back(ch);
            } else if (tokens.back().size()) {
                tokens.push_back(std::string());
            }
        }
        while (tokens.size() && tokens.back().empty()) {
            tokens.pop_back();
        }
        return tokens;
    }
};

//  handy variant class as key/values
struct variant : public std::string {
    template<typename T>
    variant(const T &t) :
            std::string(std::to_string(t)) {
    }

    template<size_t N>
    variant(const char (&s)[N]) :
            std::string(s, N) {
    }

    variant(const char *cstr) :
            std::string(cstr) {
    }

    variant(const std::string &other = std::string()) :
            std::string(other) {
    }

    template <typename T>
    operator T() const {
        return as<T>();
    }

    template<typename T>
    bool operator==(const T &t) const {
        return 0 == this->compare(variant(t));
    }

    bool operator==(const char *t) const {
        return this->compare(t) == 0;
    }

    template <typename T>
    T as() const {
        return as_default<T>();
    }

private:
    template <typename T>
    T as_default() const {
        T t;
        std::stringstream ss;
        return ss << *this && ss >> t ? t : T();
    }
};

template <>
bool variant::as<bool>() const;

template <>
uint8_t variant::as<uint8_t>() const;

using mINI = mINI_basic<std::string, variant>;

}  // namespace toolkit
#endif //UTIL_MINI_H

