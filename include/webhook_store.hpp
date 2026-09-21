#pragma once
#include <boost/json.hpp>
#include <deque>
#include <mutex>
#include <string>

namespace json = boost::json;

struct WebhookEntry {
    long long   id = 0;
    std::string received_at;
    std::string source;
    std::string message_type;
    std::string body;
    std::string remote_ip;
};

class WebhookStore {
public:
    static WebhookStore& instance();

    void add(WebhookEntry entry);
    json::array last(size_t limit = 100) const;
    void clear();

private:
    WebhookStore() = default;
    mutable std::mutex       mutex_;
    std::deque<WebhookEntry> entries_;
    long long                next_id_ = 1;
    static constexpr size_t  MAX_SIZE = 500;
};
