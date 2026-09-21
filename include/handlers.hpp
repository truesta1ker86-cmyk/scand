#pragma once
#include "router.hpp"
#include "sync_service.hpp"
#include "onec_catalog.hpp"

void register_handlers(Router& router,
                       SyncService& sync,
                       OnecCatalog& onec_catalog);
