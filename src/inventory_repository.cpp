#include "inventory_repository.hpp"
#include "sse_broker.hpp"
#include "onec_raw_log.hpp"

#include <boost/json.hpp>
#include <pqxx/pqxx>
#include <openssl/sha.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace json = boost::json;

namespace scand::inventory {

// ===========================================================================
// Утилиты
// ===========================================================================
namespace {

std::string sha256_hex(const std::string& data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()),
           data.size(), hash);
    std::ostringstream oss;
    for (unsigned char c : hash) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(c);
    }
    return oss.str();
}

// Канонический JSON: массивы отсортированы, компактно.
// warehouse_name в stocks НЕ участвует — он живёт в справочнике,
// и не должен влиять на payload_hash.
std::string canonical_json(const SnapshotRequest& req) {
    std::vector<const StockInput*> st;
    st.reserve(req.stocks.size());
    for (const auto& s : req.stocks) st.push_back(&s);
    std::sort(st.begin(), st.end(),
        [](const StockInput* a, const StockInput* b) {
            if (a->sku_1c != b->sku_1c) return a->sku_1c < b->sku_1c;
            return a->warehouse_id < b->warehouse_id;
        });

    std::vector<const CostInput*> co;
    co.reserve(req.costs.size());
    for (const auto& c : req.costs) co.push_back(&c);
    std::sort(co.begin(), co.end(),
        [](const CostInput* a, const CostInput* b) {
            return a->sku_1c < b->sku_1c;
        });

    std::vector<const WarehouseInput*> wa;
    wa.reserve(req.warehouses.size());
    for (const auto& w : req.warehouses) wa.push_back(&w);
    std::sort(wa.begin(), wa.end(),
        [](const WarehouseInput* a, const WarehouseInput* b) {
            return a->warehouse_id < b->warehouse_id;
        });

    auto opt_str = [](const std::optional<std::string>& v) {
        if (v && !v->empty()) return json::value(*v);
        return json::value(nullptr);
    };
    auto opt_num = [](const std::optional<double>& v) {
        if (v) return json::value(*v);
        return json::value(nullptr);
    };

    json::array stocks_json;
    for (const auto* s : st) {
        stocks_json.push_back(json::object{
            {"sku_1c",       s->sku_1c},
            {"offer_id",     opt_str(s->offer_id)},
            {"warehouse_id", s->warehouse_id},
            {"available",    s->available},
            {"reserved",     s->reserved},
        });
    }

    json::array costs_json;
    for (const auto* c : co) {
        costs_json.push_back(json::object{
            {"sku_1c",            c->sku_1c},
            {"batch_avg_cost",    opt_num(c->batch_avg_cost)},
            {"purchase_cost",     opt_num(c->purchase_cost)},
            {"purchase_source",   opt_str(c->purchase_source)},
            {"purchase_source_at",opt_str(c->purchase_source_at)},
            {"dealer_cost",       opt_num(c->dealer_cost)},
        });
    }

    json::array warehouses_json;
    for (const auto* w : wa) {
        warehouses_json.push_back(json::object{
            {"warehouse_id",   w->warehouse_id},
            {"warehouse_name", w->warehouse_name},
        });
    }

    json::object body{
        {"batch_id",    req.batch_id},
        {"measured_at", req.measured_at},
        {"mode",        req.mode},
        {"stocks",      std::move(stocks_json)},
        {"warehouses",  std::move(warehouses_json)},
        {"costs",       std::move(costs_json)},
    };

    return json::serialize(body);
}

} // namespace

// ===========================================================================
// Конструктор
// ===========================================================================
InventoryRepository::InventoryRepository(std::shared_ptr<db::ConnectionPool> pool)
    : pool_(std::move(pool)) {}

// ===========================================================================
// import_snapshot
// ===========================================================================
scand::Result<ImportResult> InventoryRepository::import_snapshot(
    const SnapshotRequest& req,
    long long tenant_id) const
{
    try {
        if (req.stocks.empty() && req.costs.empty()) {
            return scand::Result<ImportResult>::failure(
                "Снимок не может быть пустым");
        }
        if (req.batch_id.empty() || req.batch_id.size() > 128) {
            return scand::Result<ImportResult>::failure(
                "batch_id обязателен (1..128)");
        }
        if (req.measured_at.empty()) {
            return scand::Result<ImportResult>::failure(
                "measured_at обязателен");
        }

        std::string payload_hash = sha256_hex(canonical_json(req));

        auto lease = pool_->acquire();
        pqxx::work tx(lease.get());

        // --- проверка batch_id ---
        auto existing = tx.exec_params(
            "SELECT payload_hash FROM inventory1c_snapshots "
            "WHERE tenant_id = $1 AND batch_id = $2",
            tenant_id, req.batch_id);

        if (!existing.empty()) {
            std::string existing_hash = existing[0][0].as<std::string>("");
            tx.commit();

            if (existing_hash == payload_hash) {
                ImportResult r;
                r.status   = "duplicate";
                r.batch_id = req.batch_id;
                return scand::Result<ImportResult>::success(std::move(r));
            }

            return scand::Result<ImportResult>::failure(
                "batch_id уже использован для другого содержимого");
        }

        // --- очистка ---
        tx.exec_params(
            "DELETE FROM pricing_cost_batch    WHERE tenant_id = $1", tenant_id);
        tx.exec_params(
            "DELETE FROM pricing_cost_supplier WHERE tenant_id = $1", tenant_id);

        // =====================================================================
        // ВАЖНО: warehouses вставляем ПЕРЕД stocks — FK на (tenant_id, warehouse_id)
        // =====================================================================
        if (!req.warehouses.empty()) {
            tx.exec("CREATE TEMP TABLE tmp_wh ("
                    "  warehouse_id   VARCHAR(64), "
                    "  warehouse_name VARCHAR(255)"
                    ") ON COMMIT DROP");

            auto stream = pqxx::stream_to::table(tx, "tmp_wh",
                {"warehouse_id", "warehouse_name"});

            for (const auto& w : req.warehouses) {
                stream << std::make_tuple(w.warehouse_id, w.warehouse_name);
            }
            stream.complete();

            // ON CONFLICT DO UPDATE — при переименовании склада в 1С
            // имя в справочнике обновляется, FK не падает.
            tx.exec_params(
                "INSERT INTO inventory1c_warehouses"
                "(tenant_id, warehouse_id, warehouse_name) "
                "SELECT $1, warehouse_id, warehouse_name FROM tmp_wh "
                "ON CONFLICT (tenant_id, warehouse_id) DO UPDATE "
                "   SET warehouse_name = EXCLUDED.warehouse_name, "
                "       updated_at     = NOW()",
                tenant_id);
        }

        // --- stocks (без warehouse_name) ---
        if (!req.stocks.empty()) {
            tx.exec("CREATE TEMP TABLE tmp_stocks ("
                    "  sku_1c         VARCHAR(128), "
                    "  offer_id       VARCHAR(128), "
                    "  warehouse_id   VARCHAR(64),  "
                    "  available      BIGINT, "
                    "  reserved       BIGINT"
                    ") ON COMMIT DROP");

            auto stream = pqxx::stream_to::table(tx, "tmp_stocks",
                {
                    "sku_1c", "offer_id", "warehouse_id",
                    "available", "reserved"});

            for (const auto& s : req.stocks) {
                std::optional<std::string> off;
                if (s.offer_id && !s.offer_id->empty()) off = *s.offer_id;

                stream << std::make_tuple(
                    s.sku_1c,
                    off,
                    s.warehouse_id,
                    s.available,
                    s.reserved);
            }
            stream.complete();

            tx.exec_params(
                "INSERT INTO inventory1c_stocks"
                "(tenant_id, sku_1c, offer_id, warehouse_id, "
                " available, reserved, measured_at, batch_id) "
                "SELECT $1, sku_1c, offer_id, warehouse_id, "
                "       available, reserved, $2::timestamptz, $3 "
                "FROM tmp_stocks",
                tenant_id, req.measured_at, req.batch_id);
        }

        // --- costs ---
        for (const auto& c : req.costs) {
            if (c.batch_avg_cost && *c.batch_avg_cost > 0) {
                tx.exec_params(
                    "INSERT INTO pricing_cost_batch"
                    "(tenant_id, sku_1c, cost) VALUES ($1, $2, $3) "
                    "ON CONFLICT (tenant_id, sku_1c) DO UPDATE SET "
                    "  cost = EXCLUDED.cost, updated_at = NOW()",
                    tenant_id, c.sku_1c, *c.batch_avg_cost);
            }

            if (c.purchase_cost || c.dealer_cost) {
                if (c.purchase_cost) {
                    tx.exec_params(
                        "INSERT INTO pricing_cost_supplier"
                        "(tenant_id, sku_1c, purchase, purchase_source, "
                        " purchase_source_at, dealer) "
                        "VALUES ($1, $2, $3, $4, $5::timestamptz, $6) "
                        "ON CONFLICT (tenant_id, sku_1c) DO UPDATE SET "
                        "  purchase = EXCLUDED.purchase, "
                        "  purchase_source = EXCLUDED.purchase_source, "
                        "  purchase_source_at = EXCLUDED.purchase_source_at, "
                        "  dealer = EXCLUDED.dealer, "
                        "  updated_at = NOW()",
                        tenant_id, c.sku_1c, *c.purchase_cost,
                        c.purchase_source.value_or(""),
                        c.purchase_source_at.value_or(""),
                        c.dealer_cost.value_or(0.0));
                }
            }
        }

        // --- snapshot ---
        tx.exec_params(
            "INSERT INTO inventory1c_snapshots"
            "(tenant_id, batch_id, payload_hash, measured_at, "
            " stocks_count, costs_count) "
            "VALUES ($1, $2, $3, $4::timestamptz, $5, $6)",
            tenant_id, req.batch_id, payload_hash, req.measured_at,
            static_cast<int>(req.stocks.size()),
            static_cast<int>(req.costs.size()));

        tx.commit();

        try {
            SseBroker::instance().broadcast("ka2_progress", json::object{
                {"source",   "ka2"},
                {"stage",    "save"},
                {"status",   "done"},
                {"stocks",   static_cast<long long>(req.stocks.size())},
                {"costs",    static_cast<long long>(req.costs.size())},
                {"batch_id", req.batch_id},
            });
        } catch (...) {}

        ImportResult r;
        r.status          = "imported";
        r.batch_id        = req.batch_id;
        r.stocks_accepted = req.stocks.size();
        r.costs_accepted  = req.costs.size();
        return scand::Result<ImportResult>::success(std::move(r));
    } catch (const std::exception& e) {
        try {
            SseBroker::instance().broadcast("ka2_progress", json::object{
                {"source",   "ka2"},
                {"stage",    "save"},
                {"status",   "failed"},
                {"error",    e.what()},
                {"batch_id", req.batch_id},
            });
        } catch (...) {}

        OnecRawLog::instance().add(
            std::string("DB import_snapshot error: ") + e.what());
        return scand::Result<ImportResult>::failure(e.what());
    }
}

// ===========================================================================
// cleanup_older_than
// ===========================================================================
scand::VoidResult InventoryRepository::cleanup_older_than(int days) const {
    try {
        auto lease = pool_->acquire();
        pqxx::work tx(lease.get());

        tx.exec_params(
            "DELETE FROM inventory1c_snapshots "
            "WHERE created_at < NOW() - ($1 || ' days')::interval",
            std::to_string(days));
        tx.commit();
        return scand::VoidResult::ok();
    } catch (const std::exception& e) {
        return scand::VoidResult::fail(e.what());
    }
}

} // namespace scand::inventory
