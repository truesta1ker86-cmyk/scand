-- История запусков синхронизаций (inventory_pull, ozon, ...)
CREATE TABLE IF NOT EXISTS sync_runs (
    id              BIGSERIAL PRIMARY KEY,
    tenant_id       INTEGER     NOT NULL DEFAULT 1,
    source          TEXT        NOT NULL,            -- inventory_pull | ozon | catalog_pull
    job_id          TEXT        NOT NULL,
    status          TEXT        NOT NULL,            -- running | done | failed | interrupted
    stage           TEXT,                            -- catalog | ka2_pull | save | done | error
    started_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    finished_at     TIMESTAMPTZ,
    duration_ms     BIGINT,
    accepted        BIGINT      DEFAULT 0,
    migrated        BIGINT      DEFAULT 0,
    conflicts       BIGINT      DEFAULT 0,
    warehouses      BIGINT      DEFAULT 0,
    stocks          BIGINT      DEFAULT 0,
    costs           BIGINT      DEFAULT 0,
    stocks_accepted BIGINT      DEFAULT 0,
    costs_accepted  BIGINT      DEFAULT 0,
    error           TEXT
);

-- Ограничение допустимых статусов
ALTER TABLE sync_runs
  DROP CONSTRAINT IF EXISTS ck_sync_runs_status;
ALTER TABLE sync_runs
  ADD CONSTRAINT ck_sync_runs_status
  CHECK (status IN ('running','done','failed','interrupted'));

-- Индекс для последнего успешного запуска
CREATE INDEX IF NOT EXISTS ix_sync_runs_source_status_fin
    ON sync_runs (tenant_id, source, finished_at DESC NULLS LAST)
    WHERE status = 'done';

-- Индекс по job_id
CREATE INDEX IF NOT EXISTS ix_sync_runs_job
    ON sync_runs (job_id);

-- Индекс по прерванным задачам (для /api/sync/last-run)
CREATE INDEX IF NOT EXISTS ix_sync_runs_interrupted
    ON sync_runs (tenant_id, source, updated_at DESC)
    WHERE status = 'interrupted';
