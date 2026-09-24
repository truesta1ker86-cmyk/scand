#include "config.hpp"
#include "db_pool.hpp"
#include "db_repository.hpp"
#include "handlers.hpp"
#include "hooks_handlers.hpp"
#include "ozon_handlers.hpp"
#include "http_server.hpp"
#include "inventory_checkpoint_repository.hpp"
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
#include "sync_runs_repository.hpp"
#include "worker_pool.hpp"

#include <boost/asio.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
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

// ===========================================================================
// Дёрнуть локальный POST /api/1c/inventory-pulls через curl
// (используется для авто-возобновления один раз при старте)
// ===========================================================================
static void resume_inventory_pull_async(unsigned short port,
                                         int delay_sec = 2) {
    std::thread([port, delay_sec]() {
        std::this_thread::sleep_for(std::chrono::seconds(delay_sec));

        std::string cmd =
            "curl -s -X POST -o /dev/null -w '%{http_code}' "
            "http://127.0.0.1:" + std::to_string(port) +
            "/api/1c/inventory-pulls";

        int rc = std::system(cmd.c_str());
        std::cout << "[resume] inventory-pull curl rc=" << rc << std::endl;
    }).detach();
}

int main(int argc, char* argv[]) {
    try {
        std::string config_path = (argc > 1) ? argv[1] : "config.ini";
        Config cfg = Config::load(config_path);
        set_global_config(cfg);
        std::cout << "Config loaded from " << config_path << std::endl;

#if !defined(_WIN32)
        struct sigaction sa{};
        sa.sa_handler = on_signal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT,  &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
#else
        std::signal(SIGINT,  on_signal);
        std::signal(SIGTERM, on_signal);
#endif

        // ================================================================
        // 1. Пул соединений PostgreSQL
        // ================================================================
        auto db_pool = std::make_shared<scand::db::ConnectionPool>(
            cfg.db_conn_str, 8);

        // ================================================================
        // 2. Пометить старые running как interrupted
        //    (сервис перезапустился, старые потоки убиты)
        // ================================================================
        std::shared_ptr<SyncRunsRepository> sync_runs_repo;
        std::shared_ptr<InventoryCheckpointRepository> inv_cp_repo;
        try {
            sync_runs_repo = std::make_shared<SyncRunsRepository>(db_pool);
            inv_cp_repo    = std::make_shared<InventoryCheckpointRepository>(db_pool);

            sync_runs_repo->mark_interrupted(1, "inventory_pull", "service restarted");
            sync_runs_repo->mark_interrupted(1, "ozon",           "service restarted");
            inv_cp_repo->mark_interrupted("service restarted", 1);

            std::cout << "[startup] marked stale runs as interrupted" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[startup] mark_interrupted failed: "
                      << e.what() << std::endl;
        }

        // ================================================================
        // 3. Notifier
        // ================================================================
        auto notifier = std::make_shared<CompositeNotifier>();
        notifier->add(std::make_shared<LogNotifier>());
        notifier->add(std::make_shared<SseNotifier>());

        // ================================================================
        // 4. WorkerPool
        // ================================================================
        WorkerPool pool(cfg.worker_threads, cfg.task_queue_max);

        if (!cfg.ozon_webhook_url.empty()) {
            auto webhook = std::make_shared<WebhookNotifier>(cfg.ozon_webhook_url);
            notifier->add(std::make_shared<AsyncNotifier>(webhook, pool));
            std::cout << "[INIT] Async webhook notifier: "
                      << cfg.ozon_webhook_url << std::endl;
        }

        // ================================================================
        // 5. OnecClient
        // ================================================================
        OnecClient onec(cfg.onec_base_url, cfg.onec_user, cfg.onec_password,
                        cfg.onec_page_size, cfg.onec_timeout_ms,
                        cfg.onec_allow_insecure_http,
                        cfg.onec_allow_private_network);

        // ================================================================
        // 6. OzonClient
        // ================================================================
        OzonClient ozon(cfg.ozon_client_id, cfg.ozon_api_key,
                        cfg.ozon_page_size, cfg.ozon_timeout_ms);

        // ================================================================
        // 7. OnecCatalog
        // ================================================================
        OnecCatalog onec_catalog(cfg.onec_base_url, cfg.onec_user,
                                 cfg.onec_password, cfg.onec_timeout_ms,
                                 cfg.onec_allow_insecure_http,
                                 cfg.onec_allow_private_network);

        // ================================================================
        // 8. OnecInventory
        // ================================================================
        scand::onec::OnecInventory onec_inventory(
            cfg.onec_base_url, cfg.onec_user, cfg.onec_password,
            cfg.onec_timeout_ms,
            cfg.onec_allow_insecure_http,
            cfg.onec_allow_private_network);

        // ================================================================
        // 9. Ka2Inventory
        // ================================================================
        scand::onec::ka2::Ka2Options ka2_opts;
        ka2_opts.chunk_size       = 40;
        ka2_opts.page_size_flat   = 10000;
        ka2_opts.max_retries      = 3;
        ka2_opts.retry_delay_ms   = 1000;
        ka2_opts.parallel_workers = 16;
        ka2_opts.verbose          = true;

        scand::onec::ka2::Ka2Inventory ka2_inventory(
            cfg.onec_base_url, cfg.onec_user, cfg.onec_password,
            cfg.onec_timeout_ms,
            cfg.onec_allow_insecure_http,
            ka2_opts,
            cfg.onec_allow_private_network);

        // ================================================================
        // 10. Репозитории
        // ================================================================
        DbRepository db(db_pool);
        scand::inventory::InventoryRepository inventory_repo(db_pool);

        // ================================================================
        // 11. SyncService
        // ================================================================
        SyncService sync(onec, ozon, db, pool, notifier, db_pool,
                         cfg.notify_every_percent,
                         cfg.notify_min_interval_sec,
                         sync_runs_repo);

        // ================================================================
        // 12. Роутер
        // ================================================================
        Router router;
        register_handlers(router, sync, onec_catalog,
                          onec_inventory, ka2_inventory,
                          inventory_repo, db_pool);
        register_hooks_handlers(router);
        register_ozon_handlers(router, sync);

        // ================================================================
        // 13. HTTP-сервер
        // ================================================================
        unsigned hc = std::max(1u, std::thread::hardware_concurrency());
        int thread_count = static_cast<int>(std::min<unsigned>(hc, 8));
        net::io_context ioc{thread_count};

        HttpServer server(ioc, cfg.listen_address, cfg.listen_port, router, sync);
        server.start();

        std::vector<std::thread> threads;
        threads.reserve(thread_count);
        for (int i = 0; i < thread_count; ++i) {
            threads.emplace_back([&ioc] { ioc.run(); });
        }

        std::cout << "=== scand — сервис интеграции Ozon ↔ 1С ===" << std::endl;
        std::cout << "Listening on http://"
                  << cfg.listen_address << ":" << cfg.listen_port
                  << std::endl << std::endl;

        // ================================================================
        // 14. Авто-возобновление Ozon (SyncService)
        // ================================================================
        try {
            sync.resume_on_start();
        } catch (const std::exception& e) {
            std::cerr << "[startup] resume_on_start failed: "
                      << e.what() << std::endl;
        }

        // ================================================================
        // 15. Авто-возобновление inventory-pull (один раз при старте)
        //
        //     find_unfinished ищет только 'interrupted' — то есть задача
        //     была прервана рестартом, но НЕ активна. Никакого цикла.
        // ================================================================
        try {
            auto cp = inv_cp_repo->find_unfinished(1);
            if (cp.has_value()) {
                std::cout << "[startup] resuming inventory_pull "
                          << "(last_stage=" << cp->last_stage
                          << ", job_id=" << cp->job_id << ")"
                          << std::endl;
                resume_inventory_pull_async(cfg.listen_port, /*delay*/ 2);
            } else {
                std::cout << "[startup] no unfinished inventory-pull — nothing to resume"
                          << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "[startup] inventory resume check failed: "
                      << e.what() << std::endl;
        }

        // ================================================================
        // 16. Retry-таймер УДАЛЁН.
        //
        //     Раньше здесь был std::thread, который каждые 60 секунд
        //     проверял inv_cp_repo->find_unfinished(1) и дёргал POST.
        //     Это и был источник бесконечного цикла: find_unfinished
        //     возвращал 'running' от активной задачи, retry запускал
        //     её заново, задача оставалась в 'running', retry снова
        //     запускал... и так по кругу.
        //
        //     Теперь:
        //       * find_unfinished ищет только 'interrupted';
        //       * retry-таймера нет;
        //       * задача перезапускается вручную (кнопка «Все») или
        //         автоматически один раз при рестарте сервиса (блок 15).
        // ================================================================

        std::cout << "[main] running, Ctrl+C — graceful shutdown" << std::endl;

        // ================================================================
        // 17. Главный цикл
        // ================================================================
        while (!g_stop_requested.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        std::cout << "[main] shutdown requested" << std::endl;

        const auto shutdown_timeout =
            std::chrono::seconds(std::max(1, cfg.shutdown_timeout_sec));

        // ================================================================
        // 18. Shutdown
        // ================================================================
        sync.request_stop();
        pool.stop_accepting();
        server.stop_accepting();

        bool all_done = pool.wait_idle(shutdown_timeout);
        if (!all_done) {
            std::cerr << "[main] TIMEOUT — forcing exit" << std::endl;
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
