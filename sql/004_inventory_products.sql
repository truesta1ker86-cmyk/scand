-- ===========================================================================
-- inventory1c_products — локальный справочник номенклатуры 1С
-- ===========================================================================
CREATE TABLE IF NOT EXISTS inventory1c_products (
    id           BIGSERIAL PRIMARY KEY,
    tenant_id    BIGINT       NOT NULL DEFAULT 1,
    code         VARCHAR(64)  NOT NULL,
    article      VARCHAR(128),
    name         VARCHAR(512),
    ref_key      VARCHAR(36)  NOT NULL,
    measured_at  TIMESTAMPTZ  NOT NULL,
    created_at   TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
    updated_at   TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
    CONSTRAINT uq_inv1c_product_tenant_code UNIQUE (tenant_id, code),
    CONSTRAINT uq_inv1c_product_tenant_ref  UNIQUE (tenant_id, ref_key)
);
CREATE INDEX IF NOT EXISTS idx_inv1c_products_code
    ON inventory1c_products(code);
CREATE INDEX IF NOT EXISTS idx_inv1c_products_article
    ON inventory1c_products(article);

-- Колонка onec_code: канонический код 1С для миграции связей.
-- pricing_config в C++ схеме отсутствует, поэтому его тут нет.
ALTER TABLE inventory1c_stocks      ADD COLUMN IF NOT EXISTS onec_code VARCHAR(64);
ALTER TABLE pricing_cost_batch      ADD COLUMN IF NOT EXISTS onec_code VARCHAR(64);
ALTER TABLE pricing_cost_supplier   ADD COLUMN IF NOT EXISTS onec_code VARCHAR(64);

CREATE INDEX IF NOT EXISTS idx_inv1c_stocks_onec_code
    ON inventory1c_stocks(onec_code);
