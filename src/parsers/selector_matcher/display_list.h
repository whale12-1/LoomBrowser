#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>


class QImage;

enum class DisplayItemType : uint8_t {
    Rect,   // background / border
    Text,   // текстовый run
    Image
};

struct DisplayItem {
    DisplayItemType type{ DisplayItemType::Rect };
    float x{ 0.0f }, y{ 0.0f };
    float width{ 0.0f }, height{ 0.0f };
    uint32_t color{ 0x00000000u };      // 0xRRGGBBAA
    float opacity{ 1.0f };

    // ---- border-radius: радиусы в px (уже резолвлены) ----
    float radius_tl{ 0.0f }, radius_tr{ 0.0f };
    float radius_br{ 0.0f }, radius_bl{ 0.0f };
    bool  has_radius() const {
        return radius_tl > 0.0f || radius_tr > 0.0f
            || radius_br > 0.0f || radius_bl > 0.0f;
    }

    // ---- Clip-регион (overflow: hidden на предке) ----
    bool  has_clip{ false };
    float clip_x{ 0.0f }, clip_y{ 0.0f };
    float clip_w{ 0.0f }, clip_h{ 0.0f };

    // ---- Только для Text ----
    std::string text;
    float font_size{ 16.0f };
    std::string font_family;
    bool  bold{ false };
    bool  italic{ false };
    bool  underline{ false };
    bool  line_through{ false };
    bool  overline{ false };
    float letter_spacing{ 0.0f };

    // ---- Image ----
    std::string image_path; // Оставляем для совместимости
    std::shared_ptr<QImage> image; // <--- Новое поле
    bool is_background{ false };
    uint8_t  bg_repeat{ 0 };
    uint8_t  bg_size_kind{ 0 };
    float    bg_size_w{ 0.0f };
    float    bg_size_h{ 0.0f };
    float    bg_pos_x{ 0.0f };
    float    bg_pos_y{ 0.0f };
    uint8_t  bg_pos_x_unit{ 1 };
    uint8_t  bg_pos_y_unit{ 1 };
};

struct DisplayList {
    std::vector<DisplayItem> items;
    float document_width{ 0.0f };
    float document_height{ 0.0f };

    void clear() { items.clear(); document_width = document_height = 0.0f; }
    size_t size() const { return items.size(); }
};