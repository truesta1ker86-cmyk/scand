#include "config.hpp"
#include "ini.h"
#include <stdexcept>
#include <cstring>

namespace {

int config_ini_handler(void* user, const char* section,
                       const char* name, const char* value) {
    auto* cfg = static_cast<Config*>(user);

    auto is = [&](const char* s) { return std::strcmp(section, s) == 0; };
    auto in = [&](const char* n) { return std::strcmp(name, n) == 0; };

    try {
        if (is("server")) {
            if (in("address"))                    cfg->listen_address = value;
            else if (in("port"))                  cfg->listen_port =
                                                      static_cast<unsigned short>(std::stoi(value));
            else if (in("shutdown_timeout_sec"))  cfg->shutdown_timeout_sec =
                                                      std::stoi(value);
        }
        else if (is("onec")) {
            if (in("base_url"))        cfg->onec_base_url = value;
            else if (in("user"))       cfg->onec_user = value;
            else if (in("password"))   cfg->onec_password = value;
            else if (in("page_size"))  cfg->onec_page_size = std::stoi(value);
            else if (in("timeout_ms")) cfg->onec_timeout_ms = std::stoi(value);
            else if (in("allow_insecure_http"))
                cfg->onec_allow_insecure_http =
                    (std::string(value) == "true" || std::string(value) == "1");

            // NEW: разрешить 1С-адрес в частной сети
            // (когда сервис и 1С находятся в одном контуре)
            else if (in("allow_private_network"))
                cfg->onec_allow_private_network =
                    (std::string(value) == "true" || std::string(value) == "1");
        }
        else if (is("database")) {
            if (in("conn_str")) cfg->db_conn_str = value;
        }
        else if (is("worker_pool")) {
            if (in("threads"))        cfg->worker_threads = std::stoi(value);
            else if (in("queue_max")) cfg->task_queue_max = std::stoi(value);
        }
        else if (is("ozon")) {
            if (in("client_id"))        cfg->ozon_client_id = value;
            else if (in("api_key"))     cfg->ozon_api_key = value;
            else if (in("page_size"))   cfg->ozon_page_size = std::stoi(value);
            else if (in("timeout_ms"))  cfg->ozon_timeout_ms = std::stoi(value);
            else if (in("webhook_url")) cfg->ozon_webhook_url = value;
        }
        else if (is("notify")) {
            if (in("every_percent"))         cfg->notify_every_percent = std::stoi(value);
            else if (in("min_interval_sec")) cfg->notify_min_interval_sec = std::stoi(value);
        }
    } catch (const std::exception&) {
        return 0;
    }
    return 1;
}

} // namespace

Config Config::load(const std::string& path) {
    Config cfg;

    int rc = ini_parse(path.c_str(), config_ini_handler, &cfg);
    if (rc < 0) throw std::runtime_error("Cannot open config file: " + path);
    if (rc > 0) throw std::runtime_error("Config parse error in " + path
                                         + " at line " + std::to_string(rc));

    if (cfg.onec_base_url.empty())
        throw std::runtime_error("Config: [onec] base_url is required");
    if (cfg.db_conn_str.empty())
        throw std::runtime_error("Config: [database] conn_str is required");
    if (cfg.worker_threads <= 0)
        throw std::runtime_error("Config: [worker_pool] threads must be > 0");
    if (cfg.task_queue_max <= 0)
        throw std::runtime_error("Config: [worker_pool] queue_max must be > 0");
    if (cfg.shutdown_timeout_sec <= 0)
        throw std::runtime_error("Config: [server] shutdown_timeout_sec must be > 0");

    return cfg;
}


namespace {
    const Config* g_config = nullptr;
}

void set_global_config(const Config& cfg) {
    g_config = &cfg;
}

const Config& global_config() {
    if (g_config == nullptr) {
        throw std::runtime_error(
            "global_config(): config не инициализирован");
    }
    return *g_config;
}
