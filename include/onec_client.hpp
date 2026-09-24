#pragma once
#include "product.hpp"
#include <optional>
#include <string>
#include <vector>

class OnecClient {
public:
    OnecClient(std::string base_url, std::string user, std::string password,
               int page_size, int timeout_ms,
               bool allow_insecure_http   = false,   // NEW (был зашит)
               bool allow_private_network = false);  // NEW

    std::vector<Product1C> fetch_page(int page) const;
    std::optional<Product1C> fetch_product(const std::string& id) const;

    // Сырой ответ от 1С (для отладки)
    struct RawResponse {
        long        status_code = 0;
        std::string body;
        std::string error;
        std::string url;
    };

    RawResponse fetch_raw(const std::string& path) const;

    int page_size() const { return page_size_; }

private:
    std::string base_url_, user_, password_;
    int         page_size_, timeout_ms_;
    bool        allow_insecure_http_   = false;  // NEW
    bool        allow_private_network_ = false;  // NEW
};