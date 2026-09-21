#include "sse_broker.hpp"
#include <boost/asio/write.hpp>
#include <chrono>
#include <iostream>

SseSubscriber::SseSubscriber(tcp::socket socket, std::shared_ptr<void> owner)
    : socket_(std::move(socket)), owner_(std::move(owner)) {}

void SseSubscriber::send(std::shared_ptr<std::string> data) {
    if (!open_) return;

    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!open_) return;

    write_buf_ += *data;
    if (!writing_) {
        writing_ = true;
        do_write();
    }
}

void SseSubscriber::do_write() {
    auto self = shared_from_this();
    net::async_write(socket_, net::buffer(write_buf_),
        [self](beast::error_code ec, std::size_t n) {
            std::lock_guard<std::mutex> lock(self->write_mutex_);
            if (ec) {
                self->open_ = false;
                return;
            }
            self->write_buf_.erase(0, n);
            self->last_write_ = std::chrono::steady_clock::now();
            if (self->write_buf_.empty()) self->writing_ = false;
            else self->do_write();
        });
}

void SseSubscriber::close() {
    if (!open_) return;
    open_ = false;
    beast::error_code ec;
    socket_.shutdown(tcp::socket::shutdown_both, ec);
}

SseBroker& SseBroker::instance() {
    static SseBroker inst;
    return inst;
}

void SseBroker::add(std::shared_ptr<SseSubscriber> sub) {
    std::lock_guard<std::mutex> lock(mutex_);
    subs_.insert(sub);
}

void SseBroker::remove(std::shared_ptr<SseSubscriber> sub) {
    std::lock_guard<std::mutex> lock(mutex_);
    subs_.erase(sub);
}

void SseBroker::broadcast(const std::string& event_name, const json::value& data) {
    auto msg = std::make_shared<std::string>();
    if (!event_name.empty()) *msg += "event: " + event_name + "\n";
    *msg += "data: " + json::serialize(data) + "\n\n";

    auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = subs_.begin(); it != subs_.end(); ) {
        auto& s = *it;

        if (!s->is_open()) {
            it = subs_.erase(it);
            continue;
        }

        auto age = std::chrono::duration_cast<std::chrono::seconds>(
            now - s->last_write()).count();
        if (age > 60) {
            s->close();
            it = subs_.erase(it);
            continue;
        }

        s->send(msg);
        ++it;
    }
}

size_t SseBroker::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return subs_.size();
}
