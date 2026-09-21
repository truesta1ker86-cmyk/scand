#include "onec_product_mapper.hpp"
#include <algorithm>
#include <cctype>

namespace {

bool is_truthy(const std::string& v) {
    if (v.empty()) return false;
    std::string s;
    s.reserve(v.size());
    for (char c : v) s.push_back(static_cast<char>(std::tolower(c)));
    return s == "true" || s == "1" || s == "да";
}

} // namespace

size_t OnecProductMapper::skipped_count(const std::vector<OnecCatalogRow>& rows) {
    size_t n = 0;
    for (const auto& r : rows) {
        if (is_truthy(r.is_folder))     { ++n; continue; }
        if (is_truthy(r.deletion_mark)) { ++n; continue; }
        if (r.ref_key.empty())          { ++n; continue; }
    }
    return n;
}

std::vector<Product_1с> OnecProductMapper::map(
    const std::vector<OnecCatalogRow>& rows)
{
    std::vector<Product_1с> out;
    out.reserve(rows.size());

    for (const auto& r : rows) {
        if (is_truthy(r.is_folder))     continue;
        if (is_truthy(r.deletion_mark)) continue;
        if (r.ref_key.empty())          continue;

        Product_1с p;
        p.id       = r.ref_key;
        p.offer_id = r.article.empty() ? r.code : r.article;
        p.name     = r.full_name.empty() ? r.name : r.full_name;
        p.price    = 0.0;

        out.push_back(std::move(p));
    }

    return out;
}
