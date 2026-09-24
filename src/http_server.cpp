#include "http_server.hpp"
#include "router.hpp"
#include "sse_broker.hpp"
#include "webhook_store.hpp"
#include <boost/beast.hpp>
#include <boost/json.hpp>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
namespace json  = boost::json;
using tcp = net::ip::tcp;

class HttpSession : public std::enable_shared_from_this<HttpSession> {
    tcp::socket socket_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> request_;
    const Router& router_;
    SyncService&  sync_;

public:
    HttpSession(tcp::socket socket, const Router& router, SyncService& sync)
        : socket_(std::move(socket)), router_(router), sync_(sync) {}

    void run() { do_read(); }

private:
    void do_read() {
        auto self = shared_from_this();
        http::async_read(socket_, buffer_, request_,
            [self](beast::error_code ec, std::size_t) {
                if (ec) return;
                self->handle_request();
            });
    }

    void handle_request() {
        if (request_.method() == http::verb::options) {
            auto res = std::make_shared<http::response<http::empty_body>>();
            res->version(request_.version());
            res->result(http::status::no_content);
            res->set(http::field::server, "1C-Sync-Service");
            res->set("Access-Control-Allow-Origin", "*");
            res->set("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
            res->set("Access-Control-Allow-Headers", "Content-Type");
            res->prepare_payload();

            auto self = shared_from_this();
            http::async_write(socket_, *res,
                [self, res](beast::error_code ec, std::size_t) {
                    if (!ec && self->request_.keep_alive()) self->do_read();
                });
            return;
        }

        if (request_.method() == http::verb::get
            && request_.target() == "/events/subscribe") {
            handle_sse();
            return;
        }

        Response r = router_.dispatch(request_.method(),
                                      std::string(request_.target()),
                                      request_);

        auto res = std::make_shared<http::response<http::string_body>>();
        res->version(request_.version());
        res->result(r.status);
        res->set(http::field::server, "1C-Sync-Service");
        res->set(http::field::content_type, "application/json");
        res->set("Access-Control-Allow-Origin", "*");
        res->set("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
        res->set("Access-Control-Allow-Headers", "Content-Type");
        res->keep_alive(request_.keep_alive());

        if (!r.raw_body.empty()) {
            res->set(http::field::content_type, r.raw_content_type);
            res->body() = std::move(r.raw_body);
        } else {
            res->body() = json::serialize(r.body);
        }
        res->prepare_payload();

        auto self = shared_from_this();
        http::async_write(socket_, *res,
            [self, res](beast::error_code ec, std::size_t) {
                if (ec) return;
                if (self->request_.keep_alive()) self->do_read();
            });
    }

    // =========================================================================
    // SSE: подключение
    // =========================================================================
    void handle_sse() {
        long long last_event_id = 0;
        {
            auto it = request_.find("Last-Event-ID");
            if (it != request_.end()) {
                try {
                    last_event_id = std::stoll(std::string(it->value()));
                } catch (...) {
                    last_event_id = 0;
                }
            }
        }

        auto headers = std::make_shared<std::string>();
        *headers += "HTTP/1.1 200 OK\r\n";
        *headers += "Server: 1C-Sync-Service\r\n";
        *headers += "Content-Type: text/event-stream; charset=utf-8\r\n";
        *headers += "Cache-Control: no-cache, no-transform\r\n";
        *headers += "Connection: keep-alive\r\n";
        *headers += "X-Accel-Buffering: no\r\n";
        *headers += "Access-Control-Allow-Origin: *\r\n";
        *headers += "\r\n";

        auto self = shared_from_this();
        net::async_write(socket_, net::buffer(*headers),
            [self, headers, last_event_id](beast::error_code ec, std::size_t) {
                if (ec) {
                    std::cerr << "[SSE] headers write error: "
                              << ec.message() << std::endl;
                    return;
                }

                auto sub = std::make_shared<SseSubscriber>(
                    std::move(self->socket_), self);
                SseBroker::instance().add(sub);

                std::cout << "[SSE] client connected (total "
                          << SseBroker::instance().count()
                          << ", last_event_id=" << last_event_id << ")"
                          << std::endl;

                if (last_event_id > 0) {
                    SseBroker::instance().replay_to(sub, last_event_id);
                }

                {
                    json::object init_body{
                        {"items", WebhookStore::instance().last(200)}
                    };
                    auto msg = std::make_shared<std::string>();
                    *msg += "event: init\n";
                    *msg += "data: " + json::serialize(init_body) + "\n\n";
                    sub->send(msg);
                }

                if (auto pending = self->sync_.take_pending_resume()) {
                    json::object body{
                        {"source",    pending->source},
                        {"processed", pending->processed},
                        {"total",     pending->total},
                        {"percent",   pending->percent},
                        {"status",    pending->status},
                        {"message",   "Незавершённая синхронизация будет продолжена"}
                    };
                    auto msg = std::make_shared<std::string>();
                    *msg += "event: resumed\n";
                    *msg += "data: " + json::serialize(body) + "\n\n";
                    sub->send(msg);
                    std::cout << "[SSE] Sent pending resumed to client" << std::endl;
                }

                std::thread([sub]() {
                    while (sub->is_open()) {
                        std::this_thread::sleep_for(std::chrono::seconds(15));
                        if (!sub->is_open()) break;
                        auto ka = std::make_shared<std::string>(": keep-alive\n\n");
                        sub->send(ka);
                    }
                    SseBroker::instance().remove(sub);
                    std::cout << "[SSE] client disconnected (total "
                              << SseBroker::instance().count() << ")" << std::endl;
                }).detach();
            });
    }
};

class HttpServer::Listener : public std::enable_shared_from_this<Listener> {
    tcp::acceptor acceptor_;
    const Router& router_;
    SyncService&  sync_;
    std::atomic<bool> stopping_{false};

public:
    Listener(net::io_context& ioc, tcp::endpoint endpoint,
             const Router& router, SyncService& sync)
        : acceptor_(ioc, endpoint), router_(router), sync_(sync) {}

    void run() {
        beast::error_code ec;
        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        do_accept();
    }

    void stop_accepting() {
        stopping_.store(true);
        beast::error_code ec;
        acceptor_.close(ec);
    }

private:
    void do_accept() {
        if (stopping_.load()) return;
        auto self = shared_from_this();
        acceptor_.async_accept(
            [self](beast::error_code ec, tcp::socket socket) {
                if (ec == net::error::operation_aborted) return;
                if (ec) {
                    std::cerr << "[HTTP] accept error: " << ec.message() << "\n";
                    return;
                }
                std::make_shared<HttpSession>(std::move(socket),
                                              self->router_,
                                              self->sync_)->run();
                if (!self->stopping_.load()) self->do_accept();
            });
    }
};

HttpServer::HttpServer(net::io_context& ioc,
                       const std::string& address,
                       unsigned short port,
                       const Router& router,
                       SyncService& sync)
    : listener_(std::make_shared<Listener>(
          ioc, tcp::endpoint{net::ip::make_address(address), port},
          router, sync)) {}

void HttpServer::start() { listener_->run(); }

void HttpServer::stop_accepting() {
    std::cout << "[HTTP] stop_accepting\n";
    listener_->stop_accepting();
}

void HttpServer::stop() {
    stop_accepting();
    SseBroker::instance().close_all();
}
