#include "http_page.h"
#include "hdir.h"
#include "hurl.h"

#define AUTOINDEX_FILENAME_MAXLEN       50

void make_http_status_page(http_status status_code, std::string& page) {
    char szCode[128];
    snprintf(szCode, sizeof(szCode), "%d %s", status_code, http_status_str(status_code));
    page += R"(<!DOCTYPE html>
<html>
<head>
  <title>)";
    page += szCode;
    page += R"(</title>
</head>
<body>
  <center><h1>)";
    page += szCode;
    page += R"(</h1></center>
  <hr>
</body>
</html>)";
}

void make_index_of_page(const char* dir, std::string& page, const char* url) {
    char c_str[1024] = {0};
    snprintf(c_str, sizeof(c_str), R"(<!DOCTYPE html>
<html>
<head>
  <title>Index of %s</title>
</head>
<body>
  <h1>Index of %s</h1>
  <hr>
)", url, url);
    page += c_str;

    page += "  <table border=\"0\">\n";
    page += R"(    <tr>
      <th align="left" width="30%">Name</th>
      <th align="left" width="20%">Date</th>
      <th align="left" width="20%">Size</th>
    </tr>
)";

#define _ADD_TD_(page, td)  \
    page += "      <td>";   \
    page += td;             \
    page += "</td>\n";      \

    std::list<hdir_t> dirs;
    listdir(dir, dirs);
    std::string escaped_name;
    for (auto& item : dirs) {
        if (item.name[0] == '.' && item.name[1] == '\0') continue;
        page += "    <tr>\n";
        // fix CVE-2023-26146
        escaped_name = hv::escapeHTML(item.name);
        const char* filename = escaped_name.c_str();
        size_t len = escaped_name.size() + (item.type == 'd');
        // name
        snprintf(c_str, sizeof(c_str), "<a href=\"%s%s\">%s%s</a>",
                filename,
                item.type == 'd' ? "/" : "",
                len < AUTOINDEX_FILENAME_MAXLEN ? filename : std::string(filename, filename+AUTOINDEX_FILENAME_MAXLEN-4).append("...").c_str(),
                item.type == 'd' ? "/" : "");
        _ADD_TD_(page, c_str)
        if (strcmp(filename, "..") != 0) {
            // mtime
            struct tm* tm = localtime(&item.mtime);
            snprintf(c_str, sizeof(c_str), "%04d-%02d-%02d %02d:%02d:%02d",
                    tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
            _ADD_TD_(page, c_str)
            // size
            if (item.type == 'd') {
                page += '-';
            }
            else {
                float hsize;
                if (item.size < 1024) {
                    snprintf(c_str, sizeof(c_str), "%lu", (unsigned long)item.size);
                }
                else if ((hsize = item.size/1024.0f) < 1024.0f) {
                    snprintf(c_str, sizeof(c_str), "%.1fK", hsize);
                }
                else if ((hsize /= 1024.0f) < 1024.0f) {
                    snprintf(c_str, sizeof(c_str), "%.1fM", hsize);
                }
                else {
                    hsize /= 1024.0f;
                    snprintf(c_str, sizeof(c_str), "%.1fG", hsize);
                }
                _ADD_TD_(page, c_str)
            }
        }
        page += "    </tr>\n";
    }

#undef _ADD_TD_

    page += R"(  </table>
  <hr>
</body>
</html>
)";
}
