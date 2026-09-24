#pragma once
#include <pqxx/pqxx>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace scand::db {

class ConnectionPool {
public:
    ConnectionPool(const std::string& conn_str, size_t size = 4)
        : conn_str_(conn_str) {
        for (size_t i = 0; i < size; ++i) {
            pool_.push_back(std::make_shared<pqxx::connection>(conn_str_));
        }
    }

    class Lease {
    public:
        Lease() = default;
        Lease(std::shared_ptr<pqxx::connection> c, ConnectionPool* pool)
            : conn_(std::move(c)), pool_(pool) {}

        ~Lease() { if (pool_) pool_->release(conn_); }

        Lease(Lease&& o) noexcept
            : conn_(std::move(o.conn_)),
              pool_(std::exchange(o.pool_, nullptr)) {}

        Lease& operator=(Lease&& o) noexcept {
            if (this != &o) {
                if (pool_) pool_->release(conn_);
                conn_ = std::move(o.conn_);
                pool_ = std::exchange(o.pool_, nullptr);
            }
            return *this;
        }

        Lease(const Lease&)            = delete;
        Lease& operator=(const Lease&) = delete;

        pqxx::connection& get() { return *conn_; }

    private:
        std::shared_ptr<pqxx::connection> conn_;
        ConnectionPool*                   pool_ = nullptr;
    };

    Lease acquire() {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return !pool_.empty(); });
        auto c = pool_.front();
        pool_.pop_front();
        return Lease(c, this);
    }

    void release(std::shared_ptr<pqxx::connection> c) {
        if (!c) return;
        {
            std::lock_guard<std::mutex> lk(m_);
            pool_.push_back(std::move(c));
        }
        cv_.notify_one();
    }

private:
    std::string conn_str_;
    std::deque<std::shared_ptr<pqxx::connection>> pool_;
    std::mutex              m_;
    std::condition_variable cv_;
};

} // namespace scand::db
