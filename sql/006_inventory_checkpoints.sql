CREATE TABLE IF NOT EXISTS inventory1c_checkpoints (
    id              BIGSERIAL PRIMARY KEY,
    tenant_id       INTEGER     NOT NULL DEFAULT 1,
    job_id          TEXT        NOT NULL,
    source          TEXT        NOT NULL DEFAULT 'inventory_pull',

    -- Последняя завершённая стадия: catalog | ka2_pull | save | done
    last_stage      TEXT        NOT NULL DEFAULT 'start',

    -- Частичная информация для возобновления
    product_keys_json  JSONB,        -- список ref_key, с которыми работали
    offer_by_ref_json  JSONB,        -- {ref_key: article}
    period             TEXT,         -- "2026-09-23T15:08:23"
    batch_id           TEXT,         -- batch_id снимка

    -- Счётчики для информации
    products_accepted  BIGINT      DEFAULT 0,
    products_migrated  BIGINT      DEFAULT 0,
    products_conflicts BIGINT      DEFAULT 0,

    started_at         TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at         TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    finished_at        TIMESTAMPTZ,

    status             TEXT        NOT NULL DEFAULT 'running',  -- running | done | failed
    error              TEXT
);

-- Только один активный чекпоинт на (tenant, source)
CREATE UNIQUE INDEX IF NOT EXISTS uq_inventory_checkpoints_active
    ON inventory1c_checkpoints (tenant_id, source)
    WHERE status = 'running';

-- Индекс для запроса «последний незавершённый»
CREATE INDEX IF NOT EXISTS ix_inventory_checkpoints_lookup
    ON inventory1c_checkpoints (tenant_id, source, status, updated_at DESC);
