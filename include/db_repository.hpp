#pragma once
#include "product.hpp"
#include "sync_checkpoint.hpp"
#include <string>
#include <vector>

class DbRepository {
public:
    explicit DbRepository(std::string conn_str);

    void upsert_batch_1c(const std::vector<Product_1с>& products);
    void upsert_one_1c(const Product_1с& product);

    void upsert_batch_ozon(const std::vector<OzonProduct>& products);
    void upsert_one_ozon(const OzonProduct& product);

    SyncCheckpoint load_checkpoint(const std::string& source);
    void           save_checkpoint(const SyncCheckpoint& cp);
    void           reset_checkpoint(const std::string& source);

    // Обновить только статус (без курсора)
    void update_checkpoint_status(const std::string& source,
                                  const std::string& status);

private:
    std::string conn_str_;
};
