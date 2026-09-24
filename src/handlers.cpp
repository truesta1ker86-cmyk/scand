#include "handlers.hpp"
#include "sse_broker.hpp"
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
#include "sync_runs_repository.hpp"
#include "inventory_checkpoint_repository.hpp"
#include "allowed_warehouse_repository.hpp"

#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <pqxx/pqxx>
#include <sstream>
#include <string>
#include <thread>
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

inline std::optional<json::object> safe_parse_object(const std::string& s) {
    if (s.empty()) return std::nullopt;
    try {
        auto parsed = json::parse(s);
        if (parsed.is_object()) return parsed.as_object();
    } catch (...) {}
    return std::nullopt;
}

inline void merge_object(json::object& dst, const json::object& src) {
    try {
        for (const auto& [k, v] : src) dst[k] = v;
    } catch (...) {}
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

// ===========================================================================
// SnapshotRequest из PullResult
// ===========================================================================
scand::inventory::SnapshotRequest pull_to_snapshot(
    const std::string& period,
    const scand::onec::ka2::PullResult& pull,
    const std::unordered_map<std::string, std::string>& offer_by_ref,
    bool is_preview,
    const std::unordered_set<std::string>& allowed_warehouses)
{
    scand::inventory::SnapshotRequest req;
    req.measured_at = period + "Z";
    req.mode        = "FULL";

    std::string batch = std::string(is_preview ? "preview-" : "odata-") + period;
    std::replace(batch.begin(), batch.end(), ':', '-');
    req.batch_id = batch;

    auto is_allowed = [&](const std::string& id) {
        if (allowed_warehouses.empty()) return true;
        return allowed_warehouses.find(id) != allowed_warehouses.end();
    };

    // 1. warehouses
    for (const auto& w : pull.warehouses) {
        if (w.ref_key.empty()) continue;
        if (!is_allowed(w.ref_key)) continue;

        scand::inventory::WarehouseInput wi;
        wi.warehouse_id   = w.ref_key;
        wi.warehouse_name = w.description;
        req.warehouses.push_back(std::move(wi));
    }

    // 2. stocks
    {
        struct StockAccum {
            scand::inventory::StockInput si;
            bool initialized = false;
        };
        std::unordered_map<std::string, StockAccum> by_key;

        for (const auto& s : pull.stocks) {
            if (s.nomenclature_key.empty()) continue;
            if (!is_allowed(s.warehouse_key)) continue;

            std::string key = s.nomenclature_key + "|" + s.warehouse_key;
            auto& acc = by_key[key];
            if (!acc.initialized) {
                acc.si.sku_1c       = s.nomenclature_key;
                acc.si.warehouse_id = s.warehouse_key;
                acc.si.available    = 0;
                acc.si.reserved     = 0;

                auto oit = offer_by_ref.find(s.nomenclature_key);
                if (oit != offer_by_ref.end() && !oit->second.empty()) {
                    acc.si.offer_id = oit->second;
                }

                for (const auto& w : pull.warehouses) {
                    if (w.ref_key == s.warehouse_key) {
                        acc.si.warehouse_name = w.description;
                        break;
                    }
                }
                acc.initialized = true;
            }
            acc.si.available += static_cast<long long>(s.quantity);
        }

        req.stocks.reserve(by_key.size());
        for (auto& [key, acc] : by_key) {
            req.stocks.push_back(std::move(acc.si));
        }
    }

    // 3. costs
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

// ===========================================================================
// Глобальные указатели + счётчик поколений
// ===========================================================================
scand::onec::ka2::Ka2Inventory* g_ka2_inventory = nullptr;
scand::inventory::InventoryRepository* g_inventory_repo = nullptr;
std::function<scand::inventory::SnapshotRequest(
    const std::string&,
    const scand::onec::ka2::PullResult&,
    const std::unordered_map<std::string, std::string>&,
    bool,
    const std::unordered_set<std::string>&)> g_build_snapshot;

std::atomic<unsigned long long> g_inventory_generation{0};

// ===========================================================================
// Состояние последней задачи inventory_pull
// ===========================================================================
struct InventorySnapshot {
    std::string job_id;
    std::string stage;
    std::string status;
    long long   current = 0;
    long long   total = 0;
    long long   percent = 0;
    long long   accepted = 0;
    long long   migrated = 0;
    long long   conflicts = 0;
    long long   warehouses = 0;
    long long   stocks = 0;
    long long   costs = 0;
    long long   stocks_accepted = 0;
    long long   costs_accepted = 0;
    std::string error;
    std::string measured_at;
    std::map<std::string, std::string> stages;
};

class InventoryPullState {
public:
    static InventoryPullState& instance() {
        static InventoryPullState s;
        return s;
    }

    void update(const InventorySnapshot& snap) {
        std::lock_guard<std::mutex> lk(m_);
        snap_ = snap;
    }

    void clear() {
        std::lock_guard<std::mutex> lk(m_);
        snap_.reset();
    }

    void start_new(const std::string& job_id) {
        std::lock_guard<std::mutex> lk(m_);
        InventorySnapshot s;
        s.job_id  = job_id;
        s.stage   = "start";
        s.status  = "running";
        s.percent = 0;
        s.stages = {
            {"catalog",  "pending"},
            {"ka2_pull", "pending"},
            {"save",     "pending"},
            {"done",     "pending"},
        };
        snap_ = s;
    }

    std::optional<InventorySnapshot> snapshot() {
        std::lock_guard<std::mutex> lk(m_);
        return snap_;
    }

private:
    std::mutex m_;
    std::optional<InventorySnapshot> snap_;
};

void fill_inventory_snapshot(const std::string& job_id,
                             const std::string& stage,
                             const std::string& status,
                             const json::object& obj,
                             InventorySnapshot& out) noexcept
{
    out.job_id = job_id;
    out.stage  = stage;
    out.status = status;

    auto get_i64 = [&](const char* key, long long def) -> long long {
        try {
            auto it = obj.find(key);
            if (it == obj.end()) return def;
            const auto& v = it->value();
            if (v.is_int64())   return v.as_int64();
            if (v.is_uint64())  return static_cast<long long>(v.as_uint64());
            if (v.is_string()) {
                try { return std::stoll(std::string(v.as_string())); }
                catch (...) { return def; }
            }
        } catch (...) {}
        return def;
    };
    auto get_str = [&](const char* key) -> std::string {
        try {
            auto it = obj.find(key);
            if (it == obj.end()) return {};
            const auto& v = it->value();
            if (v.is_string()) return std::string(v.as_string());
        } catch (...) {}
        return {};
    };

    out.current         = get_i64("current",         out.current);
    out.total           = get_i64("total",           out.total);
    out.percent         = get_i64("percent",         out.percent);
    out.accepted        = get_i64("accepted",        out.accepted);
    out.migrated        = get_i64("migrated",        out.migrated);
    out.conflicts       = get_i64("conflicts",       out.conflicts);
    out.warehouses      = get_i64("warehouses",      out.warehouses);
    out.stocks          = get_i64("stocks",          out.stocks);
    out.costs           = get_i64("costs",           out.costs);
    out.stocks_accepted = get_i64("stocks_accepted", out.stocks_accepted);
    out.costs_accepted  = get_i64("costs_accepted",  out.costs_accepted);
    out.error           = get_str("error");
    out.measured_at     = get_str("measured_at");

    const std::vector<std::string> order = {"catalog", "ka2_pull", "save", "done"};
    for (const auto& k : order) {
        if (out.stages.find(k) == out.stages.end()) out.stages[k] = "pending";
    }

    std::string stage_key = stage;
    if (stage == "read" || stage == "normalize" || stage == "migrate" || stage == "start") {
        stage_key = "catalog";
    }

    if (status == "failed" || stage == "error") {
        for (auto& [k, v] : out.stages) {
            if (k == stage_key) v = "failed";
            else if (v == "running") v = "failed";
        }
        if (out.stages.find(stage_key) != out.stages.end()) {
            out.stages[stage_key] = "failed";
        }
    } else if (status == "done" || status == "imported" || stage == "done") {
        for (auto& [k, v] : out.stages) v = "done";
        out.percent = 100;
    } else {
        bool found = false;
        for (const auto& k : order) {
            if (k == stage_key) {
                out.stages[k] = "running";
                found = true;
            } else if (!found) {
                if (out.stages[k] != "done") out.stages[k] = "done";
            } else {
                if (out.stages[k] == "running") out.stages[k] = "pending";
            }
        }
    }
}

long long compute_percent(const std::string& stage,
                          const std::string& status,
                          long long current,
                          long long total) noexcept
{
    if (status == "done" || status == "imported" || stage == "done") return 100;

    const std::vector<std::string> order = {"catalog", "ka2_pull", "save", "done"};
    std::string stage_key = stage;
    if (stage == "read" || stage == "normalize" || stage == "migrate" || stage == "start") {
        stage_key = "catalog";
    }

    int idx = -1;
    for (size_t i = 0; i < order.size(); ++i) {
        if (order[i] == stage_key) { idx = (int)i; break; }
    }
    if (idx < 0) return 0;

    long long pct = (long long)idx * 25;

    if (total > 0) {
        double frac = (double)current / (double)total;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        pct += (long long)(frac * 25.0);
    } else if (status == "running") {
        pct += 12;
    }

    if (pct > 99) pct = 99;
    return pct;
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
    g_ka2_inventory  = &ka2_inventory;
    g_inventory_repo = &inventory_repo;
    g_build_snapshot = &pull_to_snapshot;

    auto sync_runs_repo  = std::make_shared<SyncRunsRepository>(db_pool);
    auto inv_cp_repo     = std::make_shared<InventoryCheckpointRepository>(db_pool);
    auto allowed_wh_repo = std::make_shared<AllowedWarehouseRepository>(db_pool);

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

    router.get("/ozon/progress",
               [&sync, sync_runs_repo](const RequestContext&) {
        Response r;
        try {
            auto snap = sync.ozon_snapshot();

            std::string st;
            try {
                if (auto it = snap.find("status");
                    it != snap.end() && it->value().is_string()) {
                    st = std::string(it->value().as_string());
                }
            } catch (...) {}

            if (!st.empty() && st != "idle") {
                r.status = http::status::ok;
                r.body   = snap;
                return r;
            }

            std::optional<SyncRunRecord> rec;
            try {
                rec = sync_runs_repo->last_run(1, "ozon");
            } catch (const std::exception& e) {
                std::cerr << "[ozon/progress] last_run failed: "
                          << e.what() << std::endl;
            }

            if (!rec.has_value()) {
                r.status = http::status::ok;
                r.body   = snap;
                return r;
            }

            const auto& s = *rec;
            const bool is_done   = (s.status == "done");
            const bool is_failed = (s.status == "failed");

            json::object obj{
                {"source",    "ozon"},
                {"job_id",    s.job_id},
                {"status",    s.status},
                {"stage",     s.stage.empty()
                                ? (is_done ? "done" : (is_failed ? "error" : "start"))
                                : s.stage},
                {"processed", s.accepted},
                {"total",     0},
                {"percent",   is_done ? 100 : 0},
            };
            if (!s.error.empty()) obj["error"] = s.error;

            r.status = http::status::ok;
            r.body   = std::move(obj);
        } catch (const std::exception& e) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", e.what()}};
        } catch (...) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", "unknown"}};
        }
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
        } catch (...) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", "unknown"}};
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
                std::string limit_str, offer_str, ref_key_str;
                {
                    auto it = ctx.query_params.find("limit");
                    if (it != ctx.query_params.end()) limit_str = it->second;
                    it = ctx.query_params.find("offer");
                    if (it != ctx.query_params.end()) offer_str = it->second;
                    it = ctx.query_params.find("ref_key");
                    if (it != ctx.query_params.end()) ref_key_str = it->second;
                }
                long long limit_n = -1;
                if (!limit_str.empty()) {
                    try { limit_n = std::stoll(limit_str); } catch (...) {}
                }

                std::string sql =
                    "SELECT ref_key, COALESCE(article, '') "
                    "FROM inventory1c_products WHERE tenant_id = 1";
                std::vector<std::string> params;
                if (!ref_key_str.empty()) {
                    sql += " AND ref_key = $1";
                    params.push_back(ref_key_str);
                } else if (!offer_str.empty()) {
                    sql += " AND article = $1";
                    params.push_back(offer_str);
                } else if (limit_n > 0) {
                    sql += " ORDER BY id LIMIT " + std::to_string(limit_n);
                }

                auto res = params.empty()
                    ? tx.exec(sql)
                    : tx.exec_params(sql, params[0]);

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
                    "inventory1c_products is empty, run /api/onec/product-catalog/pull first"}};
                return r;
            }

            std::string period;
            auto it = ctx.query_params.find("period");
            if (it != ctx.query_params.end() && !it->second.empty())
                period = it->second;
            else {
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
                period = buf;
            }

            auto pull = ka2_inventory.pull(product_keys, period);

            // Пустое множество — все склады
            std::unordered_set<std::string> no_filter;

            auto req = pull_to_snapshot(period, pull, offer_by_ref, true, no_filter);
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
        } catch (...) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", "unknown"}};
        }
        return r;
    });

    // ======================= INVENTORY KA2 SAVE =======================
    router.post("/api/inventory/ka2/save",
                [&ka2_inventory, &inventory_repo, db_pool, allowed_wh_repo]
                (const RequestContext& ctx) {
        Response r;
        try {
            std::vector<std::string> product_keys;
            std::unordered_map<std::string, std::string> offer_by_ref;
            {
                auto lease = db_pool->acquire();
                pqxx::work tx(lease.get());
                std::string limit_str, offer_str, ref_key_str;
                {
                    auto it = ctx.query_params.find("limit");
                    if (it != ctx.query_params.end()) limit_str = it->second;
                    it = ctx.query_params.find("offer");
                    if (it != ctx.query_params.end()) offer_str = it->second;
                    it = ctx.query_params.find("ref_key");
                    if (it != ctx.query_params.end()) ref_key_str = it->second;
                }
                long long limit_n = -1;
                if (!limit_str.empty()) {
                    try { limit_n = std::stoll(limit_str); } catch (...) {}
                }

                std::string sql =
                    "SELECT ref_key, COALESCE(article, '') "
                    "FROM inventory1c_products WHERE tenant_id = 1";
                std::vector<std::string> params;
                if (!ref_key_str.empty()) {
                    sql += " AND ref_key = $1";
                    params.push_back(ref_key_str);
                } else if (!offer_str.empty()) {
                    sql += " AND article = $1";
                    params.push_back(offer_str);
                } else if (limit_n > 0) {
                    sql += " ORDER BY id LIMIT " + std::to_string(limit_n);
                }

                auto res = params.empty()
                    ? tx.exec(sql)
                    : tx.exec_params(sql, params[0]);

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
                    "inventory1c_products is empty, run /api/onec/product-catalog/pull first"}};
                return r;
            }

            std::string period;
            auto it = ctx.query_params.find("period");
            if (it != ctx.query_params.end() && !it->second.empty())
                period = it->second;
            else {
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
                period = buf;
            }

            auto pull = ka2_inventory.pull(product_keys, period);

            // Читаем фильтр складов
            std::unordered_set<std::string> allowed_warehouses;
            try {
                allowed_warehouses = allowed_wh_repo->enabled_ids(1);
            } catch (const std::exception& e) {
                std::cerr << "[ka2/save] allowed_wh_repo failed: "
                          << e.what() << std::endl;
            }

            auto req  = pull_to_snapshot(period, pull, offer_by_ref, false,
                                         allowed_warehouses);
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
        } catch (...) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", "unknown"}};
        }
        return r;
    });

    // ======================= 1C PRODUCT CATALOG PULL (SSE) =================
    router.post("/api/onec/product-catalog/pull",
                [db_pool](const RequestContext&) {
        Response r;
        try {
            std::string job_id = std::to_string(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count());

            try {
                SseBroker::instance().broadcast("catalog_pull", json::object{
                    {"job_id", job_id},
                    {"stage",  "start"},
                    {"status", "running"},
                });
            } catch (...) {}

            std::thread([db_pool, job_id]() {
                auto emit = [&](const std::string& stage,
                                long long current,
                                long long total,
                                const std::string& extra_json) {
                    try {
                        json::object obj{
                            {"job_id",  job_id},
                            {"stage",   stage},
                            {"current", current},
                            {"total",   total},
                        };
                        if (auto parsed = safe_parse_object(extra_json)) {
                            merge_object(obj, *parsed);
                        }
                        SseBroker::instance().broadcast("catalog_pull", obj);
                    } catch (...) {}
                };

                try {
                    auto lease = db_pool->acquire();
                    pqxx::work tx(lease.get());
                    auto result = scand::onec::catalog::pull_onec_product_catalog(
                        tx, 1,
                        [&](const std::string& stage,
                            long long current,
                            long long total,
                            const std::string& extra_json) {
                            emit(stage, current, total, extra_json);
                        });
                    tx.commit();

                    try {
                        SseBroker::instance().broadcast("catalog_pull", json::object{
                            {"job_id",      job_id},
                            {"stage",       "done"},
                            {"status",      "imported"},
                            {"accepted",    static_cast<long long>(result.accepted)},
                            {"migrated",    static_cast<long long>(result.migrated)},
                            {"conflicts",   static_cast<long long>(result.conflicts)},
                            {"measured_at", result.measured_at},
                        });
                    } catch (...) {}
                } catch (const std::exception& e) {
                    try {
                        SseBroker::instance().broadcast("catalog_pull", json::object{
                            {"job_id", job_id},
                            {"stage",  "error"},
                            {"status", "failed"},
                            {"error",  e.what()},
                        });
                    } catch (...) {}
                } catch (...) {
                    try {
                        SseBroker::instance().broadcast("catalog_pull", json::object{
                            {"job_id", job_id},
                            {"stage",  "error"},
                            {"status", "failed"},
                            {"error",  "unknown"},
                        });
                    } catch (...) {}
                }
            }).detach();

            r.status = http::status::accepted;
            r.body   = json::object{
                {"status", "started"},
                {"job_id", job_id},
                {"message", "Подпишитесь на /events/subscribe — там будет прогресс"},
            };
        } catch (const std::exception& e) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", e.what()}};
        } catch (...) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", "unknown"}};
        }
        return r;
    });
    // ======================= 1C INVENTORY PULLS =======================
    router.post("/api/1c/inventory-pulls",
                [db_pool, sync_runs_repo, inv_cp_repo, allowed_wh_repo]
                (const RequestContext& ctx) {
        Response r;
        try {
            std::string job_id = std::to_string(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count());

            // Новое поколение — старые потоки замолкают
            unsigned long long my_gen = ++g_inventory_generation;

            std::string limit_str, offer_str, ref_key_str, period;
            {
                auto it = ctx.query_params.find("limit");
                if (it != ctx.query_params.end()) limit_str = it->second;
                it = ctx.query_params.find("offer");
                if (it != ctx.query_params.end()) offer_str = it->second;
                it = ctx.query_params.find("ref_key");
                if (it != ctx.query_params.end()) ref_key_str = it->second;
                it = ctx.query_params.find("period");
                if (it != ctx.query_params.end()) period = it->second;
            }

            // Фильтр складов
            std::unordered_set<std::string> allowed_warehouses;
            try {
                allowed_warehouses = allowed_wh_repo->enabled_ids(1);
                std::cout << "[inventory-pull] allowed_warehouses="
                          << allowed_warehouses.size() << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "[inventory-pull] allowed_wh_repo failed: "
                          << e.what() << std::endl;
            }

            // Закрыть старые running
            try {
                sync_runs_repo->abandon_running(1, "inventory_pull",
                                                "superseded by new run");
            } catch (const std::exception& e) {
                std::cerr << "[sync_runs] abandon_running failed: "
                          << e.what() << std::endl;
            }
            try {
                inv_cp_repo->abandon_running("superseded by new run", 1);
            } catch (const std::exception& e) {
                std::cerr << "[inv_cp] abandon_running failed: "
                          << e.what() << std::endl;
            }

            InventoryPullState::instance().start_new(job_id);

            try {
                sync_runs_repo->start("inventory_pull", job_id, 1);
            } catch (const std::exception& e) {
                std::cerr << "[sync_runs] start failed: "
                          << e.what() << std::endl;
            }
            try {
                inv_cp_repo->start(job_id, 1);
            } catch (const std::exception& e) {
                std::cerr << "[inv_cp] start failed: "
                          << e.what() << std::endl;
            }

            auto* ka2_ptr  = g_ka2_inventory;
            auto* repo_ptr = g_inventory_repo;
            auto  build_fn = g_build_snapshot;

            std::thread([db_pool, job_id, limit_str, offer_str, ref_key_str, period,
                         ka2_ptr, repo_ptr, build_fn, sync_runs_repo, inv_cp_repo,
                         allowed_warehouses, my_gen]() mutable {
                auto emit = [&](const std::string& stage,
                                const std::string& status,
                                const std::string& extra_json = "") {
                    // Отбрасываем события устаревших потоков
                    if (g_inventory_generation.load() != my_gen) return;

                    try {
                        json::object obj{
                            {"job_id", job_id},
                            {"stage",  stage},
                            {"status", status},
                        };
                        if (auto parsed = safe_parse_object(extra_json)) {
                            merge_object(obj, *parsed);
                        }

                        long long cur = 0, tot = 0;
                        {
                            auto it_c = obj.find("current");
                            if (it_c != obj.end()) {
                                const auto& v = it_c->value();
                                if (v.is_int64()) cur = v.as_int64();
                                else if (v.is_uint64()) cur = (long long)v.as_uint64();
                            }
                            auto it_t = obj.find("total");
                            if (it_t != obj.end()) {
                                const auto& v = it_t->value();
                                if (v.is_int64()) tot = v.as_int64();
                                else if (v.is_uint64()) tot = (long long)v.as_uint64();
                            }
                        }
                        long long pct = compute_percent(stage, status, cur, tot);
                        obj["percent"] = pct;

                        {
                            InventorySnapshot snap;
                            fill_inventory_snapshot(job_id, stage, status, obj, snap);
                            snap.percent = pct;
                            InventoryPullState::instance().update(snap);
                        }

                        SseBroker::instance().broadcast("inventory_pull", obj);
                    } catch (...) {}
                };

                try {
                    // =====================================================
                    // 1. Каталог
                    // =====================================================
                    emit("catalog", "running");
                    std::size_t products_accepted = 0;
                    std::size_t products_migrated = 0;
                    std::size_t products_conflicts = 0;
                    std::string products_measured_at;
                    {
                        auto lease = db_pool->acquire();
                        pqxx::work tx(lease.get());
                        auto result = scand::onec::catalog::pull_onec_product_catalog(
                            tx, 1,
                            [&](const std::string& stage,
                                long long /*current*/,
                                long long /*total*/,
                                const std::string& extra_json) {
                                emit(stage, "running", extra_json);
                            });
                        tx.commit();
                        products_accepted    = result.accepted;
                        products_migrated    = result.migrated;
                        products_conflicts   = result.conflicts;
                        products_measured_at = result.measured_at;
                    }
                    emit("catalog", "done",
                         "{\"accepted\":" + std::to_string(products_accepted)
                         + ",\"migrated\":" + std::to_string(products_migrated)
                         + ",\"conflicts\":" + std::to_string(products_conflicts)
                         + "}");

                    try {
                        inv_cp_repo->update(
                            job_id, "catalog", "", "",
                            {}, {},
                            (long long)products_accepted,
                            (long long)products_migrated,
                            (long long)products_conflicts,
                            1);
                    } catch (const std::exception& e) {
                        std::cerr << "[inv_cp] update catalog failed: "
                                  << e.what() << std::endl;
                    }

                    // =====================================================
                    // 2. SKU
                    // =====================================================
                    std::vector<std::string> product_keys;
                    std::unordered_map<std::string, std::string> offer_by_ref;
                    {
                        auto lease = db_pool->acquire();
                        pqxx::work tx(lease.get());

                        std::string sql =
                            "SELECT ref_key, COALESCE(article, '') "
                            "FROM inventory1c_products WHERE tenant_id = 1";
                        std::vector<std::string> params;
                        if (!ref_key_str.empty()) {
                            sql += " AND ref_key = $1";
                            params.push_back(ref_key_str);
                        } else if (!offer_str.empty()) {
                            sql += " AND article = $1";
                            params.push_back(offer_str);
                        } else if (!limit_str.empty()) {
                            try {
                                long long limit_n = std::stoll(limit_str);
                                sql += " ORDER BY id LIMIT " + std::to_string(limit_n);
                            } catch (...) {}
                        }

                        auto res = params.empty()
                            ? tx.exec(sql)
                            : tx.exec_params(sql, params[0]);

                        for (const auto& row : res) {
                            std::string id = row[0].as<std::string>();
                            std::string of = row[1].as<std::string>();
                            product_keys.push_back(id);
                            if (!of.empty()) offer_by_ref[id] = of;
                        }
                    }
                    if (product_keys.empty()) {
                        emit("error", "failed",
                             "{\"error\":\"inventory1c_products is empty\"}");
                        if (g_inventory_generation.load() == my_gen) {
                            try {
                                sync_runs_repo->finish_failed(
                                    1, "inventory_pull", job_id,
                                    "inventory1c_products is empty", "error");
                            } catch (...) {}
                            try {
                                inv_cp_repo->finish(job_id, "failed",
                                    "inventory1c_products is empty", 1);
                            } catch (...) {}
                        }
                        return;
                    }

                    // =====================================================
                    // 3. Период
                    // =====================================================
                    std::string period_use = period;
                    if (period_use.empty()) {
                        auto now = std::chrono::system_clock::now();
                        auto t   = std::chrono::system_clock::to_time_t(now);
                        std::tm tm_buf{};
#if defined(_WIN32)
                        gmtime_s(&tm_buf, &t);
#else
                        gmtime_r(&t, &tm_buf);
#endif
                        char buf[32];
                        std::strftime(buf, sizeof(buf),
                                      "%Y-%m-%dT%H:%M:%S", &tm_buf);
                        period_use = buf;
                    }

                    // =====================================================
                    // 4. KA2 pull
                    // =====================================================
                    emit("ka2_pull", "running",
                         "{\"product_keys\":" + std::to_string(product_keys.size()) + "}");

                    if (ka2_ptr == nullptr) {
                        emit("error", "failed",
                             "{\"error\":\"ka2_inventory not set\"}");
                        if (g_inventory_generation.load() == my_gen) {
                            try {
                                sync_runs_repo->finish_failed(
                                    1, "inventory_pull", job_id,
                                    "ka2_inventory not set", "error");
                            } catch (...) {}
                            try {
                                inv_cp_repo->finish(job_id, "failed",
                                    "ka2_inventory not set", 1);
                            } catch (...) {}
                        }
                        return;
                    }

                    auto pull_result = ka2_ptr->pull(product_keys, period_use);

                    emit("ka2_pull", "done",
                         "{\"warehouses\":" + std::to_string(pull_result.warehouses.size())
                         + ",\"analytics\":" + std::to_string(pull_result.analytics.size())
                         + ",\"stocks\":" + std::to_string(pull_result.stocks.size())
                         + ",\"costs\":" + std::to_string(pull_result.costs.size())
                         + ",\"suppliers\":" + std::to_string(pull_result.suppliers.size())
                         + ",\"warnings\":" + std::to_string(pull_result.warnings.size())
                         + "}");

                    try {
                        inv_cp_repo->update(
                            job_id, "ka2_pull", period_use, "",
                            product_keys, offer_by_ref,
                            (long long)products_accepted,
                            (long long)products_migrated,
                            (long long)products_conflicts,
                            1);
                    } catch (const std::exception& e) {
                        std::cerr << "[inv_cp] update ka2_pull failed: "
                                  << e.what() << std::endl;
                    }

                    // =====================================================
                    // 5. Snapshot
                    // =====================================================
                    if (!build_fn) {
                        emit("error", "failed",
                             "{\"error\":\"snapshot builder not set\"}");
                        if (g_inventory_generation.load() == my_gen) {
                            try {
                                sync_runs_repo->finish_failed(
                                    1, "inventory_pull", job_id,
                                    "snapshot builder not set", "error");
                            } catch (...) {}
                            try {
                                inv_cp_repo->finish(job_id, "failed",
                                    "snapshot builder not set", 1);
                            } catch (...) {}
                        }
                        return;
                    }

                    auto req = build_fn(period_use, pull_result, offer_by_ref, false,
                                        allowed_warehouses);

                    // =====================================================
                    // 6. Save
                    // =====================================================
                    emit("save", "running",
                         "{\"warehouses\":" + std::to_string(req.warehouses.size())
                         + ",\"stocks\":" + std::to_string(req.stocks.size())
                         + ",\"costs\":" + std::to_string(req.costs.size())
                         + "}");

                    if (repo_ptr == nullptr) {
                        emit("error", "failed",
                             "{\"error\":\"inventory_repo not set\"}");
                        if (g_inventory_generation.load() == my_gen) {
                            try {
                                sync_runs_repo->finish_failed(
                                    1, "inventory_pull", job_id,
                                    "inventory_repo not set", "error");
                            } catch (...) {}
                            try {
                                inv_cp_repo->finish(job_id, "failed",
                                    "inventory_repo not set", 1);
                            } catch (...) {}
                        }
                        return;
                    }

                    auto save_result = repo_ptr->import_snapshot(req);
                    if (!save_result.ok()) {
                        emit("error", "failed",
                             "{\"error\":\"" + save_result.error + "\"}");
                        if (g_inventory_generation.load() == my_gen) {
                            try {
                                sync_runs_repo->finish_failed(
                                    1, "inventory_pull", job_id,
                                    save_result.error, "save");
                            } catch (...) {}
                            try {
                                inv_cp_repo->finish(job_id, "failed",
                                    save_result.error, 1);
                            } catch (...) {}
                        }
                        return;
                    }

                    emit("save", "done",
                         "{\"stocks_accepted\":" + std::to_string(save_result.value->stocks_accepted)
                         + ",\"costs_accepted\":"  + std::to_string(save_result.value->costs_accepted)
                         + "}");

                    try {
                        inv_cp_repo->update(
                            job_id, "save", period_use, req.batch_id,
                            product_keys, offer_by_ref,
                            (long long)products_accepted,
                            (long long)products_migrated,
                            (long long)products_conflicts,
                            1);
                    } catch (const std::exception& e) {
                        std::cerr << "[inv_cp] update save failed: "
                                  << e.what() << std::endl;
                    }

                    // =====================================================
                    // 7. Финал
                    // =====================================================
                    emit("done", "imported",
                         "{\"accepted\":" + std::to_string(products_accepted)
                         + ",\"migrated\":" + std::to_string(products_migrated)
                         + ",\"conflicts\":" + std::to_string(products_conflicts)
                         + ",\"stocks_accepted\":" + std::to_string(save_result.value->stocks_accepted)
                         + ",\"costs_accepted\":" + std::to_string(save_result.value->costs_accepted)
                         + ",\"warehouses\":" + std::to_string(req.warehouses.size())
                         + ",\"measured_at\":\"" + products_measured_at + "\"}");

                    if (g_inventory_generation.load() == my_gen) {
                        try {
                            sync_runs_repo->finish_done(
                                1, "inventory_pull", job_id,
                                (long long)products_accepted,
                                (long long)products_migrated,
                                (long long)products_conflicts,
                                (long long)pull_result.warehouses.size(),
                                (long long)pull_result.stocks.size(),
                                (long long)pull_result.costs.size(),
                                (long long)save_result.value->stocks_accepted,
                                (long long)save_result.value->costs_accepted);
                        } catch (const std::exception& e) {
                            std::cerr << "[sync_runs] finish_done failed: "
                                      << e.what() << std::endl;
                        }

                        try {
                            inv_cp_repo->finish(job_id, "done", "", 1);
                        } catch (...) {}
                    }
                } catch (const std::exception& e) {
                    if (g_inventory_generation.load() == my_gen) {
                        try {
                            sync_runs_repo->finish_failed(
                                1, "inventory_pull", job_id, e.what(), "error");
                        } catch (...) {}
                        try {
                            inv_cp_repo->finish(job_id, "failed", e.what(), 1);
                        } catch (...) {}

                        try {
                            json::object obj{
                                {"job_id", job_id},
                                {"stage",  "error"},
                                {"status", "failed"},
                                {"error",  e.what()},
                                {"percent", 0},
                            };
                            InventorySnapshot snap;
                            fill_inventory_snapshot(job_id, "error", "failed", obj, snap);
                            snap.percent = 0;
                            for (auto& [k, v] : snap.stages) {
                                if (v != "done") v = "failed";
                            }
                            InventoryPullState::instance().update(snap);
                            SseBroker::instance().broadcast("inventory_pull", obj);
                        } catch (...) {}
                    }
                } catch (...) {
                    if (g_inventory_generation.load() == my_gen) {
                        try {
                            sync_runs_repo->finish_failed(
                                1, "inventory_pull", job_id, "unknown", "error");
                        } catch (...) {}
                        try {
                            inv_cp_repo->finish(job_id, "failed", "unknown", 1);
                        } catch (...) {}

                        try {
                            json::object obj{
                                {"job_id", job_id},
                                {"stage",  "error"},
                                {"status", "failed"},
                                {"error",  "unknown"},
                                {"percent", 0},
                            };
                            InventorySnapshot snap;
                            fill_inventory_snapshot(job_id, "error", "failed", obj, snap);
                            snap.percent = 0;
                            for (auto& [k, v] : snap.stages) {
                                if (v != "done") v = "failed";
                            }
                            InventoryPullState::instance().update(snap);
                            SseBroker::instance().broadcast("inventory_pull", obj);
                        } catch (...) {}
                    }
                }
            }).detach();

            r.status = http::status::accepted;
            r.body   = json::object{
                {"status", "started"},
                {"job_id", job_id},
                {"message", "Подпишитесь на /events/subscribe — там будет прогресс"},
            };
        } catch (const std::exception& e) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", e.what()}};
        } catch (...) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", "unknown"}};
        }
        return r;
    });

    // ======================= 1C INVENTORY PULLS STATUS =======================
    router.get("/api/1c/inventory-pulls/status",
               [sync_runs_repo](const RequestContext&) {
        Response r;
        try {
            auto snap = InventoryPullState::instance().snapshot();
            if (snap.has_value()) {
                const auto& s = *snap;
                json::object obj{
                    {"job_id",  s.job_id},
                    {"stage",   s.stage},
                    {"status",  s.status},
                    {"percent", s.percent},
                };
                if (s.current)         obj["current"]         = s.current;
                if (s.total)           obj["total"]           = s.total;
                if (s.accepted)        obj["accepted"]        = s.accepted;
                if (s.migrated)        obj["migrated"]        = s.migrated;
                if (s.conflicts)       obj["conflicts"]       = s.conflicts;
                if (s.warehouses)      obj["warehouses"]      = s.warehouses;
                if (s.stocks)          obj["stocks"]          = s.stocks;
                if (s.costs)           obj["costs"]           = s.costs;
                if (s.stocks_accepted) obj["stocks_accepted"] = s.stocks_accepted;
                if (s.costs_accepted)  obj["costs_accepted"]  = s.costs_accepted;
                if (!s.error.empty())       obj["error"]       = s.error;
                if (!s.measured_at.empty()) obj["measured_at"] = s.measured_at;

                json::object stages_obj;
                for (const auto& [k, v] : s.stages) stages_obj[k] = v;
                obj["stages"] = std::move(stages_obj);

                r.status = http::status::ok;
                r.body   = std::move(obj);
                return r;
            }

            std::optional<SyncRunRecord> rec;
            try {
                rec = sync_runs_repo->last_run(1, "inventory_pull");
            } catch (const std::exception& e) {
                std::cerr << "[status] last_run failed: "
                          << e.what() << std::endl;
            }

            if (!rec.has_value()) {
                r.status = http::status::ok;
                r.body   = json::object{{"stage", nullptr}};
                return r;
            }

            const auto& s = *rec;
            const bool is_done   = (s.status == "done");
            const bool is_failed = (s.status == "failed");

            json::object obj{
                {"job_id",  s.job_id},
                {"stage",   s.stage.empty()
                                ? (is_done ? "done" : (is_failed ? "error" : "start"))
                                : s.stage},
                {"status",  s.status},
                {"percent", is_done ? 100 : 0},
            };
            if (s.accepted)        obj["accepted"]        = s.accepted;
            if (s.migrated)        obj["migrated"]        = s.migrated;
            if (s.conflicts)       obj["conflicts"]       = s.conflicts;
            if (s.warehouses)      obj["warehouses"]      = s.warehouses;
            if (s.stocks)          obj["stocks"]          = s.stocks;
            if (s.costs)           obj["costs"]           = s.costs;
            if (s.stocks_accepted) obj["stocks_accepted"] = s.stocks_accepted;
            if (s.costs_accepted)  obj["costs_accepted"]  = s.costs_accepted;
            if (!s.error.empty())  obj["error"]           = s.error;

            const char* wh_state = is_done ? "done" : (is_failed ? "failed" : "pending");
            json::object stages_obj;
            stages_obj["catalog"]  = wh_state;
            stages_obj["ka2_pull"] = wh_state;
            stages_obj["save"]     = wh_state;
            stages_obj["done"]     = wh_state;
            obj["stages"] = std::move(stages_obj);

            r.status = http::status::ok;
            r.body   = std::move(obj);
        } catch (const std::exception& e) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", e.what()}};
        } catch (...) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", "unknown"}};
        }
        return r;
    });

    // ======================= RESUME INFO =======================
    router.get("/api/1c/inventory-pulls/resume-info",
               [inv_cp_repo](const RequestContext&) {
        Response r;
        try {
            auto cp = inv_cp_repo->find_unfinished(1);
            if (!cp.has_value()) {
                r.status = http::status::ok;
                r.body   = json::object{{"found", false}};
                return r;
            }
            const auto& c = *cp;
            r.status = http::status::ok;
            r.body   = json::object{
                {"found",              true},
                {"job_id",             c.job_id},
                {"last_stage",         c.last_stage},
                {"period",             c.period},
                {"batch_id",           c.batch_id},
                {"products_accepted",  c.products_accepted},
                {"products_migrated",  c.products_migrated},
                {"products_conflicts", c.products_conflicts},
                {"updated_at",         c.updated_at},
            };
        } catch (const std::exception& e) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", e.what()}};
        } catch (...) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", "unknown"}};
        }
        return r;
    });

    // ======================= SYNC RESET =======================
    router.post("/api/sync/reset",
               [sync_runs_repo, inv_cp_repo, &sync](const RequestContext&) {
        Response r;
        try {
            sync_runs_repo->abandon_running(1, "inventory_pull", "reset by user");
            sync_runs_repo->abandon_running(1, "ozon",           "reset by user");
            inv_cp_repo->abandon_running("reset by user", 1);

            InventoryPullState::instance().clear();
            sync.reset_ozon_progress();

            ++g_inventory_generation;

            r.status = http::status::ok;
            r.body   = json::object{{"status", "reset"}};
        } catch (const std::exception& e) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", e.what()}};
        } catch (...) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", "unknown"}};
        }
        return r;
    });

    // ======================= LAST SUCCESSFUL SYNC =======================
    router.get("/api/sync/last-success",
               [sync_runs_repo](const RequestContext& ctx) {
        Response r;
        try {
            std::string source = "inventory_pull";
            auto it = ctx.query_params.find("source");
            if (it != ctx.query_params.end() && !it->second.empty()) {
                source = it->second;
            }

            auto rec = sync_runs_repo->last_success(1, source);
            if (!rec.has_value()) {
                r.status = http::status::ok;
                r.body   = json::object{{"found", false}};
                return r;
            }

            const auto& s = *rec;
            r.status = http::status::ok;
            r.body   = json::object{
                {"found",           true},
                {"id",              s.id},
                {"source",          s.source},
                {"job_id",          s.job_id},
                {"status",          s.status},
                {"stage",           s.stage},
                {"started_at",      s.started_at},
                {"finished_at",     s.finished_at},
                {"duration_ms",     s.duration_ms},
                {"accepted",        s.accepted},
                {"migrated",        s.migrated},
                {"conflicts",       s.conflicts},
                {"warehouses",      s.warehouses},
                {"stocks",          s.stocks},
                {"costs",           s.costs},
                {"stocks_accepted", s.stocks_accepted},
                {"costs_accepted",  s.costs_accepted},
            };
        } catch (const std::exception& e) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", e.what()}};
        } catch (...) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", "unknown"}};
        }
        return r;
    });

    // ======================= LAST RUN (any status) =======================
    router.get("/api/sync/last-run",
               [sync_runs_repo](const RequestContext& ctx) {
        Response r;
        try {
            std::string source = "inventory_pull";
            auto it = ctx.query_params.find("source");
            if (it != ctx.query_params.end() && !it->second.empty()) {
                source = it->second;
            }

            auto rec = sync_runs_repo->last_run(1, source);
            if (!rec.has_value()) {
                r.status = http::status::ok;
                r.body   = json::object{{"found", false}};
                return r;
            }

            const auto& s = *rec;
            r.status = http::status::ok;
            r.body   = json::object{
                {"found",           true},
                {"id",              s.id},
                {"source",          s.source},
                {"job_id",          s.job_id},
                {"status",          s.status},
                {"stage",           s.stage},
                {"started_at",      s.started_at},
                {"finished_at",     s.finished_at},
                {"duration_ms",     s.duration_ms},
                {"accepted",        s.accepted},
                {"stocks_accepted", s.stocks_accepted},
                {"costs_accepted",  s.costs_accepted},
                {"error",           s.error},
            };
        } catch (const std::exception& e) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", e.what()}};
        } catch (...) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", "unknown"}};
        }
        return r;
    });

    // ======================= WAREHOUSES: ALLOWED =======================
    router.get("/api/warehouses/allowed",
               [allowed_wh_repo, db_pool](const RequestContext&) {
        Response r;
        try {
            auto list = allowed_wh_repo->list(1);

            std::unordered_map<std::string, std::string> names;
            {
                auto lease = db_pool->acquire();
                pqxx::work tx(lease.get());
                auto res = tx.exec(
                    "SELECT warehouse_id, warehouse_name "
                    "  FROM inventory1c_warehouses WHERE tenant_id = 1");
                for (const auto& row : res) {
                    names[row[0].as<std::string>()] = row[1].as<std::string>();
                }
            }

            json::array items;
            for (const auto& w : list) {
                auto it = names.find(w.warehouse_id);
                items.push_back(json::object{
                    {"warehouse_id",   w.warehouse_id},
                    {"warehouse_name", it != names.end() ? it->second : ""},
                    {"enabled",        w.enabled},
                    {"note",           w.note},
                });
            }
            r.status = http::status::ok;
            r.body   = json::object{{"items", std::move(items)}};
        } catch (const std::exception& e) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", e.what()}};
        } catch (...) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", "unknown"}};
        }
        return r;
    });

    router.get("/api/warehouses/known",
               [allowed_wh_repo, db_pool](const RequestContext&) {
        Response r;
        try {
            std::unordered_set<std::string> enabled =
                allowed_wh_repo->enabled_ids(1);

            auto lease = db_pool->acquire();
            pqxx::work tx(lease.get());
            auto res = tx.exec(
                "SELECT warehouse_id, warehouse_name "
                "  FROM inventory1c_warehouses WHERE tenant_id = 1 "
                " ORDER BY warehouse_name");

            json::array items;
            for (const auto& row : res) {
                const std::string id   = row[0].as<std::string>();
                const std::string name = row[1].as<std::string>();
                items.push_back(json::object{
                    {"warehouse_id",   id},
                    {"warehouse_name", name},
                    {"enabled",        enabled.find(id) != enabled.end()},
                });
            }
            r.status = http::status::ok;
            r.body   = json::object{{"items", std::move(items)}};
        } catch (const std::exception& e) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", e.what()}};
        } catch (...) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", "unknown"}};
        }
        return r;
    });

    router.post("/api/warehouses/allowed",
                [allowed_wh_repo](const RequestContext& ctx) {
        Response r;
        try {
            auto it_id = ctx.query_params.find("warehouse_id");
            if (it_id == ctx.query_params.end() || it_id->second.empty()) {
                r.status = http::status::bad_request;
                r.body   = json::object{{"error", "warehouse_id is required"}};
                return r;
            }
            bool enabled = true;
            auto it_en = ctx.query_params.find("enabled");
            if (it_en != ctx.query_params.end()) {
                enabled = (it_en->second == "1" || it_en->second == "true");
            }
            std::string note;
            auto it_note = ctx.query_params.find("note");
            if (it_note != ctx.query_params.end()) note = it_note->second;

            allowed_wh_repo->upsert(it_id->second, enabled, note, 1);
            r.status = http::status::ok;
            r.body   = json::object{{"status", "ok"}};
        } catch (const std::exception& e) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", e.what()}};
        } catch (...) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", "unknown"}};
        }
        return r;
    });

    router.del("/api/warehouses/allowed",
               [allowed_wh_repo](const RequestContext& ctx) {
        Response r;
        try {
            auto it = ctx.query_params.find("warehouse_id");
            if (it == ctx.query_params.end() || it->second.empty()) {
                r.status = http::status::bad_request;
                r.body   = json::object{{"error", "warehouse_id is required"}};
                return r;
            }
            allowed_wh_repo->remove(it->second, 1);
            r.status = http::status::ok;
            r.body   = json::object{{"status", "ok"}};
        } catch (const std::exception& e) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", e.what()}};
        } catch (...) {
            r.status = http::status::internal_server_error;
            r.body   = json::object{{"error", "unknown"}};
        }
        return r;
    });
}
