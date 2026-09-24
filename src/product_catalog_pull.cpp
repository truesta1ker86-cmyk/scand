#include "product_catalog_pull.hpp"
#include "onec_raw_log.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>

namespace json = boost::json;

namespace scand::onec {

namespace {

using clk = std::chrono::steady_clock;

long long ms_since(clk::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        clk::now() - t0).count();
}

void emit_raw(const std::string& event_name, json::object body) {
    try {
        SseBroker::instance().broadcast(event_name, std::move(body));
    } catch (...) {}
}

void emit(const std::string& stage,
          const std::string& status,
          const json::object& extra = {})
{
    json::object body{
        {"source", "product_catalog"},
        {"stage",  stage},
        {"status", status},
    };
    for (const auto& [k, v] : extra) body[k] = v;
    emit_raw("product_catalog_progress", std::move(body));
}

std::string fmt_ms(long long ms) {
    if (ms < 1000) return std::to_string(ms) + " мс";
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << (ms / 1000.0) << " с";
    return oss.str();
}

std::string fmt_speed(double speed) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << speed;
    return oss.str();
}

std::string fmt_eta(double seconds) {
    if (seconds <= 0) return "0 сек";
    if (seconds < 60) {
        return "~" + std::to_string(static_cast<int>(seconds)) + " сек";
    }
    int min = static_cast<int>(seconds / 60);
    int sec = static_cast<int>(seconds) % 60;
    return "~" + std::to_string(min) + " мин " + std::to_string(sec) + " сек";
}

} // namespace

ProductCatalogPullResult pull_product_catalog(
    OnecCatalog& onec_catalog,
    const OnecCatalogLimits& limits,
    const std::atomic<bool>* stop)
{
    ProductCatalogPullResult result;
    auto t0 = clk::now();

    // ============== CONNECTING ==============
    emit("connecting", "started", {
        {"message",  "Проверка соединения с 1С"},
        {"base_url", onec_catalog.get_base_url()},
    });

    if (stop && stop->load()) {
        emit("connecting", "stopped", {
            {"message",    "Остановлено пользователем"},
            {"elapsed_ms", ms_since(t0)},
        });
        result.error = "stopped";
        result.elapsed_ms = ms_since(t0);
        return result;
    }

    emit("connecting", "done", {
        {"message",    "Соединение с 1С установлено"},
        {"elapsed_ms", ms_since(t0)},
    });

    // ============== FETCHING_TOTAL ==============
    emit("fetching_total", "started", {
        {"message", "Получение общего количества записей"},
    });

    size_t total = 0;
    auto t_total = clk::now();

    try {
        if (auto tc = onec_catalog.read_total_count()) {
            total = *tc;
            result.total = total;
            emit("fetching_total", "done", {
                {"total",      static_cast<long long>(total)},
                {"elapsed_ms", ms_since(t_total)},
                {"message",    "Всего записей: " + std::to_string(total)},
            });
        } else {
            emit("fetching_total", "warning", {
                {"elapsed_ms", ms_since(t_total)},
                {"message",    "Не удалось получить total count"},
            });
        }
    } catch (const std::exception& e) {
        emit("fetching_total", "failed", {
            {"error",      e.what()},
            {"elapsed_ms", ms_since(t_total)},
        });
        result.error = e.what();
        result.elapsed_ms = ms_since(t0);
        return result;
    }

    if (stop && stop->load()) {
        emit("fetching_total", "stopped", {
            {"message",    "Остановлено пользователем"},
            {"elapsed_ms", ms_since(t0)},
        });
        result.error = "stopped";
        result.elapsed_ms = ms_since(t0);
        return result;
    }

    // ============== FETCHING_PAGES ==============
    size_t total_pages = total > 0
        ? (total + limits.page_size - 1) / limits.page_size
        : 0;
    result.pages_total = total_pages;

    emit("fetching_pages", "started", {
        {"total",       static_cast<long long>(total)},
        {"total_pages", static_cast<long long>(total_pages)},
        {"page_size",   static_cast<long long>(limits.page_size)},
        {"max_rows",    static_cast<long long>(limits.max_rows)},
        {"message",     "Начало чтения каталога"},
    });

    size_t page_idx  = 0;
    auto t_pages     = clk::now();
    auto t_last_emit = t_pages;

    try {
        for (size_t offset = 0; offset < limits.max_rows;
             offset += limits.page_size)
        {
            if (stop && stop->load()) {
                emit("fetching_pages", "stopped", {
                    {"read",        static_cast<long long>(result.read)},
                    {"skipped",     static_cast<long long>(result.skipped)},
                    {"total",       static_cast<long long>(total)},
                    {"page_num",    static_cast<long long>(page_idx)},
                    {"total_pages", static_cast<long long>(total_pages)},
                    {"elapsed_ms",  ms_since(t_pages)},
                    {"message",     "Остановлено пользователем"},
                });
                result.error = "stopped";
                result.elapsed_ms = ms_since(t0);
                return result;
            }

            ++page_idx;
            auto t_page = clk::now();

            emit("fetching_pages", "page_started", {
                {"page_num",    static_cast<long long>(page_idx)},
                {"total_pages", static_cast<long long>(total_pages)},
                {"offset",      static_cast<long long>(offset)},
                {"page_size",   static_cast<long long>(limits.page_size)},
                {"read",        static_cast<long long>(result.read)},
                {"skipped",     static_cast<long long>(result.skipped)},
                {"total",       static_cast<long long>(total)},
                {"percent",     total > 0 ? 100.0 * result.read / total : 0.0},
                {"message",     "Чтение страницы " + std::to_string(page_idx) +
                                (total_pages > 0
                                    ? " / " + std::to_string(total_pages)
                                    : "")},
            });

            std::string page_json;
            try {
                page_json = onec_catalog.read_raw_page(offset, limits.page_size);
            } catch (const std::exception& e) {
                ++result.errors;
                emit("fetching_pages", "page_failed", {
                    {"page_num",   static_cast<long long>(page_idx)},
                    {"offset",     static_cast<long long>(offset)},
                    {"error",      e.what()},
                    {"errors",     static_cast<long long>(result.errors)},
                    {"elapsed_ms", ms_since(t_page)},
                });
                continue;
            }

            std::vector<OnecCatalogRow> rows;
            try {
                rows = OnecCatalog::parse_rows_static(page_json);
            } catch (const std::exception& e) {
                ++result.errors;
                emit("fetching_pages", "page_parse_failed", {
                    {"page_num",   static_cast<long long>(page_idx)},
                    {"offset",     static_cast<long long>(offset)},
                    {"error",      e.what()},
                    {"errors",     static_cast<long long>(result.errors)},
                    {"elapsed_ms", ms_since(t_page)},
                });
                continue;
            }

            size_t page_read    = 0;
            size_t page_skipped = 0;

            for (const auto& row : rows) {
                if (row.is_folder     == "true") { ++page_skipped; continue; }
                if (row.deletion_mark == "true") { ++page_skipped; continue; }
                ++page_read;
            }

            result.read       += page_read;
            result.skipped    += page_skipped;
            result.pages_done  = page_idx;

            long long elapsed_pages_ms = ms_since(t_pages);
            double elapsed_sec = elapsed_pages_ms / 1000.0;
            double speed       = elapsed_sec > 0
                ? static_cast<double>(result.read) / elapsed_sec : 0.0;
            result.speed = speed;

            double percent = total > 0
                ? 100.0 * static_cast<double>(result.read) / total : 0.0;

            double eta_sec = 0.0;
            if (speed > 0 && total > 0 && result.read < total) {
                eta_sec = static_cast<double>(total - result.read) / speed;
            }

            emit("fetching_pages", "page_done", {
                {"page_num",     static_cast<long long>(page_idx)},
                {"total_pages",  static_cast<long long>(total_pages)},
                {"offset",       static_cast<long long>(offset)},
                {"page_read",    static_cast<long long>(page_read)},
                {"page_skipped", static_cast<long long>(page_skipped)},
                {"page_size",    static_cast<long long>(rows.size())},
                {"read",         static_cast<long long>(result.read)},
                {"skipped",      static_cast<long long>(result.skipped)},
                {"total",        static_cast<long long>(total)},
                {"percent",      percent},
                {"speed",        speed},
                {"eta_sec",      eta_sec},
                {"page_ms",      ms_since(t_page)},
                {"elapsed_ms",   elapsed_pages_ms},
                {"message",      "Страница " + std::to_string(page_idx) +
                                 " прочитана за " + fmt_ms(ms_since(t_page)) +
                                 " (" + std::to_string(page_read) +
                                 " записей, " + fmt_speed(speed) + "/с" +
                                 (eta_sec > 0
                                     ? ", осталось " + fmt_eta(eta_sec)
                                     : "") + ")"},
            });

            if (ms_since(t_last_emit) >= 2000) {
                t_last_emit = clk::now();
                emit("fetching_pages", "running", {
                    {"read",        static_cast<long long>(result.read)},
                    {"skipped",     static_cast<long long>(result.skipped)},
                    {"errors",      static_cast<long long>(result.errors)},
                    {"total",       static_cast<long long>(total)},
                    {"page_num",    static_cast<long long>(page_idx)},
                    {"total_pages", static_cast<long long>(total_pages)},
                    {"percent",     percent},
                    {"speed",       speed},
                    {"eta_sec",     eta_sec},
                    {"elapsed_ms",  elapsed_pages_ms},
                    {"message",     "Прогресс: " +
                                    std::to_string(result.read) + " / " +
                                    std::to_string(total) + " (" +
                                    fmt_speed(speed) + "/с, осталось " +
                                    fmt_eta(eta_sec) + ")"},
                });
            }

            if (rows.size() < limits.page_size) break;
        }

        // ============== FINALIZING ==============
        long long total_ms = ms_since(t0);

        emit("finalizing", "started", {
            {"read",        static_cast<long long>(result.read)},
            {"skipped",     static_cast<long long>(result.skipped)},
            {"errors",      static_cast<long long>(result.errors)},
            {"total",       static_cast<long long>(total)},
            {"pages_done",  static_cast<long long>(result.pages_done)},
            {"total_pages", static_cast<long long>(result.pages_total)},
            {"elapsed_ms",  total_ms},
            {"message",     "Завершение"},
        });

        result.elapsed_ms = total_ms;
        result.speed = total_ms > 0
            ? static_cast<double>(result.read) * 1000.0 / total_ms : 0.0;

        emit("finalizing", "done", {
            {"read",        static_cast<long long>(result.read)},
            {"skipped",     static_cast<long long>(result.skipped)},
            {"errors",      static_cast<long long>(result.errors)},
            {"total",       static_cast<long long>(total)},
            {"pages_done",  static_cast<long long>(result.pages_done)},
            {"total_pages", static_cast<long long>(result.pages_total)},
            {"elapsed_ms",  result.elapsed_ms},
            {"speed",       result.speed},
            {"message",     "Готово: " + std::to_string(result.read) +
                            " записей за " + fmt_ms(result.elapsed_ms) +
                            " (" + fmt_speed(result.speed) + "/с)"},
        });

    } catch (const std::exception& e) {
        long long total_ms = ms_since(t0);
        emit("fetching_pages", "failed", {
            {"read",       static_cast<long long>(result.read)},
            {"skipped",    static_cast<long long>(result.skipped)},
            {"errors",     static_cast<long long>(result.errors)},
            {"error",      e.what()},
            {"elapsed_ms", total_ms},
        });
        result.error = e.what();
        result.elapsed_ms = total_ms;
        return result;
    }

    return result;
}

} // namespace scand::onec
