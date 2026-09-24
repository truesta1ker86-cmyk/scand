#pragma once
#include "onec_types.hpp"
#include <atomic>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

struct OnecCatalogLimits {
    size_t page_size = 500;
    size_t max_rows  = 200'000;
};

struct OnecCatalogRow {
    std::string ref_key;
    std::string code;
    std::string article;
    std::string name;
    std::string full_name;
    std::string is_folder;
    std::string deletion_mark;
    std::string parent_key;
    std::string unit_key;
    std::string vat_key;
    std::string weight_num;
    std::string weight_den;
    std::string raw_json;
};

class OnecCatalog {
public:
    OnecCatalog(std::string base_url,
                std::string username,
                std::string password,
                int timeout_ms,
                bool allow_insecure_http   = false,
                bool allow_private_network = false);   // NEW

    struct RawResponse {
        long        status_code = 0;
        std::string body;
        std::string error;
        std::string url;

        bool ok() const { return status_code >= 200 && status_code < 300; }
    };

    RawResponse read_raw(const std::string& path) const;
    std::string build_url(const std::string& path) const;

    std::vector<std::string>    read_all_pages(const OnecCatalogLimits& limits) const;
    std::vector<OnecCatalogRow> read_all_rows (const OnecCatalogLimits& limits) const;

    std::string read_raw_page(size_t skip, size_t top) const;

    OnecPageResult read_page_with_retry(
        size_t skip, size_t top,
        int max_attempts = 5,
        int base_delay_ms = 2000,
        std::atomic<bool>* abort_flag = nullptr,
        const std::atomic<bool>* external_stop = nullptr
    ) const;

    static std::vector<OnecCatalogRow> parse_rows_static(
        const std::string& json_text);

    std::optional<size_t> read_total_count() const;

    std::string get_base_url() const { return base_url_; }

private:
    std::string read_page(size_t skip, size_t top) const;

    // Общий конструктор OData-пути страницы.
    std::string build_page_path(size_t skip, size_t top) const;

    std::string base_url_;
    std::string username_;
    std::string password_;
    int         timeout_ms_;
    std::string root_;
    bool        allow_insecure_http_   = false;   // NEW
    bool        allow_private_network_ = false;   // NEW
};