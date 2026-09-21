#include "http_server.hpp"
#include "router.hpp"
#include "sse_broker.hpp"
#include "webhook_store.hpp"
#include <boost/beast.hpp>
#include <boost/json.hpp>
#include <chrono>
#include <iostream>
#include <thread>

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
namespace json  = boost::json;
using tcp = net::ip::tcp;

// ---------------------------------------------------------------------------
// Session — одно HTTP-соединение
// ---------------------------------------------------------------------------
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
        // --- CORS preflight ---
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

        // --- SSE-эндпоинт ---
        if (request_.method() == http::verb::get
            && request_.target() == "/events/subscribe")
        {
            handle_sse();
            return;
        }

        // --- Обычная обработка через роутер ---
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

    // -----------------------------------------------------------------------
    // SSE: отдаём заголовки вручную (без Content-Length)
    // -----------------------------------------------------------------------
    void handle_sse() {
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
            [self, headers](beast::error_code ec, std::size_t) {
                if (ec) {
                    std::cerr << "[SSE] headers write error: "
                              << ec.message() << std::endl;
                    return;
                }

                auto sub = std::make_shared<SseSubscriber>(
                    std::move(self->socket_),
                    self
                );
                SseBroker::instance().add(sub);

                std::cout << "[SSE] client connected (total "
                          << SseBroker::instance().count() << ")" << std::endl;

                // Начальное состояние
                {
                    json::object init_body{
                        {"items", WebhookStore::instance().last(200)}
                    };
                    auto msg = std::make_shared<std::string>();
                    *msg += "event: init\n";
                    *msg += "data: " + json::serialize(init_body) + "\n\n";
                    sub->send(msg);
                }

                // Если сервис только что перезапустился и есть pending-сообщение
                // о возобновлении — отправляем ТОЛЬКО ЭТОМУ клиенту.
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

                // Keep-alive каждые 15 секунд
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

// ---------------------------------------------------------------------------
// Listener
// ---------------------------------------------------------------------------
class HttpServer::Listener : public std::enable_shared_from_this<Listener> {
    tcp::acceptor acceptor_;
    const Router& router_;
    SyncService&  sync_;

public:
    Listener(net::io_context& ioc, tcp::endpoint endpoint,
             const Router& router, SyncService& sync)
        : acceptor_(ioc, endpoint), router_(router), sync_(sync) {}

    void run() {
        beast::error_code ec;
        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        do_accept();
    }

private:
    void do_accept() {
        auto self = shared_from_this();
        acceptor_.async_accept(
            [self](beast::error_code ec, tcp::socket socket) {
                if (!ec) {
                    std::make_shared<HttpSession>(std::move(socket),
                                                  self->router_,
                                                  self->sync_)->run();
                }
                self->do_accept();
            });
    }
};

// ---------------------------------------------------------------------------
// HttpServer
// ---------------------------------------------------------------------------
HttpServer::HttpServer(net::io_context& ioc,
                       const std::string& address,
                       unsigned short port,
                       const Router& router,
                       SyncService& sync)
    : listener_(std::make_shared<Listener>(
          ioc,
          tcp::endpoint{net::ip::make_address(address), port},
          router,
          sync)) {}

void HttpServer::start() {
    listener_->run();
}
