-- ==========================================================================
-- v_inventory_margin: товар + остаток + себестоимость + маржа
-- ==========================================================================
CREATE OR REPLACE VIEW v_inventory_margin AS
SELECT
    p.id             AS sku_1c,
    p.offer_id,
    p.name,
    COALESCE(SUM(s.quantity), 0)::NUMERIC(14,3) AS total_quantity,
    c.selected_cost  AS cost,
    c.cost_source,
    p.price          AS ozon_price,
    (p.price - c.selected_cost) AS margin,
    CASE
        WHEN p.price > 0
        THEN ROUND(100 * (p.price - c.selected_cost) / p.price, 2)
        ELSE NULL
    END              AS margin_percent
FROM products_1c p
LEFT JOIN inventory_stocks s
       ON s.sku_1c = p.id
LEFT JOIN inventory_costs c
       ON c.sku_1c = p.id
GROUP BY p.id, p.offer_id, p.name, c.selected_cost,
         c.cost_source, p.price;

-- ==========================================================================
-- v_inventory_warnings: проблемные SKU
-- ==========================================================================
CREATE OR REPLACE VIEW v_inventory_warnings AS
SELECT
    s.sku_1c,
    s.offer_id,
    CASE
        WHEN s.offer_id IS NULL OR s.offer_id = ''
            THEN 'missing_offer_id'
        WHEN c.selected_cost IS NULL
            THEN 'missing_cost'
        ELSE 'ok'
    END AS issue
FROM (SELECT DISTINCT sku_1c, offer_id FROM inventory_stocks) s
LEFT JOIN inventory_costs c ON c.sku_1c = s.sku_1c
WHERE s.offer_id IS NULL
   OR s.offer_id = ''
   OR c.selected_cost IS NULL;
