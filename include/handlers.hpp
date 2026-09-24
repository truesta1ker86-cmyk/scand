#pragma once
#include "router.hpp"
#include "sync_service.hpp"
#include "onec_catalog.hpp"
#include "onec_inventory.hpp"
#include "onec_ka2_inventory.hpp"
#include "inventory_repository.hpp"
#include "db_pool.hpp"
#include <memory>

void register_handlers(Router& router,
                       SyncService& sync,
                       OnecCatalog& onec_catalog,
                       scand::onec::OnecInventory& /*onec_inventory*/,
                       scand::onec::ka2::Ka2Inventory& ka2_inventory,
                       scand::inventory::InventoryRepository& inventory_repo,
                       std::shared_ptr<scand::db::ConnectionPool> db_pool);
