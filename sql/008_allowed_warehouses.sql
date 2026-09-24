-- Список складов, которые нужно сканировать.
-- Если таблица пуста — берутся все склады (поведение по умолчанию).
CREATE TABLE IF NOT EXISTS allowed_warehouses (
    tenant_id    INTEGER      NOT NULL DEFAULT 1,
    warehouse_id VARCHAR(64)  NOT NULL,        -- GUID из 1С (ref_key)
    enabled      BOOLEAN      NOT NULL DEFAULT TRUE,
    note         TEXT,
    created_at   TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
    updated_at   TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
    PRIMARY KEY (tenant_id, warehouse_id)
);

CREATE INDEX IF NOT EXISTS ix_allowed_warehouses_enabled
    ON allowed_warehouses (tenant_id, enabled)
    WHERE enabled = TRUE;
