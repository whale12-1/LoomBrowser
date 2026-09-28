#pragma once
#include <cstdint>
#include <string>
#include <vector>

enum class DisplayItemType : uint8_t {
    Rect,   // background / border
    Text    // текстовый ран
};

struct DisplayItem {
    DisplayItemType type{ DisplayItemType::Rect };
    float x{ 0.0f }, y{ 0.0f };
    float width{ 0.0f }, height{ 0.0f };
    uint32_t color{ 0x000000FFu };      // 0xRRGGBBAA
    // Только для Text:
    std::string text;
    float font_size{ 16.0f };
    bool  bold{ false };
};

struct DisplayList {
    std::vector<DisplayItem> items;
    float document_width{ 0.0f };
    float document_height{ 0.0f };

    void clear() { items.clear(); document_width = document_height = 0.0f; }
    size_t size() const { return items.size(); }
};