#pragma once
#include <string>

struct Config {
    // [server]
    std::string    listen_address = "0.0.0.0";
    unsigned short listen_port    = 8080;

    // [onec]
    std::string onec_base_url;
    std::string onec_user;
    std::string onec_password;
    int         onec_page_size  = 100;
    int         onec_timeout_ms = 10000;
    bool        onec_allow_insecure_http = false;   // ← НОВОЕ

    // [database]
    std::string db_conn_str;

    // [worker_pool]
    int worker_threads = 2;
    int task_queue_max = 100;

    // [ozon]
    std::string ozon_client_id;
    std::string ozon_api_key;
    int         ozon_page_size  = 100;
    int         ozon_timeout_ms = 10000;
    std::string ozon_webhook_url;

    // [notify]
    int notify_every_percent    = 5;
    int notify_min_interval_sec = 2;

    static Config load(const std::string& path);
};
