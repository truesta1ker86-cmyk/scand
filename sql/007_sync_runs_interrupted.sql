BEGIN;

-- 1. Убрать старый CHECK (если есть), чтобы можно было вставить 'interrupted'
ALTER TABLE sync_runs
  DROP CONSTRAINT IF EXISTS ck_sync_runs_status;

-- 2. Убедиться, что в таблице нет несовместимых значений
UPDATE sync_runs
   SET status = 'failed'
 WHERE status NOT IN ('running','done','failed','interrupted');

-- 3. Поставить новый CHECK
ALTER TABLE sync_runs
  ADD CONSTRAINT ck_sync_runs_status
  CHECK (status IN ('running','done','failed','interrupted'));

-- 4. Индекс по прерванным задачам
CREATE INDEX IF NOT EXISTS ix_sync_runs_interrupted
    ON sync_runs (tenant_id, source, finished_at DESC)
    WHERE status = 'interrupted';

COMMIT;
