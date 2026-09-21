#include "config.hpp"
#include "db_repository.hpp"
#include "handlers.hpp"
#include "hooks_handlers.hpp"
#include "http_server.hpp"
#include "notifier.hpp"
#include "onec_catalog.hpp"
#include "onec_catalog_sync.hpp"
#include "onec_client.hpp"
#include "onec_product_mapper.hpp"
#include "onec_sync_repository.hpp"
#include "ozon_client.hpp"
#include "router.hpp"
#include "sync_service.hpp"
#include "worker_pool.hpp"

#include <boost/asio.hpp>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>
#include <vector>

namespace net = boost::asio;

int main(int argc, char* argv[]) {
    try {
        // -------------------------------------------------------------------
        // Конфиг
        // -------------------------------------------------------------------
        std::string config_path = (argc > 1) ? argv[1] : "config.ini";
        Config cfg = Config::load(config_path);

        std::cout << "Config loaded from " << config_path << std::endl;

        // -------------------------------------------------------------------
        // Notifier
        // -------------------------------------------------------------------
        auto notifier = std::make_shared<CompositeNotifier>();
        notifier->add(std::make_shared<LogNotifier>());
        notifier->add(std::make_shared<SseNotifier>());

        if (!cfg.ozon_webhook_url.empty()) {
            notifier->add(std::make_shared<WebhookNotifier>(cfg.ozon_webhook_url));
            std::cout << "[INIT] Webhook notifier: "
                      << cfg.ozon_webhook_url << std::endl;
        }

        // -------------------------------------------------------------------
        // Клиенты и инфраструктура
        // -------------------------------------------------------------------
        OnecClient   onec(cfg.onec_base_url, cfg.onec_user, cfg.onec_password,
                          cfg.onec_page_size, cfg.onec_timeout_ms);
        OzonClient   ozon(cfg.ozon_client_id, cfg.ozon_api_key,
                          cfg.ozon_page_size, cfg.ozon_timeout_ms);
        DbRepository db(cfg.db_conn_str);
        WorkerPool   pool(cfg.worker_threads, cfg.task_queue_max);

        // -------------------------------------------------------------------
        // 1С OData + репозиторий чекпоинтов
        // -------------------------------------------------------------------
        OnecCatalog         onec_catalog(cfg.onec_base_url,
                                         cfg.onec_user,
                                         cfg.onec_password,
                                         cfg.onec_timeout_ms,
                                         cfg.onec_allow_insecure_http);
        OnecSyncRepository  onec_repo(cfg.db_conn_str);
        OnecCatalogSync     onec_catalog_sync(onec_catalog, onec_repo, "onec_catalog");

        // -------------------------------------------------------------------
        // SyncService (Ozon + 1С + старый HTTP 1С)
        // -------------------------------------------------------------------
        SyncService sync(onec,
                         ozon,
                         onec_catalog_sync,
                         onec_repo,
                         db,
                         pool,
                         notifier,
                         cfg.notify_every_percent,
                         cfg.notify_min_interval_sec);

        // -------------------------------------------------------------------
        // HTTP-сервер
        // -------------------------------------------------------------------
        Router router;
        register_handlers(router, sync, onec_catalog);
        register_hooks_handlers(router);

        const int thread_count = std::max(1u, std::thread::hardware_concurrency());
        net::io_context ioc{thread_count};

        HttpServer server(ioc, cfg.listen_address, cfg.listen_port, router, sync);
        server.start();

        std::vector<std::thread> threads;
        threads.reserve(thread_count);
        for (int i = 0; i < thread_count; ++i) {
            threads.emplace_back([&ioc] { ioc.run(); });
        }

        std::cout << "Server listening on http://"
                  << cfg.listen_address << ":" << cfg.listen_port << std::endl;
        std::cout << "SSE endpoint:  /events/subscribe" << std::endl;
        std::cout << "Ozon:          POST /ozon/sync" << std::endl;
        std::cout << "1C  catalog:   POST /onec/sync" << std::endl;
        std::cout << "1C  raw:       GET  /api/onec/raw" << std::endl;
        std::cout << "1C  log:       GET  /api/onec/log" << std::endl;

        // ===================================================================
        // АВТОМАТИЧЕСКОЕ ВОЗОБНОВЛЕНИЕ НЕЗАВЕРШЁННЫХ СИНХРОНИЗАЦИЙ
        //
        // Проверяет:
        //   - Ozon: чекпоинт в sync_state (last_status != done)
        //   - 1С:   onec_sync_state.status != done/stopped
        //
        // Если что-то не завершено — автоматически запускает продолжение.
        // ===================================================================
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        sync.resume_on_start();

        for (auto& t : threads) t.join();

    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
