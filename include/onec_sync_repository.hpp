#pragma once
#include "onec_types.hpp"
#include <set>
#include <string>
#include <vector>

class OnecSyncRepository {
public:
    explicit OnecSyncRepository(std::string conn_str);

    bool load_state(const std::string& source, OnecSyncState& out) const;
    void save_state(const OnecSyncState& state) const;
    void update_state_status(const std::string& source,
                             const std::string& status) const;
    void add_accepted(const std::string& source, size_t delta) const;

    std::set<int> get_completed_pages(const std::string& source) const;
    std::set<int> get_failed_pages(const std::string& source) const;

    void mark_page_completed(const std::string& source,
                             int page_idx, int accepted) const;
    void mark_page_failed(const std::string& source, int page_idx) const;

    std::set<std::string> get_existing_refs(const std::string& source) const;

    void insert_records_batch(
        const std::string& source,
        const std::vector<OnecCatalogRecord>& records) const;

    void reset_all(const std::string& source) const;

    void commit_page(const std::string& source,
                     int page_idx,
                     const std::vector<OnecCatalogRecord>& records) const;

private:
    std::string conn_str_;
};
