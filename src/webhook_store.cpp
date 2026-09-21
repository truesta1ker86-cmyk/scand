#include "webhook_store.hpp"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace {

std::string now_iso8601() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

} // namespace

WebhookStore& WebhookStore::instance() {
    static WebhookStore inst;
    return inst;
}

void WebhookStore::add(WebhookEntry entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    entry.id          = next_id_++;
    entry.received_at = now_iso8601();
    entries_.push_front(std::move(entry));
    while (entries_.size() > MAX_SIZE) entries_.pop_back();
}

json::array WebhookStore::last(size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    json::array out;
    size_t count = std::min(limit, entries_.size());
    for (size_t i = 0; i < count; ++i) {
        const auto& e = entries_[i];
        out.push_back(json::object{
            {"id",           e.id},
            {"received_at",  e.received_at},
            {"source",       e.source},
            {"message_type", e.message_type},
            {"body",         e.body},
            {"remote_ip",    e.remote_ip}
        });
    }
    return out;
}

void WebhookStore::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}
