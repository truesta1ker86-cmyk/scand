#pragma once
#include <string>

struct Product_1с {
    std::string id;
    std::string offer_id;
    std::string name;
    double      price = 0.0;
};


struct OzonProduct {
    std::string id;
    std::string sku;
    std::string name;
    double price;
    std::string currency;
    int in_stock;
    std::string description;
    int weight;
    std::string created_at;
    std::string updated_at;
    std::string scategorie;
    std::string related_products;
};
