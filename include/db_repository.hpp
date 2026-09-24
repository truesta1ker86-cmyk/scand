#pragma once
#include "product.hpp"
#include "result.hpp"
#include "db_pool.hpp"
#include "sync_checkpoint.hpp"
#include <memory>
#include <string>
#include <vector>

class DbRepository {
public:
    explicit DbRepository(std::shared_ptr<scand::db::ConnectionPool> pool);

    // 1С
    scand::VoidResult upsert_batch_1c(const std::vector<Product1C>& products);
    scand::VoidResult upsert_one_1c(const Product1C& product);

    // Ozon
    scand::VoidResult upsert_batch_ozon(const std::vector<OzonProduct>& products);
    scand::VoidResult upsert_one_ozon(const OzonProduct& product);

    // Чекпоинты
    SyncCheckpoint    load_checkpoint(const std::string& source);
    scand::VoidResult save_checkpoint(const SyncCheckpoint& cp);
    scand::VoidResult reset_checkpoint(const std::string& source);
    scand::VoidResult update_checkpoint_status(const std::string& source,
                                               const std::string& status);

private:
    std::shared_ptr<scand::db::ConnectionPool> pool_;
};
