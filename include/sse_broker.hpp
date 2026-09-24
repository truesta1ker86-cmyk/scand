#pragma once
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/json.hpp>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace net   = boost::asio;
namespace beast = boost::beast;
namespace json  = boost::json;
using tcp = net::ip::tcp;

class SseSubscriber : public std::enable_shared_from_this<SseSubscriber> {
public:
    SseSubscriber(tcp::socket socket, std::shared_ptr<void> owner);

    void send(std::shared_ptr<std::string> data);
    void close();

    // Атомарный доступ — устранён data race.
    bool is_open() const { return open_.load(std::memory_order_acquire); }

    std::chrono::steady_clock::time_point last_write() const {
        auto rep = last_write_rep_.load(std::memory_order_relaxed);
        return std::chrono::steady_clock::time_point{
            std::chrono::steady_clock::duration{rep}};
    }

private:
    void do_write();

    tcp::socket           socket_;
    std::shared_ptr<void> owner_;
    std::mutex            write_mutex_;
    std::string           write_buf_;
    std::atomic<bool>     open_{true};
    bool                  writing_ = false;
    std::atomic<std::chrono::steady_clock::rep> last_write_rep_{
        std::chrono::steady_clock::now().time_since_epoch().count()
    };
};

class SseBroker {
public:
    static SseBroker& instance();

    void add(std::shared_ptr<SseSubscriber> sub);
    void remove(std::shared_ptr<SseSubscriber> sub);

    // Раскладка события (авто-id).
    void broadcast(const std::string& event_name, const json::value& data);

    // Раскладка события с явным id.
    void broadcast_with_id(const std::string& event_name,
                           const json::value& data,
                           long long event_id);

    // Досыпать конкретному подписчику события с id > last_event_id.
    void replay_to(std::shared_ptr<SseSubscriber> sub, long long last_event_id);

    size_t count() const;

    void close_all();

private:
    SseBroker() = default;

    struct HistoryEntry {
        long long   id;
        std::string event_name;
        std::string payload;
    };

    static constexpr std::size_t HISTORY_MAX = 500;

    mutable std::mutex                       mutex_;
    std::set<std::shared_ptr<SseSubscriber>> subs_;

    // История последних событий для replay по Last-Event-ID.
    std::deque<HistoryEntry> history_;
    long long                next_id_ = 1;
};