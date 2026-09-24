#include "sse_broker.hpp"

#include <boost/asio/write.hpp>
#include <algorithm>
#include <iostream>
#include <vector>

// ===========================================================================
// SseSubscriber
// ===========================================================================
SseSubscriber::SseSubscriber(tcp::socket socket, std::shared_ptr<void> owner)
    : socket_(std::move(socket))
    , owner_(std::move(owner)) {}

void SseSubscriber::send(std::shared_ptr<std::string> data) {
    if (!data || data->empty()) return;

    bool start_write = false;
    {
        std::lock_guard<std::mutex> lk(write_mutex_);
        if (!open_.load(std::memory_order_acquire)) return;
        write_buf_.append(*data);
        if (!writing_) {
            writing_ = true;
            start_write = true;
        }
    }
    if (start_write) do_write();
}

void SseSubscriber::close() {
    std::lock_guard<std::mutex> lk(write_mutex_);
    if (!open_.load(std::memory_order_acquire)) return;
    open_.store(false, std::memory_order_release);

    boost::system::error_code ec;
    socket_.shutdown(tcp::socket::shutdown_both, ec);
    if (ec) socket_.close(ec);
}

void SseSubscriber::do_write() {
    auto self = shared_from_this();

    std::shared_ptr<std::string> chunk;
    {
        std::lock_guard<std::mutex> lk(write_mutex_);
        if (write_buf_.empty()) {
            writing_ = false;
            return;
        }
        chunk = std::make_shared<std::string>(std::move(write_buf_));
        write_buf_.clear();
    }

    boost::asio::async_write(
        socket_,
        boost::asio::buffer(*chunk),
        [self, chunk](boost::system::error_code ec, std::size_t) {
            bool has_more = false;
            {
                std::lock_guard<std::mutex> lk(self->write_mutex_);

                self->last_write_rep_.store(
                    std::chrono::steady_clock::now().time_since_epoch().count(),
                    std::memory_order_relaxed);

                if (ec) {
                    self->open_.store(false, std::memory_order_release);
                    self->writing_ = false;
                    boost::system::error_code ignored;
                    self->socket_.close(ignored);
                    return;
                }
                if (!self->write_buf_.empty()) has_more = true;
                else                           self->writing_ = false;
            }
            if (has_more) self->do_write();
        });
}

// ===========================================================================
// SseBroker
// ===========================================================================
SseBroker& SseBroker::instance() {
    static SseBroker broker;
    return broker;
}

void SseBroker::add(std::shared_ptr<SseSubscriber> sub) {
    if (!sub) return;
    std::lock_guard<std::mutex> lk(mutex_);
    subs_.insert(std::move(sub));
}

void SseBroker::remove(std::shared_ptr<SseSubscriber> sub) {
    if (!sub) return;
    std::lock_guard<std::mutex> lk(mutex_);
    subs_.erase(sub);
}

// ---------------------------------------------------------------------------
// broadcast — вызывает broadcast_with_id с event_id = 0 (авто)
// ---------------------------------------------------------------------------
void SseBroker::broadcast(const std::string& event_name,
                          const json::value& data) {
    broadcast_with_id(event_name, data, /*event_id=*/0);
}

// ---------------------------------------------------------------------------
// broadcast_with_id — рассылка с номером события + сохранение в history_
// ---------------------------------------------------------------------------
void SseBroker::broadcast_with_id(const std::string& event_name,
                                  const json::value& data,
                                  long long event_id) {
    std::string payload;
    long long   id = event_id;

    {
        std::lock_guard<std::mutex> lk(mutex_);

        if (id == 0) id = next_id_++;

        payload  = "event: " + event_name + "\n";
        payload += "id: "    + std::to_string(id) + "\n";
        payload += "data: "  + json::serialize(data) + "\n\n";

        HistoryEntry entry;
        entry.id         = id;
        entry.event_name = event_name;
        entry.payload    = payload;

        history_.push_back(std::move(entry));
        while (history_.size() > HISTORY_MAX) {
            history_.pop_front();
        }
    }

    auto payload_ptr = std::make_shared<std::string>(std::move(payload));

    std::vector<std::shared_ptr<SseSubscriber>> snapshot;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        snapshot.assign(subs_.begin(), subs_.end());
    }
    for (auto& s : snapshot) {
        if (!s) continue;
        try { s->send(payload_ptr); }
        catch (...) { /* не роняем broadcast */ }
    }
}

// ---------------------------------------------------------------------------
// replay_to — досыпает конкретному подписчику события с id > last_event_id
// ---------------------------------------------------------------------------
void SseBroker::replay_to(std::shared_ptr<SseSubscriber> sub,
                          long long last_event_id) {
    if (!sub || last_event_id <= 0) return;

    std::vector<std::string> missed;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto& e : history_) {
            if (e.id > last_event_id) {
                missed.push_back(e.payload);
            }
        }
    }
    for (const auto& p : missed) {
        try { sub->send(std::make_shared<std::string>(p)); }
        catch (...) {}
    }
}

// ---------------------------------------------------------------------------
// count
// ---------------------------------------------------------------------------
size_t SseBroker::count() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return subs_.size();
}

// ---------------------------------------------------------------------------
// close_all
// ---------------------------------------------------------------------------
void SseBroker::close_all() {
    std::vector<std::shared_ptr<SseSubscriber>> snapshot;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        snapshot.assign(subs_.begin(), subs_.end());
        subs_.clear();
    }
    std::cout << "[SSE] close_all: closing "
              << snapshot.size() << " subscriber(s)" << std::endl;
    for (auto& s : snapshot) {
        if (!s) continue;
        try { s->close(); } catch (...) {}
    }
}
