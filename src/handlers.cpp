#include "handlers.hpp"
#include "onec_raw_log.hpp"
#include "ozon_handlers.hpp"
#include <boost/json.hpp>
#include <iostream>

namespace json = boost::json;

void register_handlers(Router& router,
                       SyncService& sync,
                       OnecCatalog& onec_catalog) {

    // =========================================================================
    // 1С КАТАЛОГ
    // =========================================================================

    // -----------------------------------------------------------------------
    // POST /onec/sync — синхронизация с чекпоинта
    // -----------------------------------------------------------------------
    router.post("/onec/sync", [&sync](const RequestContext&) -> Response {
        std::cout << "[HTTP] POST /onec/sync received" << std::endl;
        bool ok = sync.trigger_onec_sync();
        Response res;
        res.body = json::object({
            {"status", ok ? "onec_sync_triggered" : "onec_sync_already_running"}
        });
        return res;
    });

    // -----------------------------------------------------------------------
    // POST /onec/sync/full — полная синхронизация со сбросом чекпоинта
    // -----------------------------------------------------------------------
    router.post("/onec/sync/full", [&sync](const RequestContext&) -> Response {
        std::cout << "[HTTP] POST /onec/sync/full received" << std::endl;
        bool ok = sync.trigger_onec_full();
        Response res;
        res.body = json::object({
            {"status", ok ? "onec_full_sync_triggered" : "onec_sync_already_running"}
        });
        return res;
    });

    router.post("/onec/stop", [&sync](const RequestContext&) -> Response {
        std::cout << "[HTTP] POST /onec/stop received" << std::endl;
        sync.stop_onec();
        Response res;
        res.body = json::object({{"status", "stopped"}});
        return res;
    });

    // -----------------------------------------------------------------------
    // GET /onec/status — текущий прогресс 1С
    // -----------------------------------------------------------------------
    router.get("/onec/status", [&sync](const RequestContext&) -> Response {
        auto p = sync.current_onec_progress();
        Response res;
        res.body = json::object({
            {"source",    p.source},
            {"processed", p.processed},
            {"total",     p.total},
            {"percent",   p.percent},
            {"status",    p.status},
            {"running",   sync.is_onec_running()}
        });
        return res;
    });

    // =========================================================================
    // OZON
    // =========================================================================

    // -----------------------------------------------------------------------
    // POST /ozon/sync — синхронизация Ozon с чекпоинта
    // -----------------------------------------------------------------------
    router.post("/ozon/sync", [&sync](const RequestContext&) -> Response {
        std::cout << "[HTTP] POST /ozon/sync received" << std::endl;
        bool ok = sync.trigger_ozon_sync();
        Response res;
        res.body = json::object({
            {"status", ok ? "ozon_sync_triggered" : "ozon_sync_already_running"}
        });
        return res;
    });

    // -----------------------------------------------------------------------
    // POST /ozon/sync/full — полная синхронизация Ozon со сбросом
    // -----------------------------------------------------------------------
    router.post("/ozon/sync/full", [&sync](const RequestContext&) -> Response {
        std::cout << "[HTTP] POST /ozon/sync/full received" << std::endl;
        bool ok = sync.trigger_ozon_full();
        Response res;
        res.body = json::object({
            {"status", ok ? "ozon_full_sync_triggered" : "ozon_sync_already_running"}
        });
        return res;
    });

    router.post("/ozon/stop", [&sync](const RequestContext&) -> Response {
        std::cout << "[HTTP] POST /ozon/stop received" << std::endl;
        sync.stop_ozon();
        Response res;
        res.body = json::object({{"status", "stopped"}});
        return res;
    });

    // -----------------------------------------------------------------------
    // GET /ozon/status — текущий прогресс Ozon
    // -----------------------------------------------------------------------
    router.get("/ozon/status", [&sync](const RequestContext&) -> Response {
        auto p = sync.current_ozon_progress();
        Response res;
        res.body = json::object({
            {"source",    p.source},
            {"processed", p.processed},
            {"total",     p.total},
            {"percent",   p.percent},
            {"status",    p.status},
            {"running",   sync.is_ozon_running()}
        });
        return res;
    });

    // =========================================================================
    // 1С OData — отладочные эндпоинты
    // =========================================================================

    // -----------------------------------------------------------------------
    // GET /api/onec/raw — сырой ответ от 1С (10 записей)
    // -----------------------------------------------------------------------
    router.get("/api/onec/raw", [&onec_catalog](const RequestContext&) -> Response {
        auto raw = onec_catalog.read_raw(
            "/Catalog_Номенклатура?$format=json&$top=10"
        );
        Response res;
        res.raw_body = (raw.status_code != 200)
            ? ("HTTP " + std::to_string(raw.status_code) + "\n\n" + raw.body)
            : raw.body;
        res.raw_content_type = "application/json; charset=utf-8";
        return res;
    });

    // -----------------------------------------------------------------------
    // GET /api/onec/raw/{path} — произвольный путь
    // -----------------------------------------------------------------------
    router.get("/api/onec/raw/{path}", [&onec_catalog](const RequestContext& ctx) -> Response {
        auto it = ctx.path_params.find("path");
        std::string path = (it != ctx.path_params.end()) ? it->second : "";
        auto raw = onec_catalog.read_raw("/" + path);
        Response res;
        res.raw_body = (raw.status_code != 200)
            ? ("HTTP " + std::to_string(raw.status_code) + "\n\n" + raw.body)
            : raw.body;
        res.raw_content_type = "application/json; charset=utf-8";
        return res;
    });

    // -----------------------------------------------------------------------
    // GET /api/onec/raw-url — какой URL строится
    // -----------------------------------------------------------------------
    router.get("/api/onec/raw-url", [&onec_catalog](const RequestContext&) -> Response {
        auto url = onec_catalog.build_url(
            "/Catalog_Номенклатура?$format=json&$top=10"
        );
        Response res;
        res.body = json::object({{"url", url}});
        return res;
    });

    // -----------------------------------------------------------------------
    // GET /api/onec/log — полный лог обмена с 1С
    // -----------------------------------------------------------------------
    router.get("/api/onec/log", [](const RequestContext&) -> Response {
        Response res;
        res.raw_body         = OnecRawLog::instance().dump();
        res.raw_content_type = "text/plain; charset=utf-8";
        return res;
    });

    // -----------------------------------------------------------------------
    // GET /api/onec/log/tail/{n} — последние N строк лога
    // -----------------------------------------------------------------------
    router.get("/api/onec/log/tail/{n}", [](const RequestContext& ctx) -> Response {
        auto it = ctx.path_params.find("n");
        size_t n = 50;
        if (it != ctx.path_params.end()) {
            try { n = std::stoul(it->second); } catch (...) {}
        }
        Response res;
        res.raw_body         = OnecRawLog::instance().tail(n);
        res.raw_content_type = "text/plain; charset=utf-8";
        return res;
    });

    // -----------------------------------------------------------------------
    // DELETE /api/onec/log — очистить лог
    // -----------------------------------------------------------------------
    router.del("/api/onec/log", [](const RequestContext&) -> Response {
        OnecRawLog::instance().clear();
        Response res;
        res.body = json::object({{"status", "cleared"}});
        return res;
    });

    // =========================================================================
    // 1С — старый режим через HTTP-сервис (сигналы от 1С)
    // =========================================================================

    // -----------------------------------------------------------------------
    // POST /notify — сигнал от 1С об изменении
    // -----------------------------------------------------------------------
    router.post("/notify", [&sync](const RequestContext& ctx) -> Response {
        std::string event_type = "unknown";
        std::string product_id;

        if (ctx.body.is_object()) {
            const auto& obj = ctx.body.as_object();

            auto it_event = obj.find("event");
            if (it_event != obj.end() && it_event->value().is_string())
                event_type = json::value_to<std::string>(it_event->value());

            auto it_id = obj.find("id");
            if (it_id != obj.end() && it_id->value().is_string())
                product_id = json::value_to<std::string>(it_id->value());
        }

        std::cout << "[SIGNAL] event=" << event_type
                  << " id=" << (product_id.empty() ? "<none>" : product_id)
                  << std::endl;

        Response res;
        if (!product_id.empty()) {
            bool ok = sync.trigger_single(product_id);
            res.body = json::object({
                {"status", ok ? "single_update_triggered" : "queue_full"}
            });
        } else {
            bool ok = sync.trigger_full();
            res.body = json::object({
                {"status", ok ? "full_sync_triggered" : "sync_already_running"}
            });
        }
        return res;
    });

    // -----------------------------------------------------------------------
    // POST /sync — принудительная полная синхронизация 1С (старый режим)
    // -----------------------------------------------------------------------
    router.post("/sync", [&sync](const RequestContext&) -> Response {
        std::cout << "[HTTP] POST /sync received" << std::endl;
        bool ok = sync.trigger_full();
        Response res;
        res.body = json::object({
            {"status", ok ? "full_sync_triggered" : "sync_already_running"}
        });
        return res;
    });

    // -----------------------------------------------------------------------
    // POST /sync/{id} — точечное обновление товара 1С (старый режим)
    // -----------------------------------------------------------------------
    router.post("/sync/{id}", [&sync](const RequestContext& ctx) -> Response {
        auto it = ctx.path_params.find("id");
        if (it == ctx.path_params.end()) {
            Response res;
            res.status = http::status::bad_request;
            res.body = json::object({{"error", "missing id"}});
            return res;
        }

        bool ok = sync.trigger_single(it->second);
        Response res;
        res.body = json::object({
            {"status", ok ? "single_update_triggered" : "queue_full"},
            {"id", it->second}
        });
        return res;
    });

    // =========================================================================
    // HEALTH
    // =========================================================================

    // -----------------------------------------------------------------------
    // GET /health — проверка живости сервиса
    // -----------------------------------------------------------------------
    router.get("/health", [&sync](const RequestContext&) -> Response {
        Response res;
        res.body = json::object({
            {"status", "ok"},
            {"sync_running",   sync.is_running()},
            {"ozon_running",   sync.is_ozon_running()},
            {"onec_running",   sync.is_onec_running()}
        });
        return res;
    });

    // =========================================================================
    // Ozon push-уведомления
    // =========================================================================
    register_ozon_handlers(router, sync);
}
