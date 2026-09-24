-- =============================================================================
-- Нормализация: привязка складов через FK по (tenant_id, warehouse_id).
-- Имя склада живёт ТОЛЬКО в inventory1c_warehouses.
-- =============================================================================

BEGIN;

-- 1. UNIQUE на inventory1c_warehouses (нужен для FK)
CREATE UNIQUE INDEX IF NOT EXISTS uq_inventory1c_warehouses_tenant_wh
    ON inventory1c_warehouses (tenant_id, warehouse_id);

-- 2. Удаляем колонку warehouse_name из stocks (если есть)
ALTER TABLE inventory1c_stocks
  DROP COLUMN IF EXISTS warehouse_name;

-- 3. На всякий случай: удалить «осиротевшие» stocks, для которых нет склада
DELETE FROM inventory1c_stocks s
 WHERE NOT EXISTS (
    SELECT 1 FROM inventory1c_warehouses w
     WHERE w.tenant_id    = s.tenant_id
       AND w.warehouse_id = s.warehouse_id
 );

-- 4. FK на (tenant_id, warehouse_id)
ALTER TABLE inventory1c_stocks
  DROP CONSTRAINT IF EXISTS fk_inventory1c_stocks_warehouse;
ALTER TABLE inventory1c_stocks
  ADD CONSTRAINT fk_inventory1c_stocks_warehouse
  FOREIGN KEY (tenant_id, warehouse_id)
  REFERENCES inventory1c_warehouses (tenant_id, warehouse_id)
  ON DELETE RESTRICT;

-- 5. View для удобства
CREATE OR REPLACE VIEW inventory1c_stocks_view AS
SELECT
    s.id,
    s.tenant_id,
    s.sku_1c,
    s.offer_id,
    s.warehouse_id,
    w.warehouse_name,
    s.available,
    s.reserved,
    s.measured_at,
    s.batch_id,
    s.created_at
  FROM inventory1c_stocks s
  LEFT JOIN inventory1c_warehouses w
         ON w.tenant_id    = s.tenant_id
        AND w.warehouse_id = s.warehouse_id;

COMMIT;
