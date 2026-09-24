-- products_1c — товары из 1С (пишет OnecSyncRepository)
CREATE TABLE IF NOT EXISTS products_1c (
    id            TEXT PRIMARY KEY,
    offer_id      TEXT,
    name          TEXT,
    price         NUMERIC(12,2) DEFAULT 0,
    updated_at_db TIMESTAMPTZ DEFAULT NOW()
);
CREATE INDEX IF NOT EXISTS idx_products_1c_offer ON products_1c(offer_id);

-- products — товары Ozon (пишет DbRepository)
CREATE TABLE IF NOT EXISTS products (
    id                 TEXT PRIMARY KEY,
    sku                TEXT,
    name               TEXT,
    price              NUMERIC(12,2),
    currency           TEXT,
    in_stock           INT,
    description        TEXT,
    weight             INT,
    created_at         TEXT,
    updated_at         TEXT,
    scategorie         TEXT,
    related_products   TEXT,
    updated_at_db      TIMESTAMPTZ DEFAULT NOW()
);
CREATE INDEX IF NOT EXISTS idx_products_sku ON products(sku);

-- sync_state — чекпоинты Ozon/1С
CREATE TABLE IF NOT EXISTS sync_state (
    source        TEXT PRIMARY KEY,
    last_cursor   TEXT,
    last_page     INT DEFAULT 0,
    total_synced  BIGINT DEFAULT 0,
    last_status   TEXT,
    updated_at    TIMESTAMPTZ
);

-- onec_sync_state — состояние 1С-каталога
CREATE TABLE IF NOT EXISTS onec_sync_state (
    source       TEXT PRIMARY KEY,
    url          TEXT,
    catalog      TEXT,
    page_size    INT,
    total_pages  INT,
    accepted     BIGINT DEFAULT 0,
    started_at   TIMESTAMPTZ,
    updated_at   TIMESTAMPTZ,
    status       TEXT
);

-- onec_sync_pages
CREATE TABLE IF NOT EXISTS onec_sync_pages (
    source      TEXT,
    page_idx    INT,
    status      TEXT,
    accepted    INT,
    updated_at  TIMESTAMPTZ,
    PRIMARY KEY (source, page_idx)
);

-- onec_sync_records
CREATE TABLE IF NOT EXISTS onec_sync_records (
    source       TEXT,
    ref_key      TEXT,
    code         TEXT,
    article      TEXT,
    name         TEXT,
    measured_at  TIMESTAMPTZ,
    PRIMARY KEY (source, ref_key)
);
