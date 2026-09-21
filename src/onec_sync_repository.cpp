#include "onec_sync_repository.hpp"
#include "onec_raw_log.hpp"
#include <iostream>
#include <pqxx/pqxx>
#include <stdexcept>

OnecSyncRepository::OnecSyncRepository(std::string conn_str)
    : conn_str_(std::move(conn_str)) {}

// ---------------------------------------------------------------------------
// Состояние
// ---------------------------------------------------------------------------
bool OnecSyncRepository::load_state(const std::string& source,
                                    OnecSyncState& out) const
{
    try {
        pqxx::connection conn(conn_str_);
        pqxx::work tx(conn);

        auto res = tx.exec_params(
            "SELECT url, catalog, page_size, total_pages, accepted, "
            "       to_char(started_at, 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
            "       to_char(updated_at, 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
            "       status "
            "FROM onec_sync_state WHERE source = $1",
            source
        );

        if (res.empty()) return false;

        const auto& row = res[0];
        out.source      = source;
        out.url         = row[0].as<std::string>("");
        out.catalog     = row[1].as<std::string>("");
        out.page_size   = row[2].as<int>(0);
        out.total_pages = row[3].as<int>(0);
        out.accepted    = static_cast<size_t>(row[4].as<long long>(0));
        out.started_at  = row[5].as<std::string>("");
        out.updated_at  = row[6].as<std::string>("");
        out.status      = row[7].as<std::string>("");
        return true;

    } catch (const std::exception& e) {
        OnecRawLog::instance().add(
            std::string("DB load_state error: ") + e.what()
        );
        return false;
    }
}

void OnecSyncRepository::save_state(const OnecSyncState& state) const {
    try {
        pqxx::connection conn(conn_str_);
        pqxx::work tx(conn);

        tx.exec_params(
            "INSERT INTO onec_sync_state(source, url, catalog, page_size, "
            "                            total_pages, accepted, started_at, "
            "                            updated_at, status) "
            "VALUES($1, $2, $3, $4, $5, $6, "
            "       COALESCE($7::timestamptz, NOW()), NOW(), $8) "
            "ON CONFLICT (source) DO UPDATE SET "
            "  url = EXCLUDED.url, "
            "  catalog = EXCLUDED.catalog, "
            "  page_size = EXCLUDED.page_size, "
            "  total_pages = EXCLUDED.total_pages, "
            "  accepted = EXCLUDED.accepted, "
            "  updated_at = NOW(), "
            "  status = EXCLUDED.status",
            state.source, state.url, state.catalog,
            state.page_size, state.total_pages,
            static_cast<long long>(state.accepted),
            state.started_at.empty() ? nullptr : state.started_at,
            state.status
        );
        tx.commit();

    } catch (const std::exception& e) {
        OnecRawLog::instance().add(
            std::string("DB save_state error: ") + e.what()
        );
    }
}

void OnecSyncRepository::update_state_status(const std::string& source,
                                             const std::string& status) const
{
    try {
        pqxx::connection conn(conn_str_);
        pqxx::work tx(conn);

        tx.exec_params(
            "UPDATE onec_sync_state SET status = $1, updated_at = NOW() "
            "WHERE source = $2",
            status, source
        );
        tx.commit();
    } catch (const std::exception& e) {
        OnecRawLog::instance().add(
            std::string("DB update_state_status error: ") + e.what()
        );
    }
}

void OnecSyncRepository::add_accepted(const std::string& source,
                                      size_t delta) const
{
    try {
        pqxx::connection conn(conn_str_);
        pqxx::work tx(conn);

        tx.exec_params(
            "UPDATE onec_sync_state SET accepted = accepted + $1, "
            "       updated_at = NOW() WHERE source = $2",
            static_cast<long long>(delta), source
        );
        tx.commit();
    } catch (const std::exception& e) {
        OnecRawLog::instance().add(
            std::string("DB add_accepted error: ") + e.what()
        );
    }
}

// ---------------------------------------------------------------------------
// Страницы
// ---------------------------------------------------------------------------
std::set<int> OnecSyncRepository::get_completed_pages(
    const std::string& source) const
{
    std::set<int> result;
    try {
        pqxx::connection conn(conn_str_);
        pqxx::work tx(conn);

        auto res = tx.exec_params(
            "SELECT page_idx FROM onec_sync_pages "
            "WHERE source = $1 AND status = 'completed'",
            source
        );
        for (const auto& row : res) result.insert(row[0].as<int>());

    } catch (const std::exception& e) {
        OnecRawLog::instance().add(
            std::string("DB get_completed_pages error: ") + e.what()
        );
    }
    return result;
}

std::set<int> OnecSyncRepository::get_failed_pages(
    const std::string& source) const
{
    std::set<int> result;
    try {
        pqxx::connection conn(conn_str_);
        pqxx::work tx(conn);

        auto res = tx.exec_params(
            "SELECT page_idx FROM onec_sync_pages "
            "WHERE source = $1 AND status = 'failed'",
            source
        );
        for (const auto& row : res) result.insert(row[0].as<int>());

    } catch (const std::exception& e) {
        OnecRawLog::instance().add(
            std::string("DB get_failed_pages error: ") + e.what()
        );
    }
    return result;
}

void OnecSyncRepository::mark_page_completed(const std::string& source,
                                             int page_idx, int accepted) const
{
    try {
        pqxx::connection conn(conn_str_);
        pqxx::work tx(conn);

        tx.exec_params(
            "INSERT INTO onec_sync_pages(source, page_idx, status, accepted, updated_at) "
            "VALUES($1, $2, 'completed', $3, NOW()) "
            "ON CONFLICT (source, page_idx) DO UPDATE SET "
            "  status = 'completed', accepted = EXCLUDED.accepted, updated_at = NOW()",
            source, page_idx, accepted
        );
        tx.commit();
    } catch (const std::exception& e) {
        OnecRawLog::instance().add(
            std::string("DB mark_page_completed error: ") + e.what()
        );
    }
}

void OnecSyncRepository::mark_page_failed(const std::string& source,
                                          int page_idx) const
{
    try {
        pqxx::connection conn(conn_str_);
        pqxx::work tx(conn);

        tx.exec_params(
            "INSERT INTO onec_sync_pages(source, page_idx, status, updated_at) "
            "VALUES($1, $2, 'failed', NOW()) "
            "ON CONFLICT (source, page_idx) DO UPDATE SET "
            "  status = 'failed', updated_at = NOW()",
            source, page_idx
        );
        tx.commit();
    } catch (const std::exception& e) {
        OnecRawLog::instance().add(
            std::string("DB mark_page_failed error: ") + e.what()
        );
    }
}

// ---------------------------------------------------------------------------
// Записи
// ---------------------------------------------------------------------------
std::set<std::string> OnecSyncRepository::get_existing_refs(
    const std::string& source) const
{
    std::set<std::string> result;
    try {
        pqxx::connection conn(conn_str_);
        pqxx::work tx(conn);

        auto res = tx.exec_params(
            "SELECT ref_key FROM onec_sync_records WHERE source = $1",
            source
        );
        for (const auto& row : res) result.insert(row[0].as<std::string>());

    } catch (const std::exception& e) {
        OnecRawLog::instance().add(
            std::string("DB get_existing_refs error: ") + e.what()
        );
    }
    return result;
}

void OnecSyncRepository::insert_records_batch(
    const std::string& source,
    const std::vector<OnecCatalogRecord>& records) const
{
    if (records.empty()) return;

    try {
        pqxx::connection conn(conn_str_);
        pqxx::work tx(conn);

        tx.exec("CREATE TEMP TABLE tmp_records ("
                "  source TEXT, ref_key TEXT, code TEXT, "
                "  article TEXT, name TEXT, measured_at TEXT"
                ") ON COMMIT DROP");

        pqxx::stream_to stream(tx, "tmp_records",
            std::vector<std::string>{"source", "ref_key", "code",
                                     "article", "name", "measured_at"});

        for (const auto& r : records) {
            stream << std::make_tuple(source, r.ref_key, r.code,
                                      r.article, r.name, r.measured_at);
        }
        stream.complete();

        tx.exec(
            "INSERT INTO onec_sync_records(source, ref_key, code, article, name, measured_at) "
            "SELECT source, ref_key, code, article, name, "
            "       NULLIF(measured_at, '')::timestamptz "
            "FROM tmp_records "
            "ON CONFLICT (source, ref_key) DO UPDATE SET "
            "  code = EXCLUDED.code, article = EXCLUDED.article, "
            "  name = EXCLUDED.name, measured_at = EXCLUDED.measured_at"
        );
        tx.commit();

    } catch (const std::exception& e) {
        OnecRawLog::instance().add(
            std::string("DB insert_records_batch error: ") + e.what()
        );
        throw;
    }
}

// ---------------------------------------------------------------------------
// Сброс
// ---------------------------------------------------------------------------
void OnecSyncRepository::reset_all(const std::string& source) const {
    try {
        pqxx::connection conn(conn_str_);
        pqxx::work tx(conn);

        tx.exec_params("DELETE FROM onec_sync_pages WHERE source = $1", source);
        tx.exec_params("DELETE FROM onec_sync_records WHERE source = $1", source);
        tx.exec_params("DELETE FROM onec_sync_state WHERE source = $1", source);
        tx.commit();

        OnecRawLog::instance().add(
            "RESET: cleared all data for source=" + source
        );
    } catch (const std::exception& e) {
        OnecRawLog::instance().add(
            std::string("DB reset_all error: ") + e.what()
        );
    }
}

// ---------------------------------------------------------------------------
// Транзакционная фиксация страницы:
//   1. onec_sync_records (для дедупликации/чекпоинта)
//   2. products_1c       (бизнес-таблица)
//   3. onec_sync_pages   (completed)
//   4. onec_sync_state   (accepted += N)
// ---------------------------------------------------------------------------
void OnecSyncRepository::commit_page(
    const std::string& source,
    int page_idx,
    const std::vector<OnecCatalogRecord>& records) const
{
    try {
        pqxx::connection conn(conn_str_);
        pqxx::work tx(conn);

        // 1. onec_sync_records — для дедупликации при перезапуске
        if (!records.empty()) {
            tx.exec("CREATE TEMP TABLE tmp_records ("
                    "  source TEXT, ref_key TEXT, code TEXT, "
                    "  article TEXT, name TEXT, measured_at TEXT"
                    ") ON COMMIT DROP");

            pqxx::stream_to stream(tx, "tmp_records",
                std::vector<std::string>{"source", "ref_key", "code",
                                         "article", "name", "measured_at"});
            for (const auto& r : records) {
                stream << std::make_tuple(source, r.ref_key, r.code,
                                          r.article, r.name, r.measured_at);
            }
            stream.complete();

            tx.exec(
                "INSERT INTO onec_sync_records"
                "(source, ref_key, code, article, name, measured_at) "
                "SELECT source, ref_key, code, article, name, "
                "       NULLIF(measured_at, '')::timestamptz "
                "FROM tmp_records "
                "ON CONFLICT (source, ref_key) DO UPDATE SET "
                "  code = EXCLUDED.code, article = EXCLUDED.article, "
                "  name = EXCLUDED.name, measured_at = EXCLUDED.measured_at"
            );
        }

        // 2. products_1c — бизнес-таблица (как products для Ozon)
        if (!records.empty()) {
            tx.exec("CREATE TEMP TABLE tmp_p1c ("
                    "  id TEXT, offer_id TEXT, name TEXT"
                    ") ON COMMIT DROP");

            pqxx::stream_to stream_p1c(tx, "tmp_p1c",
                std::vector<std::string>{"id", "offer_id", "name"});
            for (const auto& r : records) {
                std::string offer = r.article.empty() ? r.code : r.article;
                stream_p1c << std::make_tuple(r.ref_key, offer, r.name);
            }
            stream_p1c.complete();

            tx.exec(
                "INSERT INTO products_1c(id, offer_id, name, price, updated_at_db) "
                "SELECT id, offer_id, name, 0, NOW() FROM tmp_p1c "
                "ON CONFLICT (id) DO UPDATE SET "
                "  offer_id = EXCLUDED.offer_id, "
                "  name = EXCLUDED.name, "
                "  updated_at_db = NOW()"
            );
        }

        // 3. onec_sync_pages — отметка страницы как completed
        tx.exec_params(
            "INSERT INTO onec_sync_pages(source, page_idx, status, accepted, updated_at) "
            "VALUES($1, $2, 'completed', $3, NOW()) "
            "ON CONFLICT (source, page_idx) DO UPDATE SET "
            "  status = 'completed', accepted = EXCLUDED.accepted, updated_at = NOW()",
            source, page_idx, static_cast<int>(records.size())
        );

        // 4. onec_sync_state — accepted += N
        tx.exec_params(
            "UPDATE onec_sync_state SET accepted = accepted + $1, "
            "       updated_at = NOW() WHERE source = $2",
            static_cast<long long>(records.size()), source
        );

        tx.commit();

    } catch (const std::exception& e) {
        OnecRawLog::instance().add(
            std::string("DB commit_page error: ") + e.what()
        );
        throw;
    }
}
