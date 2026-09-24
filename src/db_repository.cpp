#include "db_repository.hpp"
#include "time_utils.hpp"
#include "onec_raw_log.hpp"
#include <pqxx/pqxx>
#include <iostream>
#include <tuple>

DbRepository::DbRepository(std::shared_ptr<scand::db::ConnectionPool> pool)
    : pool_(std::move(pool)) {}

// ---------------------------------------------------------------------------
// 1С
// ---------------------------------------------------------------------------
scand::VoidResult DbRepository::upsert_batch_1c(const std::vector<Product1C>& products) {
    if (products.empty()) return scand::VoidResult::ok();
    try {
        auto lease = pool_->acquire();
        pqxx::work tx(lease.get());

        tx.exec("CREATE TEMP TABLE tmp_products_1c ("
                "  id TEXT, offer_id TEXT, name TEXT, price NUMERIC(12,2)"
                ") ON COMMIT DROP");

        pqxx::stream_to stream(tx, "tmp_products_1c",
            std::vector<std::string>{"id", "offer_id", "name", "price"});

        for (const auto& p : products)
            stream << std::make_tuple(p.id, p.offer_id, p.name, p.price);
        stream.complete();

        tx.exec(
            "INSERT INTO products_1c(id, offer_id, name, price, updated_at_db) "
            "SELECT id, offer_id, name, price, NOW() FROM tmp_products_1c "
            "ON CONFLICT (id) DO UPDATE SET "
            "  offer_id = EXCLUDED.offer_id, "
            "  name = EXCLUDED.name, "
            "  price = EXCLUDED.price, "
            "  updated_at_db = NOW()");
        tx.commit();
        std::cout << "[DB-1C] Batch upserted " << products.size() << std::endl;
        return scand::VoidResult::ok();
    } catch (const std::exception& e) {
        OnecRawLog::instance().add(std::string("DB upsert_batch_1c: ") + e.what());
        return scand::VoidResult::fail(e.what());
    }
}

scand::VoidResult DbRepository::upsert_one_1c(const Product1C& p) {
    try {
        auto lease = pool_->acquire();
        pqxx::work tx(lease.get());

        tx.exec_params(
            "INSERT INTO products_1c(id, offer_id, name, price, updated_at_db) "
            "VALUES($1, $2, $3, $4, NOW()) "
            "ON CONFLICT (id) DO UPDATE SET "
            "  offer_id = EXCLUDED.offer_id, name = EXCLUDED.name, "
            "  price = EXCLUDED.price, updated_at_db = NOW()",
            p.id, p.offer_id, p.name, p.price);
        tx.commit();
        return scand::VoidResult::ok();
    } catch (const std::exception& e) {
        OnecRawLog::instance().add(std::string("DB upsert_one_1c: ") + e.what());
        return scand::VoidResult::fail(e.what());
    }
}

// ---------------------------------------------------------------------------
// Ozon
// ---------------------------------------------------------------------------
scand::VoidResult DbRepository::upsert_batch_ozon(const std::vector<OzonProduct>& products) {
    if (products.empty()) return scand::VoidResult::ok();
    try {
        auto lease = pool_->acquire();
        pqxx::work tx(lease.get());

        tx.exec("CREATE TEMP TABLE tmp_products ("
                "  id TEXT, sku TEXT, name TEXT, price NUMERIC(12,2), "
                "  currency TEXT, in_stock INT, description TEXT, weight INT, "
                "  created_at TEXT, updated_at TEXT, scategorie TEXT, related_products TEXT"
                ") ON COMMIT DROP");

        pqxx::stream_to stream(tx, "tmp_products",
            std::vector<std::string>{
                "id", "sku", "name", "price", "currency", "in_stock",
                "description", "weight", "created_at", "updated_at",
                "scategorie", "related_products"});

        for (const auto& p : products) {
            stream << std::make_tuple(
                p.id, p.sku, p.name, p.price, p.currency, p.in_stock,
                p.description, p.weight, p.created_at, p.updated_at,
                p.scategorie, p.related_products);
        }
        stream.complete();

        tx.exec(
            "INSERT INTO products(id, sku, name, price, currency, in_stock, "
            "  description, weight, created_at, updated_at, scategorie, related_products, updated_at_db) "
            "SELECT id, sku, name, price, currency, in_stock, "
            "  description, weight, created_at, updated_at, scategorie, related_products, NOW() FROM tmp_products "
            "ON CONFLICT (id) DO UPDATE SET "
            "  sku = EXCLUDED.sku, name = EXCLUDED.name, price = EXCLUDED.price, "
            "  currency = EXCLUDED.currency, in_stock = EXCLUDED.in_stock, "
            "  description = EXCLUDED.description, weight = EXCLUDED.weight, "
            "  created_at = EXCLUDED.created_at, updated_at = EXCLUDED.updated_at, "
            "  scategorie = EXCLUDED.scategorie, related_products = EXCLUDED.related_products, "
            "  updated_at_db = NOW()");
        tx.commit();
        std::cout << "[DB-OZON] Batch upserted " << products.size() << std::endl;
        return scand::VoidResult::ok();
    } catch (const std::exception& e) {
        OnecRawLog::instance().add(std::string("DB upsert_batch_ozon: ") + e.what());
        return scand::VoidResult::fail(e.what());
    }
}

scand::VoidResult DbRepository::upsert_one_ozon(const OzonProduct& p) {
    try {
        auto lease = pool_->acquire();
        pqxx::work tx(lease.get());

        tx.exec_params(
            "INSERT INTO products(id, sku, name, price, currency, in_stock, "
            "  description, weight, created_at, updated_at, scategorie, related_products, updated_at_db) "
            "VALUES($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, NOW()) "
            "ON CONFLICT (id) DO UPDATE SET "
            "  sku = EXCLUDED.sku, name = EXCLUDED.name, price = EXCLUDED.price, "
            "  currency = EXCLUDED.currency, in_stock = EXCLUDED.in_stock, "
            "  description = EXCLUDED.description, weight = EXCLUDED.weight, "
            "  created_at = EXCLUDED.created_at, updated_at = EXCLUDED.updated_at, "
            "  scategorie = EXCLUDED.scategorie, related_products = EXCLUDED.related_products, "
            "  updated_at_db = NOW()",
            p.id, p.sku, p.name, p.price, p.currency, p.in_stock,
            p.description, p.weight, p.created_at, p.updated_at,
            p.scategorie, p.related_products);
        tx.commit();
        return scand::VoidResult::ok();
    } catch (const std::exception& e) {
        OnecRawLog::instance().add(std::string("DB upsert_one_ozon: ") + e.what());
        return scand::VoidResult::fail(e.what());
    }
}

// ---------------------------------------------------------------------------
// Чекпоинты
// ---------------------------------------------------------------------------
SyncCheckpoint DbRepository::load_checkpoint(const std::string& source) {
    SyncCheckpoint cp;
    cp.source = source;
    try {
        auto lease = pool_->acquire();
        pqxx::work tx(lease.get());

        auto res = tx.exec_params(
            "SELECT last_cursor, last_page, total_synced, COALESCE(last_status, '') "
            "FROM sync_state WHERE source = $1",
            source);
        if (!res.empty()) {
            cp.last_cursor  = res[0][0].as<std::string>("");
            cp.last_page    = res[0][1].as<int>(0);
            cp.total_synced = res[0][2].as<long long>(0);
            cp.last_status  = res[0][3].as<std::string>("");
        }
    } catch (const std::exception& e) {
        OnecRawLog::instance().add(std::string("DB load_checkpoint: ") + e.what());
    }
    return cp;
}

scand::VoidResult DbRepository::save_checkpoint(const SyncCheckpoint& cp) {
    try {
        auto lease = pool_->acquire();
        pqxx::work tx(lease.get());

        tx.exec_params(
            "INSERT INTO sync_state(source, last_cursor, last_page, total_synced, "
            "                       last_status, updated_at) "
            "VALUES($1, $2, $3, $4, $5, NOW()) "
            "ON CONFLICT (source) DO UPDATE SET "
            "  last_cursor = EXCLUDED.last_cursor, "
            "  last_page = EXCLUDED.last_page, "
            "  total_synced = EXCLUDED.total_synced, "
            "  last_status = EXCLUDED.last_status, "
            "  updated_at = NOW()",
            cp.source, cp.last_cursor, cp.last_page, cp.total_synced,
            cp.last_status);
        tx.commit();
        return scand::VoidResult::ok();
    } catch (const std::exception& e) {
        OnecRawLog::instance().add(std::string("DB save_checkpoint: ") + e.what());
        return scand::VoidResult::fail(e.what());
    }
}

scand::VoidResult DbRepository::reset_checkpoint(const std::string& source) {
    try {
        auto lease = pool_->acquire();
        pqxx::work tx(lease.get());

        tx.exec_params(
            "UPDATE sync_state SET last_cursor = '', last_page = 0, "
            "total_synced = 0, last_status = '', updated_at = NOW() "
            "WHERE source = $1",
            source);
        tx.commit();
        std::cout << "[DB] Checkpoint reset for " << source << std::endl;
        return scand::VoidResult::ok();
    } catch (const std::exception& e) {
        OnecRawLog::instance().add(std::string("DB reset_checkpoint: ") + e.what());
        return scand::VoidResult::fail(e.what());
    }
}

scand::VoidResult DbRepository::update_checkpoint_status(const std::string& source,
                                                         const std::string& status) {
    try {
        auto lease = pool_->acquire();
        pqxx::work tx(lease.get());

        tx.exec_params(
            "UPDATE sync_state SET last_status = $1, updated_at = NOW() "
            "WHERE source = $2",
            status, source);
        tx.commit();
        return scand::VoidResult::ok();
    } catch (const std::exception& e) {
        OnecRawLog::instance().add(std::string("DB update_checkpoint_status: ") + e.what());
        return scand::VoidResult::fail(e.what());
    }
}
