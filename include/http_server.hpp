#pragma once
#include "router.hpp"
#include "sync_service.hpp"
#include <boost/asio.hpp>
#include <memory>
#include <string>

class HttpServer {
public:
    HttpServer(boost::asio::io_context& ioc,
               const std::string& address,
               unsigned short port,
               const Router& router,
               SyncService& sync);

    void start();

private:
    class Listener;
    std::shared_ptr<Listener> listener_;
};
