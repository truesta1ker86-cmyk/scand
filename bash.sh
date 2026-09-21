psql -h localhost -U postgres -d o1c <<'SQL'
DROP TABLE IF EXISTS products_1c CASCADE;

CREATE TABLE products_1c (
    id             TEXT PRIMARY KEY,
    offer_id       TEXT,
    name           TEXT,
    price          NUMERIC(12, 2) NOT NULL DEFAULT 0,
    updated_at_db  TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_products_1c_offer_id      ON products_1c(offer_id);
CREATE INDEX idx_products_1c_updated_at_db ON products_1c(updated_at_db);
SQL
