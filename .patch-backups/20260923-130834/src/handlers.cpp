#include "handlers.hpp"
#include "product_catalog_pull.hpp"
#include "router.hpp"
#include "sync_service.hpp"
#include "onec_catalog.hpp"
#include "onec_inventory.hpp"
#include "onec_ka2_inventory.hpp"
#include "onec_product_catalog.hpp"
#include "inventory_repository.hpp"
#include "onec_raw_log.hpp"
#include "inventory_preview.hpp"
#include "db_pool.hpp"

#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <pqxx/pqxx>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace beast = boost::beast;
namespace http  = beast::http;
namespace json  = boost::json;

namespace {

// ===========================================================================
// Утилиты
// ===========================================================================
Response make_503() {
    Response r;
    r.status = http::status::service_unavailable;
    r.body   = json::object{{"status", "shutting_down"}};
    return r;
}

template <class Trigger, class Snapshot>
Response handle_trigger(SyncService& sync,
                        Trigger        trigger,
                        Snapshot       snapshot,
                        const char*    ok_status)
{
    if (sync.is_shutting_down()) return make_503();

    bool ok = (sync.*trigger)();

    Response r;
    r.status = ok ? http::status::ok : http::status::conflict;

    json::object snap = (sync.*snapshot)();
    snap["started"] = ok;
    snap["status"]  = ok ? ok_status : "already_running";
    r.body = std::move(snap);
    return r;
}

// Текущее UTC-время в формате 1С: "2026-09-22T12:00:00"
std::string now_period_1c() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    return buf;
}

// ===========================================================================
// Сборка SnapshotRequest из PullResult
// ===========================================================================
scand::inventory::SnapshotRequest pull_to_snapshot(
    const std::string& period,
    const scand::onec::ka2::PullResult& pull,
    const std::unordered_map<std::string, std::string>& offer_by_ref,
    bool is_preview)
{
    scand::inventory::SnapshotRequest req;
    req.measured_at = period + "Z";    // ISO 8601 UTC
    req.mode        = "FULL";

    std::string batch = std::string(is_preview ? "preview-" : "odata-") + period;
    std::replace(batch.begin(), batch.end(), ':', '-');
    req.batch_id = batch;

    // 1. warehouses
    for (const auto& w : pull.warehouses) {
        scand::inventory::WarehouseInput wi;
        wi.warehouse_id   = w.ref_key;
        wi.warehouse_name = w.description;
        if (wi.warehouse_id.empty()) continue;
        req.warehouses.push_back(std::move(wi));
    }

    // 2. stocks
    for (const auto& s : pull.stocks) {
        if (s.nomenclature_key.empty()) continue;

        scand::inventory::StockInput si;
        si.sku_1c       = s.nomenclature_key;
        si.warehouse_id = s.warehouse_key;
        si.available    = static_cast<long long>(s.quantity);
        si.reserved     = 0;

        auto oit = offer_by_ref.find(s.nomenclature_key);
        if (oit != offer_by_ref.end() && !oit->second.empty()) {
            si.offer_id = oit->second;
        }

        for (const auto& w : pull.warehouses) {
            if (w.ref_key == s.warehouse_key) {
                si.warehouse_name = w.description;
                break;
            }
        }

        req.stocks.push_back(std::move(si));
    }

    // 3. costs — уникальные SKU
    struct Accum {
        double total_cost = 0.0;
        double total_qty  = 0.0;
    };
    std::unordered_map<std::string, Accum> by_sku;

    std::unordered_map<std::string, std::string> analit_to_nom;
    for (const auto& a : pull.analytics) {
        if (!a.ref_key.empty() && !a.nomenclature_key.empty()) {
            analit_to_nom[a.ref_key] = a.nomenclature_key;
        }
    }

    for (const auto& c : pull.costs) {
        auto it = analit_to_nom.find(c.analytic_key);
        if (it == analit_to_nom.end()) continue;
        const std::string& sku = it->second;
        if (sku.empty()) continue;

        auto& acc = by_sku[sku];
        acc.total_cost += c.cost;
        acc.total_qty  += c.quantity;
    }

    for (const auto& [sku, acc] : by_sku) {
        scand::inventory::CostInput ci;
        ci.sku_1c = sku;

        if (acc.total_qty > 0 && acc.total_cost > 0) {
            ci.batch_avg_cost = acc.total_cost / acc.total_qty;
        }

        double min_purchase = 0.0;
        std::string source_at;
        for (const auto& sp : pull.suppliers) {
            if (sp.nomenclature_key != sku) continue;
            if (sp.price <= 0) continue;
            if (min_purchase == 0.0 || sp.price < min_purchase) {
                min_purchase = sp.price;
                source_at    = sp.period;
            }
        }
        if (min_purchase > 0) {
            ci.purchase_cost       = min_purchase;
            ci.purchase_source     = "supplier_register";
            ci.purchase_source_at  = source_at;
        }

        if (ci.batch_avg_cost || ci.purchase_cost || ci.dealer_cost) {
            req.costs.push_back(std::move(ci));
        }
    }

    return req;
}

} // namespace

// ===========================================================================
// Регистрация всех хендлеров
// ===========================================================================
void register_handlers(Router& router,
                       SyncService& sync,
                       OnecCatalog& onec_catalog,
                       scand::onec::OnecInventory& /*onec_inventory*/,
                       scand::onec::ka2::Ka2Inventory& ka2_inventory,
                       scand::inventory::InventoryRepository& inventory_repo,
                       std::shared_ptr<scand::db::ConnectionPool> db_pool)
{
    // ======================= DASHBOARD ROOT =======================
    router.get("/", [](const RequestContext&) {
        Response r;
        r.status           = http::status::ok;
        r.raw_body         = "<h1>scand</h1><p>OK</p>";
        r.raw_content_type = "text/html; charset=utf-8";
        return r;
    });

    // ======================= OZON =======================
    router.post("/ozon/sync", [&sync](const RequestContext&) {
        return handle_trigger(sync, &SyncService::trigger_ozon_sync,
                              &SyncService::ozon_snapshot, "started");
    });

    router.post("/ozon/sync-full", [&sync](const RequestContext&) {
        return handle_trigger(sync, &SyncService::trigger_ozon_full,
                              &SyncService::ozon_snapshot, "started_full");
    });

    router.post("/ozon/stop", [&sync](const RequestContext&) {
        sync.stop_ozon();
        Response r;
        r.status = http::status::ok;
        r.body   = sync.ozon_snapshot();
        return r;
    });

    router.get("/ozon/progress", [&sync](const RequestContext&) {
        Response r;
        r.status = http::status::ok;
        r.body   = sync.ozon_snapshot();
        return r;
    });

    // ======================= 1C RAW =======================
    router.get("/api/onec/raw", [&onec_catalog](const RequestContext& ctx) {
        Response r;
        try {
            auto getNum = [&](const char* key, size_t def) -> size_t {
                auto it = ctx.query_params.find(key);
                if (it == ctx.query_params.end() || it->second.empty()) return def;
                try { return std::stoul(it->second); } catch (...) { return def; }
            };
            auto getBool = [&](const char* key, bool def) -> bool {
                auto it = ctx.query_params.find(key);
                if (it == ctx.query_params.end()) return def;
                return it->second == "1" || it->second == "true";
            };

            size_t top           = getNum("top",  100);
            size_t skip          = getNum("skip", 0);
            bool   only_products = getBool("only_products", true);

            if (top  == 0)   top  = 100;
            if (top  > 1000) top  = 1000;

            size_t total_count = 0;
            if (auto tc = onec_catalog.read_total_count()) total_count = *tc;

            OnecCatalogLimits limits;
            limits.page_size = top;
            limits.max_rows  = top + skip;

            auto rows = onec_catalog.read_all_rows(limits);

            json::array items;
            size_t i = 0;
            for (const auto& row : rows) {
                if (i++ < skip) continue;
                if (items.size() >= top) break;
                if (only_products) {
                    if (row.is_folder     == "true") continue;
                    if (row.deletion_mark == "true") continue;
                }
                items.push_back(json::object{
                    {"ref_key",       row.ref_key},
                    {"code",          row.code},
                    {"article",       row.article},
                    {"name",          row.name},
                    {"full_name",     row.full_name},
                    {"is_folder",     row.is_folder},
                    {"deletion_mark", row.deletion_mark},
                });
            }

            r.status = http::status::ok;
            r.body   = json::object{
                {"count",    items.size()},
                {"total",    total_count},
                {"top",      top},
                {"skip",     skip},
                {"items",    std::move(items)},
            };
        } catch (const std::exception& e) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", e.what()}};
        }
        return r;
    });

    // ======================= 1C LOG =======================
    router.get("/api/onec/log", [](const RequestContext&) {
        Response r;
        auto raw = OnecRawLog::instance().tail(500);
        json::array items;
        std::istringstream ss(raw);
        std::string line;
        while (std::getline(ss, line)) items.emplace_back(line);
        r.status = http::status::ok;
        r.body   = json::object{{"items", std::move(items)}};
        return r;
    });

    // ======================= INVENTORY KA2 PREVIEW =======================
    router.post("/api/inventory/ka2/preview",
                [&ka2_inventory, db_pool](const RequestContext& ctx) {
        Response r;
        try {
            std::vector<std::string> product_keys;
            std::unordered_map<std::string, std::string> offer_by_ref;
            {
                auto lease = db_pool->acquire();
                pqxx::work tx(lease.get());
                auto res = tx.exec(
                    "SELECT id, COALESCE(offer_id, '') FROM products_1c "
                    "WHERE id IS NOT NULL");
                for (const auto& row : res) {
                    std::string id = row[0].as<std::string>();
                    std::string of = row[1].as<std::string>();
                    product_keys.push_back(id);
                    if (!of.empty()) offer_by_ref[id] = of;
                }
            }
            if (product_keys.empty()) {
                r.status = http::status::bad_request;
                r.body   = json::object{{"error",
                    "products_1c is empty, run /onec/sync first"}};
                return r;
            }

            std::string period;
            auto it = ctx.query_params.find("period");
            if (it != ctx.query_params.end() && !it->second.empty())
                period = it->second;
            else
                period = now_period_1c();

            auto pull = ka2_inventory.pull(product_keys, period);

            auto req = pull_to_snapshot(period, pull, offer_by_ref, true);

            auto preview = scand::inventory::build_preview(req);

            json::array cost_preview;
            for (const auto& cp : preview.cost_preview) {
                cost_preview.push_back(json::object{
                    {"sku_1c",        cp.sku_1c},
                    {"selected_cost", std::to_string(cp.selected_cost)},
                    {"cost_source",   cp.cost_source},
                });
            }
            json::array warns;
            for (const auto& w : preview.warnings) warns.emplace_back(w);

            r.status = http::status::ok;
            r.body   = json::object{
                {"status",          preview.status},
                {"batch_id",        preview.batch_id},
                {"stock_rows",      preview.stock_rows},
                {"cost_rows",       preview.cost_rows},
                {"sku_count",       preview.sku_count},
                {"warehouse_count", preview.warehouse_count},
                {"cost_preview",    std::move(cost_preview)},
                {"warnings",        std::move(warns)},
            };
        } catch (const std::exception& e) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", e.what()}};
        }
        return r;
    });

    // ======================= INVENTORY KA2 SAVE =======================
    router.post("/api/inventory/ka2/save",
                [&ka2_inventory, &inventory_repo, db_pool]
                (const RequestContext& ctx) {
        Response r;
        try {
            std::vector<std::string> product_keys;
            std::unordered_map<std::string, std::string> offer_by_ref;
            {
                auto lease = db_pool->acquire();
                pqxx::work tx(lease.get());
                auto res = tx.exec(
                    "SELECT id, COALESCE(offer_id, '') FROM products_1c "
                    "WHERE id IS NOT NULL");
                for (const auto& row : res) {
                    std::string id = row[0].as<std::string>();
                    std::string of = row[1].as<std::string>();
                    product_keys.push_back(id);
                    if (!of.empty()) offer_by_ref[id] = of;
                }
            }
            if (product_keys.empty()) {
                r.status = http::status::bad_request;
                r.body   = json::object{{"error",
                    "products_1c is empty, run /onec/sync first"}};
                return r;
            }

            std::string period;
            auto it = ctx.query_params.find("period");
            if (it != ctx.query_params.end() && !it->second.empty())
                period = it->second;
            else
                period = now_period_1c();

            auto pull = ka2_inventory.pull(product_keys, period);

            auto req = pull_to_snapshot(period, pull, offer_by_ref, false);

            auto result = inventory_repo.import_snapshot(req);
            if (!result.ok()) {
                r.status = http::status::internal_server_error;
                r.body   = json::object{{"error", result.error}};
                return r;
            }

            r.status = http::status::ok;
            r.body   = json::object{
                {"status",          result.value->status},
                {"batch_id",        result.value->batch_id},
                {"stocks_accepted", result.value->stocks_accepted},
                {"costs_accepted",  result.value->costs_accepted},
            };
        } catch (const std::exception& e) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", e.what()}};
        }
        return r;
    });

    // ======================= 1C PRODUCT CATALOG PULL =======================
    router.post("/api/onec/product-catalog/pull",
                [db_pool](const RequestContext&) {
        Response r;
        try {
            auto lease = db_pool->acquire();
            pqxx::work tx(lease.get());
            auto result = scand::onec::catalog::pull_onec_product_catalog(
                tx, 1);
            tx.commit();
            r.status = http::status::ok;
            r.body = json::object{
                {"accepted",  static_cast<std::uint64_t>(result.accepted)},
                {"migrated",  static_cast<std::uint64_t>(result.migrated)},
                {"conflicts", static_cast<std::uint64_t>(result.conflicts)},
                {"measured_at", result.measured_at},
            };
        } catch (const std::exception& e) {
            r.status = http::status::unprocessable_entity;
            r.body   = json::object{{"error", e.what()}};
        }
        return r;
    });
}
