#include "config.hpp"
#include "db_pool.hpp"
#include "db_repository.hpp"
#include "handlers.hpp"
#include "hooks_handlers.hpp"
#include "ozon_handlers.hpp"
#include "http_server.hpp"
#include "inventory_repository.hpp"
#include "notifier.hpp"
#include "async_notifier.hpp"
#include "onec_catalog.hpp"
#include "onec_client.hpp"
#include "onec_inventory.hpp"
#include "onec_ka2_inventory.hpp"
#include "onec_product_mapper.hpp"
#include "ozon_client.hpp"
#include "router.hpp"
#include "sync_service.hpp"
#include "worker_pool.hpp"

#include <boost/asio.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <signal.h>
#endif

namespace net = boost::asio;

static std::atomic<bool> g_stop_requested{false};

extern "C" void on_signal(int) {
    g_stop_requested.store(true, std::memory_order_relaxed);
}

int main(int argc, char* argv[]) {
    try {
        std::string config_path = (argc > 1) ? argv[1] : "config.ini";
        Config cfg = Config::load(config_path);
        set_global_config(cfg);
        std::cout << "Config loaded from " << config_path << std::endl;

        // 1) Сигналы
#if !defined(_WIN32)
        struct sigaction sa{};
        sa.sa_handler = on_signal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;   // без SA_RESTART — Ctrl+C мгновенно
        sigaction(SIGINT,  &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
#else
        std::signal(SIGINT,  on_signal);
        std::signal(SIGTERM, on_signal);
#endif

        // 2) Пул соединений PostgreSQL
        auto db_pool = std::make_shared<scand::db::ConnectionPool>(
            cfg.db_conn_str, 8);

        // 3) Notifier
        auto notifier = std::make_shared<CompositeNotifier>();
        notifier->add(std::make_shared<LogNotifier>());
        notifier->add(std::make_shared<SseNotifier>());

        // 4) WorkerPool
        WorkerPool pool(cfg.worker_threads, cfg.task_queue_max);

        if (!cfg.ozon_webhook_url.empty()) {
            auto webhook = std::make_shared<WebhookNotifier>(
                cfg.ozon_webhook_url);
            notifier->add(std::make_shared<AsyncNotifier>(webhook, pool));
            std::cout << "[INIT] Async webhook notifier: "
                      << cfg.ozon_webhook_url << std::endl;
        }

        // 5) OnecClient
        OnecClient onec(cfg.onec_base_url,
                        cfg.onec_user,
                        cfg.onec_password,
                        cfg.onec_page_size,
                        cfg.onec_timeout_ms);

        // 6) OzonClient
        OzonClient ozon(cfg.ozon_client_id,
                        cfg.ozon_api_key,
                        cfg.ozon_page_size,
                        cfg.ozon_timeout_ms);

        // 7) OnecCatalog
        OnecCatalog onec_catalog(cfg.onec_base_url,
                                 cfg.onec_user,
                                 cfg.onec_password,
                                 cfg.onec_timeout_ms,
                                 cfg.onec_allow_insecure_http);

        // 8) OnecInventory (простой)
        scand::onec::OnecInventory onec_inventory(
            cfg.onec_base_url,
            cfg.onec_user,
            cfg.onec_password,
            cfg.onec_timeout_ms,
            cfg.onec_allow_insecure_http);

        // 9) Ka2Inventory — параметры как в Python
        //    chunk_size=40, page_size_flat=5000, parallel_workers=8
        scand::onec::ka2::Ka2Options ka2_opts;
        ka2_opts.chunk_size       = 40;      // было 15 — как Python _FILTER_CHUNK_SIZE
        ka2_opts.page_size_flat   = 5000;    // было 500 — Python использует 10000
        ka2_opts.page_size_nested = 50;
        ka2_opts.max_retries      = 3;
        ka2_opts.retry_delay_ms   = 1000;
        ka2_opts.parallel_workers = 8;
        ka2_opts.verbose          = true;

        scand::onec::ka2::Ka2Inventory ka2_inventory(
            cfg.onec_base_url,
            cfg.onec_user,
            cfg.onec_password,
            cfg.onec_timeout_ms,
            cfg.onec_allow_insecure_http,
            ka2_opts);

        // 10) Репозитории
        DbRepository       db(db_pool);

        scand::inventory::InventoryRepository inventory_repo(db_pool);

        // 12) SyncService
        SyncService sync(onec, ozon, db, pool, notifier,
                         cfg.notify_every_percent,
                         cfg.notify_min_interval_sec);

        // 13) Роутер
        Router router;
        register_handlers(router, sync, onec_catalog,
                          onec_inventory, ka2_inventory,
                          inventory_repo, db_pool);
        register_hooks_handlers(router);
        register_ozon_handlers(router, sync);

        // 14) io_context
        unsigned hc = std::max(1u, std::thread::hardware_concurrency());
        int thread_count = static_cast<int>(std::min<unsigned>(hc, 8));
        net::io_context ioc{thread_count};

        HttpServer server(ioc, cfg.listen_address, cfg.listen_port,
                          router, sync);
        server.start();

        std::vector<std::thread> threads;
        threads.reserve(thread_count);
        for (int i = 0; i < thread_count; ++i) {
            threads.emplace_back([&ioc] { ioc.run(); });
        }

        std::cout << "Server listening on http://"
                  << cfg.listen_address << ":" << cfg.listen_port << std::endl;
        std::cout << "SSE endpoint:         /events/subscribe" << std::endl;
        std::cout << "Ozon:                 POST /ozon/sync" << std::endl;
        std::cout << "1C  catalog:          POST /onec/sync" << std::endl;
        std::cout << "1C  raw:              GET  /api/onec/raw" << std::endl;
        std::cout << "1C  log:              GET  /api/onec/log" << std::endl;
        std::cout << "Inventory preview:    POST /api/inventory/preview"
                  << std::endl;
        std::cout << "Inventory KA2:        POST /api/inventory/ka2"
                  << std::endl;
        std::cout << "Inventory KA2 preview:POST /api/inventory/ka2/preview"
                  << std::endl;
        std::cout << "Inventory KA2 save:   POST /api/inventory/ka2/save"
                  << std::endl;

        std::cout << "[main] running, Ctrl+C — graceful shutdown"
                  << std::endl;

        // 16) Главный цикл
        while (!g_stop_requested.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        std::cout << "[main] shutdown requested" << std::endl;

        const auto shutdown_timeout =
            std::chrono::seconds(std::max(1, cfg.shutdown_timeout_sec));

        // 17) Shutdown
        sync.request_stop();
        pool.stop_accepting();
        server.stop_accepting();

        std::cout << "[main] waiting for tasks: active=" << pool.active()
                  << " queued=" << pool.queued()
                  << " (timeout " << shutdown_timeout.count() << "s)"
                  << std::endl;

        bool all_done = pool.wait_idle(shutdown_timeout);

        if (!all_done) {
            std::cerr << "[main] TIMEOUT: active=" << pool.active()
                      << " queued=" << pool.queued()
                      << " — forcing exit" << std::endl;
        } else {
            std::cout << "[main] all tasks finished" << std::endl;
        }

        SseBroker::instance().close_all();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        server.stop();
        ioc.stop();

        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }

        std::cout << "[main] done" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
