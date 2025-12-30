// topic_tree.cpp
#include "topic_tree.h"
#include <algorithm>
#include <random>
#include <chrono>
#include <cassert>

TopicTree::TopicTree() : root_(std::make_shared<TopicNode>()) {
    root_->set_name("/");
}

std::vector<std::string> TopicTree::split_topic(const std::string& topic) {
    std::vector<std::string> tokens;
    if (topic.empty()) return tokens;
    
    std::string token;
    std::istringstream token_stream(topic);
    
    while (std::getline(token_stream, token, '/')) {
        tokens.push_back(std::move(token));
    }
    
    // 处理空层级（以/结尾的情况）
    if (!topic.empty() && topic.back() == '/') {
        tokens.push_back("");
    }
    
    return tokens;
}

bool TopicTree::is_valid_topic_level(const std::string& level, bool allow_wildcard) {
    if (level.empty()) return true; // 空层级是允许的
    
    // 检查通配符
    if (allow_wildcard) {
        if (level == "+" || level == "#") {
            return true;
        }
    }
    
    // 检查非法字符
    if (level.find_first_of("#+") != std::string::npos) {
        return false; // 非通配符位置包含通配符
    }
    
    // 检查长度限制（MQTT协议没有明确限制，但通常实现会限制）
    if (level.length() > 65535) { // UTF-8编码的最大长度
        return false;
    }
    
    return true;
}

bool TopicTree::is_valid_topic(const std::string& topic, bool allow_wildcard) {
    if (topic.empty()) return false;
    
    auto tokens = split_topic(topic);
    
    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto& token = tokens[i];
        
        if (!is_valid_topic_level(token, allow_wildcard)) {
            return false;
        }
        
        // 多层通配符#必须出现在最后
        if (token == "#" && i != tokens.size() - 1) {
            return false;
        }
        
        // 共享订阅格式检查：$share/{ShareName}/{filter}
        if (i == 0 && token == "$share") {
            if (tokens.size() < 3) return false;
            if (tokens[1].empty()) return false; // ShareName不能为空
            // 共享订阅的第三部分开始才是真正的主题过滤器
            // 这里不进一步验证，由调用者处理
            break; // 剩下的部分由正常的主题过滤器验证
        }
    }
    
    return true;
}

bool TopicTree::is_valid_topic_filter(const std::string& topic_filter) {
    // 主题过滤器必须允许通配符
    return is_valid_topic(topic_filter, true);
}

std::shared_ptr<TopicNode> TopicTree::get_or_create_node(
    const std::vector<std::string>& tokens,
    size_t depth, bool create) {
    
    if (depth >= tokens.size()) {
        return nullptr;
    }
    
    std::shared_ptr<TopicNode> current = root_;
    
    for (size_t i = 0; i < tokens.size(); ++i) {
        const std::string& token = tokens[i];
        
        // 检查是否是多层通配符#，它必须是最后一个
        if (token == "#") {
            if (i != tokens.size() - 1) {
                return nullptr; // 无效的过滤器
            }
        }
        
        auto& children = current->children;
        auto it = children.find(token);
        if (it == children.end()) {
            if (!create) {
                return nullptr;
            }
            
            // 创建新节点
            auto new_node = std::make_shared<TopicNode>();
            new_node->set_name(token);
            children[token] = new_node;
            current = new_node;
        } else {
            current = it->second;
        }
    }
    
    return current;
}

bool TopicTree::subscribe(const std::string& topic_filter,
                         std::shared_ptr<MqttSession> session,
                         uint8_t qos,
                         bool is_shared,
                         const std::string& share_name) {
    
    if (!is_valid_topic_filter(topic_filter)) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    auto tokens = split_topic(topic_filter);
    auto node = get_or_create_node(tokens, 0);
    if (!node) {
        return false;
    }
    
    // 检查是否已经订阅
    for (const auto& sub : node->subscribers) {
        if (auto s = sub.session.lock()) {
            if (s.get() == session.get() && 
                sub.is_shared == is_shared &&
                sub.share_name == share_name) {
                // 已存在相同订阅，更新QoS
                // 注意：这里需要额外的逻辑来更新现有订阅的QoS
                // 简化实现：先移除再添加
                node->remove_subscriber(session, share_name);
                break;
            }
        }
    }
    
    node->add_subscriber(session, qos, is_shared, share_name);
    return true;
}

bool TopicTree::unsubscribe(const std::string& topic_filter,
                           std::shared_ptr<MqttSession> session,
                           const std::string& share_name) {    
    if (!is_valid_topic_filter(topic_filter)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);    
    auto tokens = split_topic(topic_filter);
    auto node = get_node(tokens, 0);
    if (!node) {
        return false;
    }
    
    node->remove_subscriber(session, share_name);
    
    // 清理空节点（可选优化）
    // 注意：这里需要递归清理，但考虑到性能，可以在后台定期清理
    
    return true;
}

void TopicTree::collect_subscribers(std::shared_ptr<TopicNode> node,
                                   const std::vector<std::string>& tokens,
                                   size_t depth,
                                   std::vector<MatchResult>& results,
                                   bool for_publish) const {
    
    if (!node) return;
    
    // 如果到达主题末尾
    if (depth >= tokens.size()) {
        // 添加当前节点的所有订阅者（除了共享订阅，如果是发布的话）
        for (const auto& sub : node->subscribers) {
            if (!for_publish || !sub.is_shared) {
                results.emplace_back(sub.session, sub.qos, sub.is_shared, sub.share_name);
            }
        }
        
        // 检查多层通配符子节点
        auto it = node->children.find("#");
        if (it != node->children.end()) {
            for (const auto& sub : it->second->subscribers) {
                if (!for_publish || !sub.is_shared) {
                    results.emplace_back(sub.session, sub.qos, sub.is_shared, sub.share_name);
                }
            }
        }
        return;
    }
    
    const std::string& current_token = tokens[depth];
    
    // 1. 精确匹配
    auto exact_it = node->children.find(current_token);
    if (exact_it != node->children.end()) {
        collect_subscribers(exact_it->second, tokens, depth + 1, results, for_publish);
    }
    
    // 2. 单层通配符+
    auto plus_it = node->children.find("+");
    if (plus_it != node->children.end()) {
        collect_subscribers(plus_it->second, tokens, depth + 1, results, for_publish);
    }
    
    // 3. 多层通配符#（如果存在，它匹配当前层级及所有后续层级）
    auto hash_it = node->children.find("#");
    if (hash_it != node->children.end()) {
        for (const auto& sub : hash_it->second->subscribers) {
            if (!for_publish || !sub.is_shared) {
                results.emplace_back(sub.session, sub.qos, sub.is_shared, sub.share_name);
            }
        }
    }
}

std::vector<MatchResult> TopicTree::get_subscribers(const std::string& topic, bool include_retained) {
    std::vector<MatchResult> results;
    if (!is_valid_topic(topic, false)) {
        return results;
    }
    
    auto tokens = split_topic(topic);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        collect_subscribers(root_, tokens, 0, results, true);
    }
    
    // 去重（同一个会话可能通过不同的通配符匹配到多次）
    std::set<std::shared_ptr<MqttSession>> seen_sessions;
    std::vector<MatchResult> unique_results;
    
    for (auto& result : results) {
        if (auto session = result.session.lock()) {
            if (seen_sessions.find(session) == seen_sessions.end()) {
                seen_sessions.insert(session);
                unique_results.push_back(result);
            }
        }
    }
    
    // 清理过期订阅（可选）
    auto it = unique_results.begin();
    while (it != unique_results.end()) {
        if (it->session.expired()) {
            it = unique_results.erase(it);
        } else {
            ++it;
        }
    }
    
    return unique_results;
}

#ifdef WITH_SHARED_SUBSCRIPTIONS
std::string TopicTree::make_share_key(const std::string& topic, 
                                     const std::string& share_name) const {
    return share_name + "|" + topic;
}

std::vector<MatchResult> TopicTree::get_shared_subscribers(const std::string& topic, const std::string& share_name) {
    std::vector<MatchResult> results;
    if (!is_valid_topic(topic, false) || share_name.empty()) {
        return results;
    }
    
    // 构造共享订阅的过滤器格式：$share/{ShareName}/{topic}
    std::string share_filter = "$share/" + share_name + "/" + topic;
    auto tokens = split_topic(share_filter);
    
    std::lock_guard<std::mutex> lock(mutex_);    
    // 找到共享订阅节点
    auto node = get_node(tokens, 0);
    if (!node) {
        return results;
    }
    
    // 根据策略选择一个订阅者
    if (node->subscribers.empty()) {
        return results;
    }
    
    std::vector<SubscriptionInfo> active_subscribers;
    for (const auto& sub : node->subscribers) {
        if (!sub.session.expired()) {
            active_subscribers.push_back(sub);
        }
    }
    
    if (active_subscribers.empty()) {
        return results;
    }
    
    MatchResult selected(std::weak_ptr<MqttSession>(), 0, true, share_name);
    switch (share_strategy_) {
        case ShareDistributionStrategy::ROUND_ROBIN:
            selected = get_next_shared_subscriber_round_robin(active_subscribers, make_share_key(topic, share_name));
            break;
        case ShareDistributionStrategy::RANDOM:
            selected = get_next_shared_subscriber_random(active_subscribers);
            break;
        case ShareDistributionStrategy::STICKY:
            // 粘性策略需要跟踪发布者-订阅者映射
            // 简化实现：使用轮询
            selected = get_next_shared_subscriber_round_robin(active_subscribers, make_share_key(topic, share_name));
            break;
    }
    
    if (!selected.session.expired()) {
        results.push_back(selected);
    }
    
    return results;
}

MatchResult TopicTree::get_next_shared_subscriber_round_robin(
    const std::vector<SubscriptionInfo>& subscribers,
    const std::string& share_key) {
    
    auto& index = share_round_robin_index_[share_key];
    
    // 找到下一个有效的订阅者
    for (size_t i = 0; i < subscribers.size(); ++i) {
        size_t current_index = index % subscribers.size();
        index++;
        
        const auto& sub = subscribers[current_index];
        if (!sub.session.expired()) {
            return MatchResult(sub.session, sub.qos, true, sub.share_name);
        }
    }
    
    // 没有找到有效订阅者
    return MatchResult();
}

MatchResult TopicTree::get_next_shared_subscriber_random(
    const std::vector<SubscriptionInfo>& subscribers) const {
    
    static std::random_device rd;
    static std::mt19937 gen(rd());
    
    // 收集所有有效订阅者
    std::vector<SubscriptionInfo> active_subs;
    for (const auto& sub : subscribers) {
        if (!sub.session.expired()) {
            active_subs.push_back(sub);
        }
    }
    
    if (active_subs.empty()) {
        return MatchResult();
    }
    
    std::uniform_int_distribution<> dis(0, active_subs.size() - 1);
    const auto& sub = active_subs[dis(gen)];
    
    return MatchResult(sub.session, sub.qos, true, sub.share_name);
}
#endif // WITH_SHARED_SUBSCRIPTIONS

bool TopicTree::clear_retained_message(const std::string& topic) {    
    if (!is_valid_topic(topic, false)) {
        return false;
    }
    
    auto tokens = split_topic(topic);
    std::lock_guard<std::mutex> lock(mutex_);
    auto node = get_node(tokens, 0);
    if (!node) {
        return false;
    }
    
    node->retained_message.reset();
    return true;
}

void TopicTree::set_retained_message(const std::string& topic,
                                    const std::vector<uint8_t>& payload,
                                    uint8_t qos,
                                    bool dup) {
    
    if (!is_valid_topic(topic, false)) {
        return;
    }
        
    auto tokens = split_topic(topic);
    std::lock_guard<std::mutex> lock(mutex_);
    auto node = get_or_create_node(tokens, 0);
    if (!node) {
        return;
    }

    if (payload.empty()) {
        // 空payload表示清除保留消息
        node->retained_message.reset();
    } else {
        node->retained_message.reset(new RetainedMessage(payload, qos, dup));
    }
}

void TopicTree::collect_retained_messages(std::shared_ptr<TopicNode> node,
                                         const std::vector<std::string>& tokens,
                                         size_t depth,
                                         const std::string& current_path,
                                         std::vector<std::pair<std::string, RetainedMessage>>& results) const {
    
    if (!node) return;
    
    // 检查通配符匹配
    if (depth >= tokens.size()) {
        // 到达过滤器末尾，添加当前节点的保留消息
        if (node->retained_message) {
            results.emplace_back(current_path, *node->retained_message);
        }
        return;
    }
    
    const std::string& current_token = tokens[depth];
    std::string new_path = current_path.empty() ? "" : current_path + "/";
    
    if (current_token == "+") {
        // 单层通配符：遍历所有非通配符子节点
        for (const auto& it : node->children) {
            auto child_name = it.first;
            if (child_name != "+" && child_name != "#") {
                collect_retained_messages(it.second, tokens, depth + 1,
                                         new_path + child_name, results);
            }
        }
    } else if (current_token == "#") {
        // 多层通配符：收集所有子孙节点的保留消息
        std::queue<std::pair<std::shared_ptr<TopicNode>, std::string>> queue;
        queue.emplace(node, current_path);
        
        while (!queue.empty()) {
            auto item = queue.front();
            auto current_node = item.first;
            auto path = item.second;
            queue.pop();
            
            // 添加当前节点的保留消息
            if (current_node->retained_message) {
                results.emplace_back(path.empty() ? "/" : path, *current_node->retained_message);
            }
            
            // 添加子节点到队列
            for (const auto& it : current_node->children) {
                auto child_name = it.first;
                std::string child_path = path.empty() ? child_name : path + "/" + child_name;
                queue.emplace(it.second, child_path);
            }
        }
    } else {
        // 精确匹配
        auto it = node->children.find(current_token);
        if (it != node->children.end()) {
            collect_retained_messages(it->second, tokens, depth + 1,
                                     new_path + current_token, results);
        }
        
        // 检查单层通配符子节点
        auto plus_it = node->children.find("+");
        if (plus_it != node->children.end()) {
            collect_retained_messages(plus_it->second, tokens, depth + 1,
                                     new_path + current_token, results);
        }
    }
}

std::vector<std::pair<std::string, RetainedMessage>> 
TopicTree::get_retained_messages(const std::string& topic_filter) {    
    std::vector<std::pair<std::string, RetainedMessage>> results;
    if (!is_valid_topic_filter(topic_filter)) {
        return results;
    }
    
    auto tokens = split_topic(topic_filter);
    std::lock_guard<std::mutex> lock(mutex_);
    collect_retained_messages(root_, tokens, 0, "", results);
    return results;
}

std::vector<std::string> TopicTree::get_session_subscriptions(
    std::shared_ptr<MqttSession> session) {
    
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> subscriptions;
    
    // 递归遍历树查找会话的订阅
    std::function<void(std::shared_ptr<TopicNode>, const std::string&)> dfs;
    dfs = [&](std::shared_ptr<TopicNode> node, const std::string& path) {
        if (!node) return;
        
        // 检查当前节点是否有该会话的订阅
        for (const auto& sub : node->subscribers) {
            if (auto s = sub.session.lock()) {
                if (s.get() == session.get()) {
                    subscriptions.push_back(path.empty() ? "/" : path);
                    break; // 每个节点只添加一次
                }
            }
        }
        
        // 遍历子节点
        for (const auto& it : node->children) {
            auto child_name = it.first;
            std::string child_path = path.empty() ? child_name : path + "/" + child_name;
            dfs(it.second, child_path);
        }
    };
    
    dfs(root_, "");
    return subscriptions;
}

void TopicTree::clean_expired_subscriptions() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::function<void(std::shared_ptr<TopicNode>)> dfs;
    dfs = [&](std::shared_ptr<TopicNode> node) {
        if (!node) return;
        
        node->clean_expired_subscribers();
        
        for (const auto& it : node->children) {
            dfs(it.second);
        }
    };
    
    dfs(root_);
}

TopicTree::Statistics TopicTree::get_statistics() const {    
    Statistics stats = {0, 0, 0, 0};
    std::lock_guard<std::mutex> lock(mutex_);
    collect_statistics(root_, 0, stats);
    return stats;
}

void TopicTree::collect_statistics(std::shared_ptr<TopicNode> node,
                                  size_t depth,
                                  Statistics& stats) const {
    
    if (!node) return;
    
    stats.total_nodes++;
    stats.total_subscribers += node->subscribers.size();
    stats.max_depth = std::max(stats.max_depth, depth);
    
    if (node->retained_message) {
        stats.total_retained_messages++;
    }
    
    for (const auto& it : node->children) {
        collect_statistics(it.second, depth + 1, stats);
    }
}

void TopicTree::dump_tree(std::function<void(const std::string&)> print_func) const {
    std::lock_guard<std::mutex> lock(mutex_);
    dump_node(root_, "", print_func);
}

void TopicTree::dump_node(std::shared_ptr<TopicNode> node,
                         const std::string& prefix,
                         std::function<void(const std::string&)> print_func) const {
    
    if (!node) return;
    
    std::string node_name = node->get_name();
    if (node_name.empty()) node_name = "ROOT";
    
    std::string line = prefix + "├─ " + node_name;
    
    if (!node->subscribers.empty()) {
        line += " [";
        int active_subs = 0;
        for (const auto& sub : node->subscribers) {
            if (!sub.session.expired()) {
                active_subs++;
            }
        }
        line += std::to_string(active_subs) + "/" + 
                std::to_string(node->subscribers.size()) + " subs]";
    }
    
    if (node->retained_message) {
        line += " [RETAINED: " + std::to_string(node->retained_message->payload.size()) + " bytes]";
    }
    
    print_func(line);
    
    // 遍历子节点
    int i = 0;
    for (const auto& it : node->children) {
        std::string new_prefix = prefix + (++i == node->children.size() ? "   " : "│  ");
        dump_node(it.second, new_prefix, print_func);
    }
}

bool TopicTree::is_wildcard_match(const std::vector<std::string>& filter_tokens,
                                 const std::vector<std::string>& topic_tokens) {
    
    size_t filter_len = filter_tokens.size();
    size_t topic_len = topic_tokens.size();
    
    for (size_t i = 0; i < filter_len; ++i) {
        const std::string& filter_token = filter_tokens[i];
        
        if (filter_token == "#") {
            // 多层通配符必须出现在最后
            return i == filter_len - 1;
        } else if (filter_token == "+") {
            // 单层通配符：需要确保主题有这一层
            if (i >= topic_len) {
                return false;
            }
        } else {
            // 精确匹配
            if (i >= topic_len || filter_token != topic_tokens[i]) {
                return false;
            }
        }
    }
    
    // 如果过滤器用完，主题也必须用完（除非过滤器以#结尾）
    return filter_len == topic_len || 
           (filter_len > 0 && filter_tokens[filter_len - 1] == "#");
}
