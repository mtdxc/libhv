/*
 * mqtt client
 *
 * @build   make examples
 *
 * @test    bin/mqtt_client_test 127.0.0.1 1883 topic payload
 *
 */
#include <map>
#include <list>
#include <string>
#include <sstream>
#include <iostream>
#include <fstream>
#include <thread>
#include "hstring.h"
#include "json.hpp"
#include "mqtt_client.h"
using namespace hv;

/*
 * @test    MQTTS
 * #define  TEST_SSL 1
 *
 * @build   ./configure --with-mqtt --with-openssl && make clean && make
 *
 */
#define TEST_SSL        0
#define TEST_AUTH       0
#define TEST_RECONNECT  1
#define TEST_QOS        0
#define TEST_JSONDUMP   0

uint8_t getCrc(const uint8_t* buff, int size) {
    uint8_t ret = 0;
    for (size_t i = 0; i < size; i++) {
        ret += buff[i];
    }
    return ret;
}

int makeCmd(uint8_t* buff, uint8_t cmd, uint8_t type = 1, uint8_t devId = 1) {
    int ret = 8;
    memset(buff, 0, ret);
    buff[0] = devId;
    buff[1] = cmd;
    buff[2] = type;
    buff[7] = getCrc(buff, 7);
    return ret;
}

class Flag {
public:
    int offset = 0; ///< 偏移
    char size = 1; ///< 大小
    std::string name; ///< 名称
    std::string unit; ///< 单位
    int mod = 1; ///< 除数
    bool time = false; ///< 四个字节，来表示时分的十位和个位

    /// 枚举值，Flag的值是枚举中的一个
    std::map<int, std::string> enums;
    /// 位域，Flag的值可能是这些位的组合
    std::map<char, std::string> bits;
    /// 编辑用的key
    char d0Type = 0;
    uint32_t rawValue = 0;

    Flag() {}
    Flag(int o, char s, const char* n, const char* u="", int m = 1) : offset(o), size(s), name(n), mod(m), unit(u) {}
    // 链式操作
    Flag& addBits(char pos, const char* desc) { bits[pos] = desc; return *this; }
    Flag& addEnums(int pos, const char* desc) { enums[pos] = desc; return *this; }
    Flag& setUnit(const char* u) { unit = u; return *this; }
    Flag& setMod(int m) { mod = m; return *this; }
    Flag& setTime(bool v) { time = v; return *this; }

    // 处理D0消息
    Flag& setD0Type(char v) { d0Type = v; return *this; }
    bool makeD0(uint8_t* buff, uint8_t devId = 1);
    bool canEdit() const { return d0Type != 0; }

    // 返回Flag的字符串表现形式，不包含name
    std::string toString() const;
    bool inRange(int total) const { return offset < total && offset + size <= total; }
    // 获取原始内存值
    bool setValue(uint8_t* buff, int size);
    bool setValue(uint32_t val);
    uint32_t getValue() const { return rawValue; }
};

//NLOHMANN_DEFINE_TYPE_INTRUSIVE(Flag, offset, size, name, rawValue, mod, unit, time, d0Type, enums, bits);
void to_json(nlohmann::json& json, const Flag& f) {
    json["offset"] = f.offset;
    json["size"] = f.size;
    json["name"] = f.name;
    json["val"] = f.rawValue;
    if (f.mod && f.mod != 1) json["mod"] = f.mod;
    if (f.unit.length()) json["unit"] = f.unit;
    if (f.time) json["time"] = f.time;
    if (f.d0Type) json["d0Type"] = f.d0Type;
    if (f.enums.size()) {
        auto& map = json["enums"];
        for (auto it : f.enums) {
            map[std::to_string(it.first)] = it.second;
        }
    }
    if (f.bits.size()) {
        auto& map = json["bits"];
        for (auto it : f.bits) {
            map[std::to_string(it.first)] = it.second;
        }
    }
}

void from_json(const nlohmann::json& json, Flag& f) {
    f.offset = json["offset"].get<int>();
    f.size = json["size"].get<char>();
    f.name = json["name"].get<std::string>();
    f.rawValue = json["val"].get<uint32_t>();
    auto it = json.find("mod");
    if (json.end() != it)
        f.mod = it->get<int>();

    it = json.find("unit");
    if (json.end() != it)
        f.unit = it->get<std::string>();

    it = json.find("d0Type");
    if (json.end() != it)
        f.d0Type = it->get<int>();

    f.enums.clear();
    it = json.find("enums");
    if (json.end() != it) {
        for (auto child : it->items()) {
            f.enums[std::stoi(child.key())] = child.value().get<std::string>();
        }
    }

    f.bits.clear();
    it = json.find("bits");
    if (json.end() != it) {
        for (auto child : it->items()) {
            f.bits[std::stoi(child.key())] = child.value().get<std::string>();
        }
    }
}

bool Flag::setValue(uint8_t* buff, int total)
{
    if (!inRange(total)) {
        printf("skip %s out of range %d,%d total %d\n", name.c_str(), offset, size, total);
        return false;
    }
    uint32_t ret = 0;
    for (size_t i = 0; i < size; i++)
    {
        ret = ret << 8;
        ret += buff[offset + i];
    }
    return setValue(ret);
}

bool Flag::setValue(uint32_t val) {
    if (enums.size() && !enums.count(val)) {
        printf("warning %s val %d not exist in enums\n", name.c_str(), val);
        return false;
    }
    rawValue = val;
    return true;
}

bool Flag::makeD0(uint8_t* buff, uint8_t devId) {
    if (!d0Type) return false;
    buff[0] = devId;
    buff[1] = 0xD0;
    buff[2] = d0Type;
    // set value
    *((uint32_t*)(buff + 3)) = htonl(rawValue);
    buff[7] = getCrc(buff, 7);
    return true;
}

std::string Flag::toString() const {
    char buff[64] = { 0 };
    if (time) {
        uint32_t l = htonl(rawValue);
        uint8_t* data = (uint8_t*)&l;
        sprintf(buff, "%d%d:%d%d", data[0], data[1], data[2], data[3]);
        return buff;
    }

    if (enums.size()) {
        auto it = enums.find(rawValue);
        if (it != enums.end()) {
            return it->second;
        }
        else {
            snprintf(buff, sizeof(buff), "unknown enum %u", rawValue);
            return buff;
        }
    }
    else if (bits.size()) {
        std::string ret;
        for (auto it : bits) {
            if (rawValue & (1 << it.first)) {
                if (ret.empty())
                    ret = it.second;
                else
                    ret = ret + "," + it.second;
            }
        }
        return ret;
    }
    else if(mod && mod != 1) {
        float value = rawValue * 1.0f / mod;
        snprintf(buff, sizeof(buff), "%.2f %s", value, unit.c_str());
    }
    else {
        snprintf(buff, sizeof(buff), "%d %s", rawValue, unit.c_str());
    }
    return buff;
}

// 协议解析器
class PduParser {
    int _nexIndex = 0;
    // 保证内存地址与返回的一致
    std::list<Flag> flags;
public:
    uint8_t cmd = 0;
    std::string desc;
    PduParser() {}
    PduParser(uint8_t c, const char* d) : cmd(c), desc(d) {}
    using Ptr = std::shared_ptr<PduParser>;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(PduParser, cmd, desc, flags);

    void parse(uint8_t* buff, int size) {
        std::ostringstream stm;
        for (auto it: flags) {
            if (it.setValue(buff, size)) // 根据buff设置值
                stm << it.name << ": " << it.toString() << std::endl;
        }
        printf("%s\n", stm.str().c_str());
    }

    // 重置解析器
    void reset(int next) {
        flags.clear(); 
        setNextIndex(next);
    }
    void setNextIndex(int next) {
        _nexIndex = next;
    }
    // 根据名字查找Flag
    Flag* getFlag(const char* name) { 
        Flag* ret = nullptr;
        for (auto it : flags)
            if (it.name == name) return &it;
        return ret;
    }
    Flag& addFlag(int size, const char* name, const char* unit = "") {
        Flag flag(_nexIndex, size, name, unit);
        _nexIndex += size;
        flags.push_back(flag);
        return *flags.rbegin();
    }
    Flag& addByteFlag(const char* name) {
        return addFlag(1, name);
    }
    Flag& addShortFlag(const char* name, const char* unit = "", int mod = 1) {
        return addFlag(2, name, unit).setMod(mod);
    }
    Flag& addTimeFlag(const char* name) {
        return addFlag(4, name).setTime(true);
    }
    Flag& addLongFlag(const char* name, const char* unit = "", int mod = 1) {
        return addFlag(4, name, unit).setMod(mod);
    }
};

class JyMqttClient : public MqttClient {
    PduParser::Ptr getParser(uint8_t cmd, const char* desc = nullptr);
    // 定义协议解析器
    std::map<uint8_t, PduParser::Ptr> parses;
    void registParser();

    std::string resp_topic, req_topic;
public:
    // 公共设备id
    int devId = 1;

    JyMqttClient(hloop_t* loop = NULL);
    void setupTopic(const char* topic);
    bool sendCmd(uint8_t cmd, uint8_t type = 1) { 
        if (!isConnected()) return false;
        publish(req_topic, makeCmd(cmd, type));
        return true;
    }
    std::string makeCmd(uint8_t cmd, uint8_t type = 1) { 
        std::string ret;
        ret.resize(8);
        ::makeCmd((uint8_t*)ret.data(), cmd, type, devId);
        return ret;
    }
    void printCmds() {
        printf("支持指令有:\n");
        for (auto it : parses) {
            printf("\t%X\t%s\n", it.first, it.second->desc.c_str());
        }
    }
    void dumpCmds(const char* file) {
        nlohmann::json root;
        for (auto it : parses) {
            root.push_back(*it.second);
        }
        std::ofstream stm(file);
        stm << root.dump(2);
    }
    void loadCmds(const char* file) {
        std::ifstream stm(file);
        nlohmann::json root = nlohmann::json::parse(stm);
        parses.clear();
        for (auto it : root) {
            auto parser = std::make_shared<PduParser>(it);
            parses[parser->cmd] = parser;
        }
    }
};

JyMqttClient::JyMqttClient(hloop_t* loop) : MqttClient(loop) {
    registParser();
    onConnect = [this](MqttClient* cli) {
        printf("connected!\n");
        subscribe(resp_topic.c_str());
        //publish(req_topic, makeCmd(0xB1));
    };

    onMessage = [this](MqttClient* cli, mqtt_message_t* msg) {
        printf("topic: %.*s recv %d bytes\n", msg->topic_len, msg->topic, msg->payload_len);
        // printf("payload %d: %.*s\n", msg->payload_len, msg->payload_len, msg->payload);
        if (msg->payload_len > 2) {
            uint8_t* buff = (uint8_t*)msg->payload;
            auto it = getParser(buff[1]);
            if (it) {
                it->parse(buff, msg->payload_len);
            }
        }
    };

    onClose = [](MqttClient* cli) { printf("disconnected!\n"); };

}

void JyMqttClient::setupTopic(const char* topic) {
    if (!topic) topic = "mppt";
    char buff[256];
    sprintf(buff, "%s/req", topic);
    req_topic = buff;
    sprintf(buff, "%s/resp", topic);
    resp_topic = buff;
    printf("use topic %s %s\n", req_topic.c_str(), resp_topic.c_str());
}

PduParser::Ptr JyMqttClient::getParser(uint8_t cmd, const char* desc) {
    PduParser::Ptr ret;
    auto it = parses.find(cmd);
    if (it != parses.end())
        ret = it->second;
    else if (desc) {
        ret = std::make_shared<PduParser>(cmd, desc);
        parses[cmd] = ret;
    }
    return ret;
}

void JyMqttClient::registParser() {
    {
        auto parser = getParser(0xEF, "错误返回");
        parser->addByteFlag("DevID");
        parser->addByteFlag("CMD"); // 0XEE
        parser->addByteFlag("ErrorCode")
            .addEnums(1, "当前状态不能完成操作")
            .addEnums(2, "不能识别的参数代码")
            .addEnums(3, "参数数据溢出");
        parser->addByteFlag("ReqCmd");  // 原命令码
        parser->addByteFlag("ReqType"); // 原控制码
        parser->addByteFlag("备用");
        parser->addByteFlag("备用");
        parser->addByteFlag("校验码"); // 1-n-1字节的累加和，取低字节
    }

    // 设置波特率，无返回值 cmd = 0xDE, 命令类型 0x42, Data1: 1=1200,2=2400,3=4800,4=9600bps
    // 设置时钟, 群控不返回，地址0x01~0xF0原样返回 cmd = 0xDF, 命令类型=年（十位和个位）0x12表示2018年，Data1-4分别表示月日时分
    {
        auto parser = getParser(0xDF, "设置时钟");
        parser->addByteFlag("DevID");
        parser->addByteFlag("CMD"); // 0xDF
        parser->addByteFlag("Year");
        parser->addByteFlag("Month");
        parser->addByteFlag("Day");
        parser->addByteFlag("Hour");
        parser->addByteFlag("Min");
        parser->addByteFlag("校验码");
    }
    {
        auto parser = getParser(0xD0, "设置参数");
        parser->addByteFlag("DevID");
        parser->addByteFlag("CMD"); // 0xD0
        parser->addByteFlag("Type")
            .addEnums(0x09, "电池类型设置") // 1 bytes，数据1,2,3无意义，填0。数据4: 0 = 铅酸免维护，1 = 铅酸胶体，2 = 铅酸液体，3 = 锂电
            .addEnums(0x0A, "电池额定电压设置") // 1 bytes: 0=自动识别，以铅酸电池12V每只为标准，1 = 12V, 2 = 24V以此类推
            .addEnums(0x0C, "DC输出控制")   // 1 bytes: 0=关闭，1=自动，2=时控，3=光控，4=远程控制
            .addEnums(0x11, "型号编码")   // 1 bytes
            .addEnums(0x12, "时控时间组标志")   // 1 bytes: Bit0：时间组1的时控标志，0 = 禁止，1 = 开启
            // Bit1：时间组2的时控标志，0 = 禁止，1 = 开启，无显示板设置
            .addEnums(0x21, "均充电压") // 2 bytes: 数据3高字节，数据4低字节，数据1, 2无意义，填0；
            // 带2位有效小数，电池类型设为锂电均充电压设置无效。自动识别状态下设置无效。
            .addEnums(0x22, "浮充电压")         // 同上
            .addEnums(0x23, "电低压保护电压") // 同上
            .addEnums(0x25, "最大充电电流")     // 同上，设定最大值不能超过硬件限流最大值
            .addEnums(0x26, "低压恢复电压")     // 同上
            .addEnums(0x27, "过压保护电压") // 同上
            .addEnums(0x28, "过压恢复电压") // 同上
            .addEnums(0x29, "PV启动电压")   // 2字节参数，无小数，最大值999
            .addEnums(0x2A, "PV关闭电压")   // 同上
            .addEnums(0x2B, "延时开启时间") // 2字节参数，以秒为单位，光控模式下PV达到设定电压后延时开启DC输出的时间，最大值999
            .addEnums(0x2C, "延时关闭时间") // 同上
            .addEnums(0x2D, "时控1开启时间") // 4字节参数，数据1时十位，数据2时个位，数据3分十位，数据4分个位，无显示板设置无效。
            .addEnums(0x2E, "时控1关闭时间")  // 同上
            .addEnums(0x2F, "时控2开启时间")  // 同上
            .addEnums(0x30, "时控2关闭时间"); // 同上
        parser->addByteFlag("Data1"); // 高字节
        parser->addByteFlag("Data2");
        parser->addByteFlag("Data3");
        parser->addByteFlag("Data4");  // 低字节
        parser->addByteFlag("校验码"); // 1-n-1字节的累加和，取低字节
    }
    {
        auto parser = getParser(0xC0, "开关功能"); // parseC0 正常返回与setC0一样
        parser->addByteFlag("DevID");
        parser->addByteFlag("CMD"); // 0xC0
        parser->addByteFlag("Type")
            .addEnums(1, "允许充电")
            .addEnums(2, "禁止充电")
            .addEnums(3, "远程开启DC输出")
            .addEnums(4, "远程关闭DC输出")
            .addEnums(5, "重置蜂鸣器报警") // 有新故障重新触发报警
            .addEnums(6, "开启背光"); // 1分钟后关闭 
        parser->addByteFlag("Data1"); // 无意义
        parser->addByteFlag("Data2");
        parser->addByteFlag("Data3");
        parser->addByteFlag("Data4");
        parser->addByteFlag("校验码"); // 1-n-1字节的累加和，取低字节
    }
    {
        // 远程上位机仅查询设置参数命令
        auto parser = getParser(0xB2, "查询设置参数");
        parser->addByteFlag("DevID");
        parser->addByteFlag("CMD");  // 0xB3
        parser->addByteFlag("Type"); // 0x01
        parser->addByteFlag("电池类型").setD0Type(0x09)
            .addEnums(0, "铅酸免维护")
            .addEnums(1, "铅酸胶体")
            .addEnums(2, "铅酸液体")
            .addEnums(3, "锂电");
        parser->addByteFlag("识别方式")
            .addEnums(0, "自动识别")
            .addEnums(1, "手动设定");
        parser->addByteFlag("电池数量");
        parser->addByteFlag("DC输出控制").setD0Type(0x0C)
            .addEnums(0, "关闭")
            .addEnums(1, "自动")
            .addEnums(2, "时控开关")
            .addEnums(3, "光控")
            .addEnums(4, "远程控制");
        parser->addByteFlag("本机地址");
        parser->addByteFlag("波特率")
            .addEnums(1, "1200")
            .addEnums(2, "2400")
            .addEnums(3, "4800")
            .addEnums(4, "9600");
        // 9
        parser->addShortFlag("恒定电压", "V", 100);
        parser->addShortFlag("均充电压", "V", 100).setD0Type(0x21);
        parser->addShortFlag("浮充电压", "V", 100).setD0Type(0x22);
        parser->addShortFlag("放电电压", "V", 100).setD0Type(0x23);

        parser->addShortFlag("硬件最大充电电流", "A", 100);
        parser->addShortFlag("最大充电电流", "A", 100).setD0Type(0x25);
        parser->addShortFlag("运行充电电流限制", "A", 100);
        // 23
        parser->addByteFlag("型号编码").setD0Type(0x11);
        parser->addByteFlag("时控输出标志").setD0Type(0x12)
            .addBits(0, "时控时间组1")
            .addBits(1, "时控时间组2");
        parser->addShortFlag("过放恢复电压", "V", 100).setD0Type(0x26);
        parser->addShortFlag("过压保护电压", "V", 100).setD0Type(0x27);
        parser->addShortFlag("过压恢复电压", "V", 100).setD0Type(0x28);

        parser->addShortFlag("PV启动电压", "V").setD0Type(0x29);
        parser->addShortFlag("PV关闭电压", "V").setD0Type(0x2A);
        parser->addShortFlag("延时开启时间", "s").setD0Type(0x2B);
        parser->addShortFlag("延时关闭时间", "s").setD0Type(0x2C);
        // 39
        parser->addTimeFlag("时控1开启时间").setD0Type(0x2D);
        parser->addTimeFlag("时控1关闭时间").setD0Type(0x2E);
        parser->addTimeFlag("时控2开启时间").setD0Type(0x2F);
        parser->addTimeFlag("时控2关闭时间").setD0Type(0x30);
        // 63 校验码
    }
    {
        // 远程上位机仅查询实时数据命令：0XB3
        auto parser = getParser(0xB3, "查询实时数据");
        parser->addByteFlag("DevID");
        parser->addByteFlag("CMD");  // 0xB3
        parser->addByteFlag("Type"); // 0x01
        parser->addByteFlag("运行状态")
            .addBits(0, "电池识别错误")   // 运行状态 0=正常；1=异常（电池自动识别错误）
            .addBits(1, "过放保护")       // 电池状态 0=正常；1=过放保护
            .addBits(2, "风扇故障")       // 风扇状态 0=正常；1=风扇故障
            .addBits(3, "过温保护")       // 温度状态 0=正常；1=过温保护
            .addBits(4, "DC输出短路保护") // DC输出状态 0=正常；1=DC输出短路保护
            .addBits(5, "内部温度1故障")  // 内部温度1状态 0=正常；1=故障
            .addBits(6, "内部温度2故障")  // 内部温度2状态 0=正常；1=故障
            .addBits(7, "外部温度1故障"); // 外部温度1状态 0=正常；1=故障
        parser->addByteFlag("充电状态")
            .addBits(0, "充电")         // 充电状态 0=停充；1=充电
            .addBits(1, "均充")         // 1有效
            .addBits(2, "跟踪")         // 1有效
            .addBits(3, "浮充")         // 1有效
            .addBits(4, "充电限流")     // 1有效
            .addBits(5, "充电降额")     // 1有效
            .addBits(6, "远程禁止充电") // 1有效
            .addBits(7, "PV过压");      // 1有效
        parser->addByteFlag("控制状态")
            .addBits(0, "充电输出继电器") // 0=关闭；1=开启
            .addBits(1, "负载输出")      // 0=关闭；1=开启
            .addBits(2, "风扇")          // 0=关闭；1=开启
            //.addBits(3, "")
            .addBits(4, "过充保护")
            .addBits(5, "过压保护");
        //.addBits(6, "备用")
        //.addBits(7, "备用");
        parser->addShortFlag("PV电压", "V", 10);
        parser->addShortFlag("电池电压", "V", 100);
        parser->addShortFlag("充电电流", "A", 100);
        // 12
        parser->addShortFlag("内部温度1", "°C", 10);
        parser->addShortFlag("内部温度2", "°C", 10);
        parser->addShortFlag("外部温度1", "°C", 10);
        parser->addByteFlag("备用");
        parser->addByteFlag("备用");
        // 20
        parser->addLongFlag("日发电量", "度", 1000);
        parser->addLongFlag("总发电量", "度", 1000);
    }
    
    {
        // 远程上位机查询MPPT命令
        auto parser = getParser(0xB1, "查询所有参数");
        parser->addByteFlag("DevID");
        parser->addByteFlag("CMD");  // 0xB1
        parser->addByteFlag("Type"); // 0x01
        parser->addByteFlag("运行状态")
            .addBits(0, "电池识别错误")   // 运行状态 0=正常；1=异常（电池自动识别错误）
            .addBits(1, "过放保护")       // 电池状态 0=正常；1=过放保护
            .addBits(2, "风扇故障")       // 风扇状态 0=正常；1=风扇故障
            .addBits(3, "过温保护")       // 温度状态 0=正常；1=过温保护
            .addBits(4, "DC输出短路保护") // DC输出状态 0=正常；1=DC输出短路保护
            .addBits(5, "内部温度1故障")  // 内部温度1状态 0=正常；1=故障
            .addBits(6, "内部温度2故障")  // 内部温度2状态 0=正常；1=故障
            .addBits(7, "外部温度1故障"); // 外部温度1状态 0=正常；1=故障
        parser->addByteFlag("充电状态")
            .addBits(0, "充电")         // 充电状态 0=停充；1=充电
            .addBits(1, "均充")         // 1有效
            .addBits(2, "跟踪")         // 1有效
            .addBits(3, "浮充")         // 1有效
            .addBits(4, "充电限流")     // 1有效
            .addBits(5, "充电降额")     // 1有效
            .addBits(6, "远程禁止充电") // 1有效
            .addBits(7, "PV过压");      // 1有效
        parser->addByteFlag("控制状态")
            .addBits(0, "充电输出继电器")  // 0=关闭；1=开启
            .addBits(1, "负载输出")       // 0=关闭；1=开启
            .addBits(2, "风扇")           // 0=关闭；1=开启
            //.addBits(3, "")
            .addBits(4, "过充保护")
            .addBits(5, "过压保护");
        //.addBits(6, "备用")
        //.addBits(7, "备用");
        parser->addByteFlag("备用");
        parser->addByteFlag("备用");
        // 8
        parser->addByteFlag("电池类型")
            .addEnums(0, "铅酸免维护")
            .addEnums(1, "铅酸胶体")
            .addEnums(2, "铅酸液体")
            .addEnums(3, "锂电");
        parser->addByteFlag("识别方式")
            .addEnums(0, "自动识别")
            .addEnums(1, "手动设定");
        parser->addByteFlag("电池数量");
        parser->addByteFlag("负载控制方式")
            .addEnums(0, "关闭")
            .addEnums(1, "自动") // 有电就输出
            .addEnums(2, "时控开关")
            .addEnums(3, "光控")
            .addEnums(4, "远程控制");
        parser->addByteFlag("本机地址");
        parser->addByteFlag("波特率")
            .addEnums(1, "1200")
            .addEnums(2, "2400")
            .addEnums(3, "4800")
            .addEnums(4, "9600");
        parser->addByteFlag("备用");
        parser->addByteFlag("备用");
        // 16
        parser->addShortFlag("恒定电压", "V", 100);
        parser->addShortFlag("均充电压", "V", 100);
        parser->addShortFlag("浮充电压", "V", 100);
        parser->addShortFlag("放电电压", "V", 100);

        parser->addShortFlag("硬件最大充电电流", "A", 100);
        parser->addShortFlag("最大充电电流", "A", 100);
        parser->addShortFlag("运行充电电流限制", "A", 100);
        // 30
        parser->addShortFlag("PV电压", "V", 10);
        parser->addShortFlag("电池电压", "V", 100);
        parser->addShortFlag("充电电流", "A", 100);
        // 36
        parser->addShortFlag("内部温度1", "°C", 10);
        parser->addShortFlag("内部温度2", "°C", 10);
        parser->addShortFlag("外部温度1", "°C", 10);

        parser->addByteFlag("备用");
        parser->addByteFlag("备用");
        // 44
        parser->addLongFlag("日发电量", "度", 1000);
        parser->addLongFlag("总发电量", "度", 1000);

        parser->addByteFlag("型号编码");
        parser->addByteFlag("时控输出标志")
            .addBits(0, "时控时间组1")
            .addBits(1, "时控时间组2");

        // 54
        parser->addShortFlag("过放恢复电压", "V", 100);
        parser->addShortFlag("过压保护电压", "V", 100);
        parser->addShortFlag("过压恢复电压", "V", 100);

        parser->addShortFlag("PV启动电压", "V");
        parser->addShortFlag("PV关闭电压", "V");
        // 64
        parser->addShortFlag("延时开启时间", "s");
        parser->addShortFlag("延时关闭时间", "s");
        // 68
        parser->addTimeFlag("时控1开启时间");
        parser->addTimeFlag("时控1关闭时间");
        parser->addTimeFlag("时控2开启时间");
        parser->addTimeFlag("时控2关闭时间");
    }
}

JyMqttClient cli;
void printHelp() {
    printf("help/h \tprint this\n");
    printf("quit/q \tquit app\n");
    printf("cmd/c code args \tsend cmd to remote\n");
    cli.printCmds();
}

// 此时工程的c++编译选项还必须加上/utf-8
#ifdef _WIN32
#include <locale.h>
int main(int argc, char** argv) {
    setlocale(LC_ALL, "en_US.UTF-8");
#else
int main(int argc, char** argv) {
#endif
    const char* host = "106.12.23.22"; 
    int port = 1883;
    const char* user = nullptr;
    const char* pwd = nullptr;
    const char* topic = "mppt";
    // 92 校验码
    if (argc > 2) {
        user = argv[1];
        pwd = argv[2];
    }
    if (argc > 4) {
        host = argv[3];
        port = atoi(argv[4]);
    }
    if (argc > 5) {
        topic = argv[5];
    }

    cli.setupTopic(topic);
    
    if (user && pwd)
        cli.setAuth(user, pwd);

#if TEST_RECONNECT
    reconn_setting_t reconn;
    reconn_setting_init(&reconn);
    reconn.min_delay = 1000;
    reconn.max_delay = 10000;
    reconn.delay_policy = 2;
    cli.setReconnect(&reconn);
#endif

    cli.setPingInterval(10);

    int ssl = 0;
#if TEST_SSL
    ssl = 1;
#endif
    cli.connect(host, port, ssl);
    std::thread([] { cli.run(); }).detach();
#if TEST_JSONDUMP
    const char* dumpfile = "cmds.json";
    cli.dumpCmds(dumpfile);
    cli.loadCmds(dumpfile);
#endif
    printHelp();
    std::string str;
    while (std::getline(std::cin, str)) {
        auto lst = hv::split(hv::trim(str), ' ');
        if (lst.size()) str = lst[0];
        if (str == "quit" || str == "q") {
            if (cli.isConnected())
                cli.disconnect();
            cli.stop();
            break;
        }
        else if (str == "cmd" || str == "c") {
            int code = 0xB1;
            sscanf(lst[1].c_str(), "%X", &code);
            int type = 1;
            if (lst.size() > 2) {
                scanf(lst[2].c_str(), "%d", &type);
            }
            cli.sendCmd(code, type);
        }
        else if (str == "help" || str == "h") {
            printHelp();
        }
    }
    return 0;
}
