-- ===========================================================================
-- inventory1c_snapshots — снимки
-- ===========================================================================
CREATE TABLE IF NOT EXISTS inventory1c_snapshots (
    id             BIGSERIAL PRIMARY KEY,
    tenant_id      BIGINT NOT NULL DEFAULT 1,
    batch_id       VARCHAR(128) NOT NULL,
    payload_hash   VARCHAR(64)  NOT NULL,
    measured_at    TIMESTAMPTZ  NOT NULL,
    stocks_count   INT          NOT NULL DEFAULT 0,
    costs_count    INT          NOT NULL DEFAULT 0,
    created_at     TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
    updated_at     TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
    CONSTRAINT uq_inv1c_snapshot_tenant_batch UNIQUE (tenant_id, batch_id)
);
CREATE INDEX IF NOT EXISTS idx_inv1c_snapshots_measured
    ON inventory1c_snapshots(measured_at);

-- ===========================================================================
-- inventory1c_stocks — последние остатки
-- ===========================================================================
CREATE TABLE IF NOT EXISTS inventory1c_stocks (
    id              BIGSERIAL PRIMARY KEY,
    tenant_id       BIGINT NOT NULL DEFAULT 1,
    sku_1c          VARCHAR(128) NOT NULL,
    offer_id        VARCHAR(128),
    warehouse_id    VARCHAR(64)  NOT NULL,
    warehouse_name  VARCHAR(255),
    available       INT          NOT NULL DEFAULT 0,
    reserved        INT          NOT NULL DEFAULT 0,
    measured_at     TIMESTAMPTZ  NOT NULL,
    batch_id        VARCHAR(128) NOT NULL,
    created_at      TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
    updated_at      TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
    CONSTRAINT uq_inv1c_stock_tenant_sku_warehouse
        UNIQUE (tenant_id, sku_1c, warehouse_id)
);
CREATE INDEX IF NOT EXISTS idx_inv1c_stocks_sku
    ON inventory1c_stocks(sku_1c);
CREATE INDEX IF NOT EXISTS idx_inv1c_stocks_offer
    ON inventory1c_stocks(offer_id);

-- ===========================================================================
-- inventory1c_warehouses — справочник складов
-- ===========================================================================
CREATE TABLE IF NOT EXISTS inventory1c_warehouses (
    id              BIGSERIAL PRIMARY KEY,
    tenant_id       BIGINT NOT NULL DEFAULT 1,
    warehouse_id    VARCHAR(64)  NOT NULL,
    warehouse_name  VARCHAR(255) NOT NULL,
    created_at      TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
    updated_at      TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
    CONSTRAINT uq_inv1c_warehouse_tenant_warehouse
        UNIQUE (tenant_id, warehouse_id)
);

-- ===========================================================================
-- inventory1c_costs — себестоимости
-- ===========================================================================
CREATE TABLE IF NOT EXISTS inventory1c_costs (
    id                 BIGSERIAL PRIMARY KEY,
    tenant_id          BIGINT NOT NULL DEFAULT 1,
    sku_1c             VARCHAR(128) NOT NULL,
    batch_avg_cost     NUMERIC(14,4),
    purchase_cost      NUMERIC(14,4),
    purchase_source    VARCHAR(32),
    purchase_source_at TIMESTAMPTZ,
    dealer_cost        NUMERIC(14,4),
    measured_at        TIMESTAMPTZ NOT NULL,
    batch_id           VARCHAR(128) NOT NULL,
    created_at         TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at         TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    CONSTRAINT uq_inv1c_cost_tenant_sku UNIQUE (tenant_id, sku_1c)
);

-- ===========================================================================
-- pricing_cost_batch — себестоимость по партиям
-- ===========================================================================
CREATE TABLE IF NOT EXISTS pricing_cost_batch (
    id           BIGSERIAL PRIMARY KEY,
    tenant_id    BIGINT NOT NULL DEFAULT 1,
    sku_1c       VARCHAR(128) NOT NULL,
    cost         NUMERIC(14,4),
    updated_at   TIMESTAMPTZ DEFAULT NOW(),
    CONSTRAINT uq_costbatch_tenant_sku UNIQUE (tenant_id, sku_1c)
);

-- ===========================================================================
-- pricing_cost_supplier — закупка/дилер
-- ===========================================================================
CREATE TABLE IF NOT EXISTS pricing_cost_supplier (
    id                 BIGSERIAL PRIMARY KEY,
    tenant_id          BIGINT NOT NULL DEFAULT 1,
    sku_1c             VARCHAR(128) NOT NULL,
    purchase           NUMERIC(14,4),
    purchase_source    VARCHAR(32),
    purchase_source_at TIMESTAMPTZ,
    dealer             NUMERIC(14,4),
    updated_at         TIMESTAMPTZ DEFAULT NOW(),
    CONSTRAINT uq_costsup_tenant_sku UNIQUE (tenant_id, sku_1c)
);

-- ===========================================================================
-- onec_connection — подключение к 1С
-- ===========================================================================
CREATE TABLE IF NOT EXISTS onec_connection (
    id                        BIGSERIAL PRIMARY KEY,
    tenant_id                 BIGINT NOT NULL DEFAULT 1,
    base_url                  VARCHAR(1024) NOT NULL,
    username                  VARCHAR(255)  NOT NULL,
    password                  VARCHAR(2048) NOT NULL,
    is_enabled                BOOLEAN NOT NULL DEFAULT TRUE,
    allow_insecure_http       BOOLEAN NOT NULL DEFAULT FALSE,
    last_status               VARCHAR(32),
    last_ready                BOOLEAN,
    last_secure_transport     BOOLEAN,
    last_status_code          INT,
    last_metadata_entity_sets INT DEFAULT 0,
    last_resource_preview     TEXT,
    last_latency_ms           INT DEFAULT 0,
    last_message              TEXT,
    last_checked_at           TIMESTAMPTZ,
    created_at                TIMESTAMPTZ DEFAULT NOW(),
    updated_at                TIMESTAMPTZ DEFAULT NOW(),
    CONSTRAINT uq_onec_conn_tenant UNIQUE (tenant_id)
);
