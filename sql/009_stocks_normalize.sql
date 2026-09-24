-- =============================================================================
-- Убираем денормализованное warehouse_name из inventory1c_stocks.
-- Имя склада живёт только в inventory1c_warehouses, привязка — по warehouse_id.
-- При переименовании склада в 1С ничего не ломается.
-- =============================================================================

-- 1. Индекс и FK на inventory1c_warehouses.warehouse_id
CREATE UNIQUE INDEX IF NOT EXISTS uq_inventory1c_warehouses_tenant_wh
    ON inventory1c_warehouses (tenant_id, warehouse_id);

-- 2. Дропаем старую колонку warehouse_name из stocks
ALTER TABLE inventory1c_stocks
  DROP COLUMN IF EXISTS warehouse_name;

-- 3. Индекс по warehouse_id уже есть — оставляем как есть.
