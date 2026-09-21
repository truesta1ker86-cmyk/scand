#pragma once
#include "product.hpp"
#include <optional>
#include <string>
#include <vector>

struct OzonPage {
    std::vector<OzonProduct> items;
    std::string              last_id;
    bool                     has_more      = false;
    bool                     network_error = false;
    long long                total         = 0;
};

class OzonClient {
public:
    OzonClient(std::string client_id, std::string api_key,
               int page_size, int timeout_ms);

    OzonPage fetch_page_cursor(const std::string& last_id) const;
    std::vector<OzonProduct> fetch_page(int page) const;
    std::vector<OzonProduct> fetch_info_batch(const std::vector<std::string>& ids) const;
    std::vector<OzonProduct> fetch_prices_stocks_batch(const std::vector<std::string>& skus) const;
    std::optional<OzonProduct> fetch_product_info(const std::string& product_id) const;
    int page_size() const { return page_size_; }

private:
    std::string base_url_ = "https://api-seller.ozon.ru";
    std::string client_id_;
    std::string api_key_;
    int         page_size_;
    int         timeout_ms_;
};
