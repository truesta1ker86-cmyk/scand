#pragma once
#include "router.hpp"
#include "sync_service.hpp"
#include <boost/asio.hpp>
#include <memory>
#include <string>

namespace net = boost::asio;
using tcp = net::ip::tcp;

class HttpServer {
public:
    HttpServer(net::io_context& ioc,
               const std::string& address,
               unsigned short port,
               const Router& router,
               SyncService& sync);

    void start();
    void stop_accepting();
    void stop();

private:
    class Listener;
    std::shared_ptr<Listener> listener_;
};
