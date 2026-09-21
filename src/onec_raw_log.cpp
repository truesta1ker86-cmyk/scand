#include "onec_raw_log.hpp"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
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

OnecRawLog& OnecRawLog::instance() {
    static OnecRawLog inst;
    return inst;
}

void OnecRawLog::add(const std::string& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string line = "[" + now_iso8601() + "] " + entry;
    entries_.push_back(line);
    while (entries_.size() > MAX_SIZE) entries_.pop_front();
    std::cout << "[1C-RAW] " << line << std::endl;
}

std::string OnecRawLog::dump() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string out;
    for (const auto& e : entries_) {
        out += e;
        out += "\n";
    }
    return out;
}

std::string OnecRawLog::tail(size_t n) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string out;
    size_t start = entries_.size() > n ? entries_.size() - n : 0;
    for (size_t i = start; i < entries_.size(); ++i) {
        out += entries_[i];
        out += "\n";
    }
    return out;
}

void OnecRawLog::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}
