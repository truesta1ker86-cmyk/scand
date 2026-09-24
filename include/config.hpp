#pragma once
#include <string>

struct Config {
    // DB
    std::string db_conn_str;

    // Server
    std::string listen_address = "0.0.0.0";
    unsigned short listen_port = 8090;

    // Worker pool
    int worker_threads   = 4;
    int task_queue_max   = 1000;
    int shutdown_timeout_sec = 30;

    // Notify
    int notify_every_percent   = 5;
    int notify_min_interval_sec = 2;

    // Ozon
    std::string ozon_client_id;
    std::string ozon_api_key;
    int         ozon_page_size  = 1000;
    int         ozon_timeout_ms = 30000;
    std::string ozon_webhook_url;

    // 1C
    std::string onec_base_url;
    std::string onec_user;
    std::string onec_password;
    int         onec_page_size        = 1000;
    int         onec_timeout_ms       = 10000;
    bool        onec_allow_insecure_http = false;

    // NEW: разрешить 1С в частной сети (192.168/10/172.16-31/link-local)
    bool        onec_allow_private_network = false;

    static Config load(const std::string& path);
};

void set_global_config(const Config& cfg);
const Config& global_config();