#include "onec_product_catalog.hpp"
#include "sse_broker.hpp"
#include "config.hpp"
#include "onec_odata_preflight.hpp"
#include "onec_url_normalizer.hpp"
#include "time_utils.hpp"

#include <boost/json.hpp>
#include <cpr/cpr.h>
#include <pqxx/pqxx>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace json = boost::json;

namespace scand::onec::catalog {

namespace {

constexpr std::size_t kPageSize = 5'000;
constexpr std::size_t kMaxRows  = 100'000;

std::string now_iso8601_utc() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

std::string get_str(const json::object& o, const char* key) {
    auto it = o.find(key);
    if (it == o.end() || !it->value().is_string()) return "";
    return json::value_to<std::string>(it->value());
}

bool get_bool(const json::object& o, const char* key) {
    auto it = o.find(key);
    if (it == o.end()) return false;
    if (it->value().is_bool()) return it->value().as_bool();
    return false;
}

bool is_valid_uuid(const std::string& s) {
    if (s.size() != 36) return false;
    const int dash[] = {8, 13, 18, 23};
    for (int p : dash) if (s[p] != '-') return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

} // namespace

std::vector<OneCProductRecord> normalize_onec_product_catalog(
    const std::vector<std::vector<std::string>>& rows,
    const std::string& measured_at)
{
    // rows[i] = {code, article, name, ref_key}
    std::unordered_map<std::string, OneCProductRecord> by_ref;
    std::unordered_map<std::string, std::unordered_set<std::string>> refs_by_code;

    for (const auto& row : rows) {
        if (row.size() < 4) continue;
        const std::string& code    = row[0];
        const std::string& article = row[1];
        const std::string& name    = row[2];
        const std::string& ref_key = row[3];
        if (code.empty() || ref_key.empty()) continue;
        if (!is_valid_uuid(ref_key)) continue;

        OneCProductRecord rec;
        rec.code        = code;
        rec.article     = article;
        rec.name        = name;
        rec.ref_key     = ref_key;
        rec.measured_at = measured_at;

        by_ref[ref_key] = rec;
        refs_by_code[code].insert(ref_key);
    }

    std::vector<OneCProductRecord> out;
    out.reserve(by_ref.size());
    for (auto& [ref, rec] : by_ref) {
        if (refs_by_code[rec.code].size() > 1) continue; // неоднозначный код
        out.push_back(std::move(rec));
    }
    std::sort(out.begin(), out.end(),
        [](const OneCProductRecord& a, const OneCProductRecord& b) {
            return a.code < b.code;
        });
    return out;
}

namespace {

std::vector<std::vector<std::string>> read_all_rows(
    const std::string& base_url,
    const std::string& username,
    const std::string& password,
    int timeout_ms,
    bool allow_insecure_http)
{
    OnecUrlNormalizeOptions opts;
    opts.allow_insecure_http = allow_insecure_http;
    std::string base = normalize_onec_publication_url(base_url, opts);
    std::string root = base;
    while (!root.empty() && root.back() == '/') root.pop_back();
    root += "/odata/standard.odata";

    std::vector<std::vector<std::string>> all;
    std::size_t offset = 0;
    while (offset < kMaxRows) {
        std::string url = root + "/Catalog_Номенклатура"
            + "?$format=json"
            + "&$top="  + std::to_string(kPageSize)
            + "&$skip=" + std::to_string(offset)
            + "&$select=Ref_Key,Code,Артикул,Description,DeletionMark,IsFolder"
            + "&$orderby=Code";

        cpr::Response r = cpr::Get(
            cpr::Url{url},
            cpr::Authentication{username, password, cpr::AuthMode::BASIC},
            cpr::Header{{"Accept", "application/json"}},
            cpr::Timeout{timeout_ms});

        if (r.status_code != 200) {
            throw OneCProductCatalogError(
                "1С вернула HTTP " + std::to_string(r.status_code)
                + " при чтении каталога номенклатуры");
        }
        json::value parsed;
        try {
            parsed = json::parse(r.text);
        } catch (const std::exception& e) {
            throw OneCProductCatalogError(
                std::string("1С вернула невалидный JSON: ") + e.what());
        }
        if (!parsed.is_object()) {
            throw OneCProductCatalogError("1С вернула не-объект");
        }
        const auto& obj = parsed.as_object();
        auto it = obj.find("value");
        if (it == obj.end() || !it->value().is_array()) {
            throw OneCProductCatalogError("1С вернула ответ без value[]");
        }
        const auto& arr = it->value().as_array();
        if (arr.empty()) break;

        for (const auto& item : arr) {
            if (!item.is_object()) continue;
            const auto& row = item.as_object();
            if (get_bool(row, "DeletionMark")) continue;
            if (get_bool(row, "IsFolder"))     continue;
            all.push_back({
                get_str(row, "Code"),
                get_str(row, "Артикул"),
                get_str(row, "Description"),
                get_str(row, "Ref_Key"),
            });
        }

        if (arr.size() < kPageSize) break;
        offset += arr.size();
    }
    if (offset >= kMaxRows) {
        throw OneCProductCatalogError(
            "Каталог 1С превышает безопасный лимит 100 000 позиций");
    }
    return all;
}

} // namespace


namespace {
void emit_progress(const CatalogProgressFn& cb,
                   const std::string& stage,
                   long long current,
                   long long total,
                   const std::string& extra_json = "")
{
    if (cb) cb(stage, current, total, extra_json);
}
} // namespace

OneCProductCatalogPullResult pull_onec_product_catalog(
    pqxx::work& tx,
    long long   tenant_id,
    CatalogProgressFn progress_cb)
{
    // 1. Читаем подключение 1С
    const Config& cfg = global_config();
    std::string base_url            = cfg.onec_base_url;
    std::string username            = cfg.onec_user;
    std::string password            = cfg.onec_password;
    bool        allow_insecure_http = cfg.onec_allow_insecure_http;
    int         timeout_ms          = cfg.onec_timeout_ms;

    if (base_url.empty() || username.empty() || password.empty()) {
        throw OneCProductCatalogError(
            "В config.ini не заполнены [onec] base_url/user/password.");
    }
    // 2. Читаем каталог из 1С
    auto raw_rows = read_all_rows(
        base_url, username, password, timeout_ms, allow_insecure_http);

    const std::string measured_at = now_iso8601_utc();
    auto records = normalize_onec_product_catalog(raw_rows, measured_at);

    emit_progress(progress_cb, "normalize",
                  static_cast<long long>(raw_rows.size()),
                  static_cast<long long>(records.size()),
                  "{\"raw\":" + std::to_string(raw_rows.size())
                  + ",\"normalized\":" + std::to_string(records.size()) + "}");

    OneCProductCatalogPullResult result;
    result.measured_at = measured_at;
    result.accepted    = records.size();

    // 3. Считаем conflicts: коды, которые 1С вернула, но мы отбросили
    {
        std::unordered_set<std::string> seen_codes;
        std::size_t dropped = 0;
        for (const auto& row : raw_rows) {
            if (row.size() < 4) continue;
            if (!seen_codes.insert(row[0]).second) {
                dropped++;
            }
        }
        result.conflicts = dropped;
    }

    // 4. Атомарная замена inventory1c_products
    emit_progress(progress_cb, "save", 0,
                  static_cast<long long>(records.size()), "");

    tx.exec_params(
        "DELETE FROM inventory1c_products WHERE tenant_id = $1",
        tenant_id);

    if (!records.empty()) {
        // Пакетная вставка через COPY
        pqxx::stream_to stream(tx, "inventory1c_products",
            std::vector<std::string>{
                "tenant_id", "code", "article", "name", "ref_key", "measured_at"});
        for (const auto& rec : records) {
            stream << std::make_tuple(
                tenant_id, rec.code, rec.article, rec.name,
                rec.ref_key, rec.measured_at);
        }
        stream.complete();
    }

    // 5. Миграция связей: pricing_config, inventory1c_stocks,
    //    pricing_cost_batch, pricing_cost_supplier
    //    Обновляем onec_code по точному совпадению артикула.
    auto migrate = [&](const char* table, const char* article_col) {
        std::string sql =
            std::string("UPDATE ") + table + " AS t "
            "SET onec_code = p.code "
            "FROM inventory1c_products AS p "
            "WHERE t.tenant_id = $1 "
            "  AND p.tenant_id = $1 "
            "  AND t.onec_code IS NULL "
            "  AND t." + article_col + " = p.article";
        auto res = tx.exec_params(sql, tenant_id);
        return res.affected_rows();
    };

    // Эти таблицы могут отсутствовать в старой БД — оборачиваем в try.
    auto safe_migrate = [&](const char* table, const char* article_col) -> unsigned long long {
        try {
            return static_cast<unsigned long long>(
                migrate(table, article_col));
        } catch (const pqxx::undefined_table&) {
            return static_cast<unsigned long long>(0);
        }
    };

    result.migrated += safe_migrate("inventory1c_stocks", "sku_1c");
    result.migrated += safe_migrate("pricing_cost_batch", "sku_1c");
    result.migrated += safe_migrate("pricing_cost_supplier", "sku_1c");

    emit_progress(progress_cb, "migrate",
                  static_cast<long long>(result.migrated),
                  0, "");

    return result;
}

// ===========================================================================
// SSE-вариант: то же, что pull_onec_product_catalog, но с эмиссией событий
// ===========================================================================
namespace {

void emit_pc_event(const std::string& stage,
                   const std::string& status,
                   const std::string& event_name,
                   boost::json::object extra = {})
{
    namespace json = boost::json;
    json::object body{
        {"source", "product_catalog"},
        {"stage",  stage},
        {"status", status},
    };
    for (auto& [k, v] : extra) body[k] = v;
    try {
        SseBroker::instance().broadcast(event_name, body);
    } catch (...) {}
}

} // namespace

OneCProductCatalogPullResult pull_onec_product_catalog_sse(
    pqxx::work& tx,
    const PullOptions& opts)
{
    auto t0 = std::chrono::steady_clock::now();
    auto ms_since = [&]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
    };

    // --- читаем подключение 1С ---
    const Config& cfg = global_config();
    const std::string base_url = cfg.onec_base_url;
    const std::string username = cfg.onec_user;
    const std::string password = cfg.onec_password;
    const bool  insecure = cfg.onec_allow_insecure_http;
    const int   timeout  = cfg.onec_timeout_ms;

    if (base_url.empty() || username.empty() || password.empty()) {
        throw OneCProductCatalogError(
            "В config.ini не заполнены [onec] base_url/user/password.");
    }

    emit_pc_event("connecting", "started", opts.event_name, {
        {"message",  "Проверка соединения с 1С"},
        {"base_url", base_url},
    });

    OnecUrlNormalizeOptions url_opts;
    url_opts.allow_insecure_http = insecure;
    std::string base = normalize_onec_publication_url(base_url, url_opts);
    std::string root = base;
    while (!root.empty() && root.back() == '/') root.pop_back();
    root += "/odata/standard.odata";

    emit_pc_event("connecting", "done", opts.event_name, {
        {"message",    "Соединение с 1С установлено"},
        {"elapsed_ms", ms_since()},
    });

    // --- читаем страницы ---
    emit_pc_event("fetching_pages", "started", opts.event_name, {
        {"page_size", static_cast<long long>(opts.page_size)},
        {"max_rows",  static_cast<long long>(opts.max_rows)},
        {"message",   "Начало чтения каталога"},
    });

    std::vector<std::vector<std::string>> all;
    std::size_t offset   = 0;
    std::size_t page_idx = 0;
    std::size_t read     = 0;

    while (offset < opts.max_rows) {
        ++page_idx;
        auto t_page = std::chrono::steady_clock::now();

        emit_pc_event("fetching_pages", "page_started", opts.event_name, {
            {"page_num", static_cast<long long>(page_idx)},
            {"offset",   static_cast<long long>(offset)},
            {"read",     static_cast<long long>(read)},
            {"message",  "Чтение страницы " + std::to_string(page_idx)},
        });

        std::string url = root + "/Catalog_Номенклатура"
            + "?$format=json"
            + "&$top="  + std::to_string(opts.page_size)
            + "&$skip=" + std::to_string(offset)
            + "&$select=Ref_Key,Code,Артикул,Description,DeletionMark,IsFolder"
            + "&$orderby=Code";

        cpr::Response r = cpr::Get(
            cpr::Url{url},
            cpr::Authentication{username, password, cpr::AuthMode::BASIC},
            cpr::Header{{"Accept", "application/json"}},
            cpr::Timeout{timeout});

        if (r.status_code != 200) {
            emit_pc_event("fetching_pages", "page_failed", opts.event_name, {
                {"page_num", static_cast<long long>(page_idx)},
                {"error",    "HTTP " + std::to_string(r.status_code)},
            });
            throw OneCProductCatalogError(
                "1С вернула HTTP " + std::to_string(r.status_code)
                + " при чтении каталога");
        }

        namespace json = boost::json;
        json::value parsed;
        try { parsed = json::parse(r.text); }
        catch (const std::exception& e) {
            throw OneCProductCatalogError(
                std::string("невалидный JSON: ") + e.what());
        }
        if (!parsed.is_object())
            throw OneCProductCatalogError("1С вернула не-объект");

        const auto& obj = parsed.as_object();
        auto it = obj.find("value");
        if (it == obj.end() || !it->value().is_array())
            throw OneCProductCatalogError("нет value[]");
        const auto& arr = it->value().as_array();
        if (arr.empty()) break;

        std::size_t page_read = 0;
        for (const auto& item : arr) {
            if (!item.is_object()) continue;
            const auto& row = item.as_object();
            if (get_bool(row, "DeletionMark")) continue;
            if (get_bool(row, "IsFolder"))     continue;
            all.push_back({
                get_str(row, "Code"),
                get_str(row, "Артикул"),
                get_str(row, "Description"),
                get_str(row, "Ref_Key"),
            });
            ++page_read;
        }
        read += page_read;

        auto page_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_page).count();

        emit_pc_event("fetching_pages", "page_done", opts.event_name, {
            {"page_num",  static_cast<long long>(page_idx)},
            {"page_read", static_cast<long long>(page_read)},
            {"read",      static_cast<long long>(read)},
            {"page_ms",   page_ms},
            {"message",   "Страница " + std::to_string(page_idx)
                          + ": +" + std::to_string(page_read)
                          + " (" + std::to_string(page_ms) + " мс)"},
        });

        if (arr.size() < opts.page_size) break;
        offset += arr.size();
    }

    // --- сохраняем ---
    emit_pc_event("saving", "started", opts.event_name, {
        {"read",    static_cast<long long>(read)},
        {"message", "Нормализация и запись в БД"},
    });

    const std::string measured_at = now_iso8601_utc();
    auto records = normalize_onec_product_catalog(all, measured_at);

    OneCProductCatalogPullResult result;
    result.measured_at = measured_at;
    result.accepted    = records.size();

    // conflicts
    {
        std::unordered_set<std::string> seen_codes;
        std::size_t dropped = 0;
        for (const auto& row : all) {
            if (row.size() < 4) continue;
            if (!seen_codes.insert(row[0]).second) ++dropped;
        }
        result.conflicts = dropped;
    }

    tx.exec_params("DELETE FROM inventory1c_products WHERE tenant_id = $1",
                   opts.tenant_id);

    if (!records.empty()) {
        pqxx::stream_to stream(tx, "inventory1c_products",
            std::vector<std::string>{
                "tenant_id","code","article","name","ref_key","measured_at"});
        for (const auto& rec : records) {
            stream << std::make_tuple(opts.tenant_id, rec.code, rec.article,
                                      rec.name, rec.ref_key, rec.measured_at);
        }
        stream.complete();
    }

    // миграция связей
    auto migrate = [&](const char* table, const char* article_col) {
        std::string sql =
            std::string("UPDATE ") + table + " AS t "
            "SET onec_code = p.code "
            "FROM inventory1c_products AS p "
            "WHERE t.tenant_id = $1 "
            "  AND p.tenant_id = $1 "
            "  AND t.onec_code IS NULL "
            "  AND t." + article_col + " = p.article";
        auto res = tx.exec_params(sql, opts.tenant_id);
        return res.affected_rows();
    };
    auto safe_migrate = [&](const char* t, const char* c) -> unsigned long long {
        try { return static_cast<unsigned long long>(migrate(t, c)); }
        catch (const pqxx::undefined_table&) { return 0; }
    };

    result.migrated += safe_migrate("inventory1c_stocks",    "sku_1c");
    result.migrated += safe_migrate("pricing_cost_batch",    "sku_1c");
    result.migrated += safe_migrate("pricing_cost_supplier", "sku_1c");

    emit_pc_event("saving", "done", opts.event_name, {
        {"accepted",   static_cast<long long>(result.accepted)},
        {"migrated",   static_cast<long long>(result.migrated)},
        {"conflicts",  static_cast<long long>(result.conflicts)},
        {"elapsed_ms", ms_since()},
        {"message",    "Готово: " + std::to_string(result.accepted) + " позиций"},
    });

    return result;
}

} // namespace scand::onec::catalog
