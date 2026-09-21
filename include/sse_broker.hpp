#pragma once
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/json.hpp>
#include <chrono>
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
    bool is_open() const { return open_; }

    std::chrono::steady_clock::time_point last_write() const { return last_write_; }

private:
    void do_write();

    tcp::socket           socket_;
    std::shared_ptr<void> owner_;
    std::mutex            write_mutex_;
    std::string           write_buf_;
    bool                  open_    = true;
    bool                  writing_ = false;
    std::chrono::steady_clock::time_point last_write_ = std::chrono::steady_clock::now();
};

class SseBroker {
public:
    static SseBroker& instance();

    void add(std::shared_ptr<SseSubscriber> sub);
    void remove(std::shared_ptr<SseSubscriber> sub);
    void broadcast(const std::string& event_name, const json::value& data);
    size_t count() const;

private:
    SseBroker() = default;
    mutable std::mutex                       mutex_;
    std::set<std::shared_ptr<SseSubscriber>> subs_;
};
