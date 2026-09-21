-- ===========================================================================
-- Существующие таблицы (products, products_1c, sync_state)
-- ===========================================================================

DROP TABLE IF EXISTS products_1c;
DROP TABLE IF EXISTS products;
DROP TABLE IF EXISTS sync_state;

CREATE TABLE products_1c (
    id             TEXT PRIMARY KEY,
    offer_id       TEXT,
    name           TEXT,
    price          NUMERIC(12, 2) NOT NULL DEFAULT 0,
    updated_at_db  TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
CREATE INDEX idx_products_1c_updated_at_db ON products_1c(updated_at_db);
CREATE INDEX idx_products_1c_offer_id      ON products_1c(offer_id);

CREATE TABLE products (
    id                TEXT PRIMARY KEY,
    sku               TEXT,
    name              TEXT,
    price             NUMERIC(12, 2) NOT NULL DEFAULT 0,
    currency          TEXT,
    in_stock          INT NOT NULL DEFAULT 0,
    description       TEXT,
    weight            INT NOT NULL DEFAULT 0,
    created_at        TEXT,
    updated_at        TEXT,
    scategorie        TEXT,
    related_products  TEXT,
    updated_at_db     TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
CREATE INDEX idx_products_updated_at_db ON products(updated_at_db);
CREATE INDEX idx_products_sku           ON products(sku);
CREATE INDEX idx_products_scategorie    ON products(scategorie);

CREATE TABLE sync_state (
    source       TEXT PRIMARY KEY,
    last_cursor  TEXT,
    last_page    INT DEFAULT 0,
    total_synced BIGINT DEFAULT 0,
    last_status  TEXT DEFAULT '',
    updated_at   TIMESTAMPTZ DEFAULT NOW()
);

INSERT INTO sync_state(source, last_cursor, last_page, total_synced, last_status)
VALUES ('ozon', '', 0, 0, ''), ('onec', '', 0, 0, '')
ON CONFLICT (source) DO NOTHING;

-- ===========================================================================
-- НОВЫЕ таблицы для синхронизации каталога 1С
-- ===========================================================================

-- Состояние синхронизации (по источнику)
CREATE TABLE IF NOT EXISTS onec_sync_state (
    source        TEXT PRIMARY KEY,       -- "onec_catalog"
    url           TEXT,
    catalog       TEXT,                   -- "Catalog_Номенклатура"
    page_size     INT,
    total_pages   INT,
    accepted      BIGINT DEFAULT 0,
    started_at    TIMESTAMPTZ DEFAULT NOW(),
    updated_at    TIMESTAMPTZ DEFAULT NOW(),
    status        TEXT DEFAULT 'idle'     -- idle, running, done, failed, stopped
);

-- Какие страницы прочитаны / упали
CREATE TABLE IF NOT EXISTS onec_sync_pages (
    source        TEXT NOT NULL,
    page_idx      INT  NOT NULL,
    status        TEXT NOT NULL,          -- completed, failed
    accepted      INT  DEFAULT 0,
    updated_at    TIMESTAMPTZ DEFAULT NOW(),
    PRIMARY KEY (source, page_idx)
);

CREATE INDEX IF NOT EXISTS idx_onec_sync_pages_status
    ON onec_sync_pages(source, status);

-- Прочитанные записи (для дедупликации между запусками)
CREATE TABLE IF NOT EXISTS onec_sync_records (
    source        TEXT NOT NULL,
    ref_key       TEXT NOT NULL,
    code          TEXT,
    article       TEXT,
    name          TEXT,
    measured_at   TIMESTAMPTZ DEFAULT NOW(),
    PRIMARY KEY (source, ref_key)
);

CREATE INDEX IF NOT EXISTS idx_onec_sync_records_code
    ON onec_sync_records(source, code);
