#pragma once
#include <string>

// ВАЖНО: раньше тип назывался Product_1с с КИРИЛЛИЧЕСКОЙ 'с'.
// Это приводило к невидимым проблемам при поиске/наборе.
// Теперь — латинская 'C'. НЕ возвращать кириллицу!
struct Product1C {
    std::string id;
    std::string offer_id;
    std::string name;
    double      price = 0.0;
};

struct OzonProduct {
    std::string id;
    std::string sku;
    std::string name;
    double      price    = 0.0;
    std::string currency;
    int         in_stock = 0;
    std::string description;
    int         weight   = 0;
    std::string created_at;
    std::string updated_at;
    std::string scategorie;
    std::string related_products;
};
