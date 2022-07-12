#ifndef HV_INI_PARSER_H_
#define HV_INI_PARSER_H_

#include <string>

#include "hexport.h"

#define DEFAULT_INI_COMMENT "#"
#define DEFAULT_INI_DELIM   "="

// fwd
class IniNode;

class HV_EXPORT IniParser {
public:
    IniParser();
    ~IniParser();
	
	static IniParser& Instance();

    int LoadFromFile(const char* filepath);
    int LoadFromMem(const char* data);
    int Unload();
    int Reload();
	
    std::string DumpString();
    int Save();
    int SaveAs(const char* filepath);

    std::string GetValue(const std::string& key, const std::string& section = "");
    void        SetValue(const std::string& key, const std::string& value, const std::string& section = "");

    // T = [bool, int, float]
    template<typename T>
    T Get(const std::string& key, const std::string& section = "", T defvalue = 0) {
        std::string str = GetValue(key, section);
        if (str.empty()) return defvalue;
        std::istringstream stm(str);
        T ret; stm >> ret;
        return ret;
    }

    // T = [bool, int, float]
    template<typename T>
    void Set(const std::string& key, const T& value, const std::string& section = "") {
        std::stringstream stm;
        stm << value;
        SetValue(key, stm.str(), section);
    }

protected:
    void DumpString(IniNode* pNode, std::string& str);

public:
    std::string _comment;
    std::string _delim;
    std::string _filepath;
private:
    IniNode* root_;
};

#endif // HV_INI_PARSER_H_
