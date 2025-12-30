#ifndef __MQTT_TOPIC_TREE_H__
#define __MQTT_TOPIC_TREE_H__

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <set>
#include <mutex>
#include <queue>
#include <sstream>
#include <functional>

// for MqttSession
#include "evpp/Channel.h"
#include "WebSocketChannel.h"
struct MqttSession {
    using Ptr = std::shared_ptr<MqttSession>;
    hv::SocketChannelPtr tcp;
    WebSocketChannelPtr ws;
    std::string recv_buf; // 用于ws的接收分包

    int write(const void* buff, int size) {
        int ret = 0;
        if (tcp)
            ret = tcp->write(buff, size);
        else if (ws)
            ret = ws->send((const char*)buff, size);
        return ret;
    }

    bool close() {
        int ret = -1;
        if (tcp) {
            ret = tcp->close(true);
            tcp = nullptr;
        } 
        if (ws) {
            ret = ws->close();
            ws = nullptr;
        }
        recv_buf.clear();
        return ret;
    }
};

// 订阅信息
struct SubscriptionInfo {
    std::weak_ptr<MqttSession> session;
    uint8_t qos = 0;            // 订阅的QoS级别
    bool is_shared = false;         // 是否是共享订阅
    std::string share_name; // 共享组名（如果是共享订阅）
    
    SubscriptionInfo() {}
    SubscriptionInfo(std::weak_ptr<MqttSession> s, uint8_t q, bool shared = false, 
                     const std::string& share = "")
        : session(s), qos(q), is_shared(shared), share_name(share) {}
};

// 保留消息
struct RetainedMessage {
    std::vector<uint8_t> payload;
    uint8_t qos = 0;
    bool dup = false;
    time_t timestamp = 0;
    
    RetainedMessage() {}
    RetainedMessage(const std::vector<uint8_t>& p, uint8_t q, bool d)
        : payload(p), qos(q), dup(d), timestamp(time(nullptr)) {}
};

// 主题节点
class TopicNode {
    std::string name_; // 节点名称（主题层级）
public:
    // 订阅者列表（包含QoS信息）
    std::vector<SubscriptionInfo> subscribers;
    
    // 保留消息
    std::unique_ptr<RetainedMessage> retained_message;
    
    // 子节点映射（主题层级 -> 子节点）
    std::unordered_map<std::string, std::shared_ptr<TopicNode>> children;
    
    // 是否是通配符节点
    bool is_wildcard() const {
        return !name_.empty() && (name_ == "+" || name_ == "#");
    }
    
    bool is_single_wildcard() const {
        return name_ == "+";
    }

    // 是否是多层通配符
    bool is_multi_wildcard() const {
        return name_ == "#";
    }
    
    // 设置节点名称
    void set_name(const std::string& name) {
        name_ = name;
    }
    
    const std::string& get_name() const {
        return name_;
    }
    
    // 添加订阅者
    void add_subscriber(std::weak_ptr<MqttSession> session, uint8_t qos,
                       bool is_shared = false, const std::string& share_name = "") {
        // 检查是否已存在相同订阅
        for (auto& sub : subscribers) {
            auto s1 = sub.session.lock(), s2 = session.lock();
            if (s1 && s2) {
                if (s1.get() == s2.get() && sub.qos == qos && 
                    sub.is_shared == is_shared && sub.share_name == share_name) {
                    return; // 已存在相同订阅
                }
            }
        }
        
        subscribers.emplace_back(session, qos, is_shared, share_name);
    }
    
    // 移除订阅者
    void remove_subscriber(std::shared_ptr<MqttSession> session,
                          const std::string& share_name = "") {
        auto it = subscribers.begin();
        while (it != subscribers.end()) {
            if (auto s = it->session.lock()) {
                if (s.get() == session.get()) {
                    if (share_name.empty() || it->share_name == share_name) {
                        it = subscribers.erase(it);
                        continue;
                    }
                }
            }
            ++it;
        }
    }
    
    // 清理过期订阅
    void clean_expired_subscribers() {
        auto it = subscribers.begin();
        while (it != subscribers.end()) {
            if (it->session.expired()) {
                it = subscribers.erase(it);
            } else {
                ++it;
            }
        }
    }

};

// 主题匹配结果
using MatchResult = SubscriptionInfo;

// 主题树主类
class TopicTree {
public:
    TopicTree();
    ~TopicTree() = default;
    
    // 订阅主题
    bool subscribe(const std::string& topic_filter,
                   std::shared_ptr<MqttSession> session,
                   uint8_t qos,
                   bool is_shared = false,
                   const std::string& share_name = "");
    
    // 取消订阅
    bool unsubscribe(const std::string& topic_filter,
                     std::shared_ptr<MqttSession> session,
                     const std::string& share_name = "");
    
    // 获取主题的所有订阅者（支持通配符匹配）
    std::vector<MatchResult> get_subscribers(const std::string& topic,
                                            bool include_retained = false);
    
    // 获取共享订阅的下一个接收者
    std::vector<MatchResult> get_shared_subscribers(const std::string& topic,
                                                   const std::string& share_name);
    
    // 设置保留消息
    void set_retained_message(const std::string& topic,
                             const std::vector<uint8_t>& payload,
                             uint8_t qos = 0,
                             bool dup = false);
    
    // 获取保留消息
    std::vector<std::pair<std::string, RetainedMessage>> 
    get_retained_messages(const std::string& topic_filter);
    
    // 清除保留消息
    bool clear_retained_message(const std::string& topic);
    
    // 检查主题是否合法
    static bool is_valid_topic(const std::string& topic, bool allow_wildcard = false);
    
    // 检查主题过滤器是否合法
    static bool is_valid_topic_filter(const std::string& topic_filter);
    
    // 获取会话的所有订阅
    std::vector<std::string> get_session_subscriptions(std::shared_ptr<MqttSession> session);
    
    // 清理过期订阅
    void clean_expired_subscriptions();
    
    // 获取统计信息
    struct Statistics {
        size_t total_nodes;
        size_t total_subscribers;
        size_t total_retained_messages;
        size_t max_depth;
    };
    
    Statistics get_statistics() const;
    
    // 转储树结构（调试用）
    void dump_tree(std::function<void(const std::string&)> print_func) const;
    
private:
    // 根节点
    std::shared_ptr<TopicNode> root_;
    
    // 线程安全
    mutable std::mutex mutex_;
        
    // 分割主题为层级
    static std::vector<std::string> split_topic(const std::string& topic);
    
    // 验证主题层级
    static bool is_valid_topic_level(const std::string& level, bool allow_wildcard);
    
    // 创建或获取节点
    std::shared_ptr<TopicNode> get_or_create_node(const std::vector<std::string>& tokens,
                                                  size_t depth, bool create = true);
    
    // 获取节点（不创建）
    std::shared_ptr<TopicNode> get_node(const std::vector<std::string>& tokens, size_t depth) const {
        return const_cast<TopicTree*>(this)->get_or_create_node(tokens, depth, false);
    }
    
    // 递归匹配订阅者
    void collect_subscribers(std::shared_ptr<TopicNode> node,
                             const std::vector<std::string>& tokens,
                             size_t depth,
                             std::vector<MatchResult>& results,
                             bool for_publish = true) const;
    
    // 递归匹配保留消息
    void collect_retained_messages(std::shared_ptr<TopicNode> node,
                                   const std::vector<std::string>& tokens,
                                   size_t depth,
                                   const std::string& current_path,
                                   std::vector<std::pair<std::string, RetainedMessage>>& results) const;
    
    // 递归收集统计信息
    void collect_statistics(std::shared_ptr<TopicNode> node,
                            size_t depth,
                            Statistics& stats) const;
    
    // 递归转储树
    void dump_node(std::shared_ptr<TopicNode> node,
                   const std::string& prefix,
                   std::function<void(const std::string&)> print_func) const;
    
    // 通配符匹配检查
    static bool is_wildcard_match(const std::vector<std::string>& filter_tokens,
                          const std::vector<std::string>& topic_tokens);
#ifdef WITH_SHARED_SUBSCRIPTIONS
public:
    // 共享订阅分发策略
    enum ShareDistributionStrategy {
        ROUND_ROBIN,    // 轮询
        RANDOM,         // 随机
        STICKY,         // 粘性（相同发布者到相同订阅者）
    };
    // 设置共享订阅分发策略
    void set_share_distribution_strategy(ShareDistributionStrategy strategy) {
        share_strategy_ = strategy;
    }
private:
    // 共享订阅分发策略
    ShareDistributionStrategy share_strategy_ = ShareDistributionStrategy::ROUND_ROBIN;
    
    // 共享订阅轮询索引
    std::unordered_map<std::string, size_t> share_round_robin_index_;

    // 获取下一个共享订阅者（轮询策略）
    MatchResult get_next_shared_subscriber_round_robin(
        const std::vector<SubscriptionInfo>& subscribers,
        const std::string& share_key);
    
    // 获取下一个共享订阅者（随机策略）
    MatchResult get_next_shared_subscriber_random(
        const std::vector<SubscriptionInfo>& subscribers) const;
    
    // 生成共享订阅键
    std::string make_share_key(const std::string& topic, const std::string& share_name) const;
#endif
};

#endif // __MQTT_TOPIC_TREE_H__