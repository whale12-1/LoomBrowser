#pragma once
#include <cstdint>
#include <string>
#include <vector>

enum class DisplayItemType : uint8_t {
    Rect,   // background / border
    Text,   // текстовый run
    Image
};

struct DisplayItem {
    DisplayItemType type{ DisplayItemType::Rect };
    float x{ 0.0f }, y{ 0.0f };
    float width{ 0.0f }, height{ 0.0f };
    uint32_t color{ 0x000000FFu };      // 0xRRGGBBAA
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
    std::string image_path;
    // true → это background-image (применяются bg_* поля ниже);
    // false → это <img> (stretch по x/y/w/h).
    bool is_background{ false };
    uint8_t  bg_repeat{ 0 };      // 0=repeat 1=repeat-x 2=repeat-y 3=no-repeat
    uint8_t  bg_size_kind{ 0 };   // 0=auto 1=cover 2=contain 3=explicit
    float    bg_size_w{ 0.0f };   // px, только для kind==explicit
    float    bg_size_h{ 0.0f };
    // Позиция: если unit == 1 (percent) → рендерер сам считает offset
    //   offset = (container_size - image_size) * percent / 100
    // Если unit == 0 (px) → offset уже в value (готовый сдвиг).
    float    bg_pos_x{ 0.0f };
    float    bg_pos_y{ 0.0f };
    uint8_t  bg_pos_x_unit{ 1 };  // 0=px 1=percent
    uint8_t  bg_pos_y_unit{ 1 };
};

struct DisplayList {
    std::vector<DisplayItem> items;
    float document_width{ 0.0f };
    float document_height{ 0.0f };

    void clear() { items.clear(); document_width = document_height = 0.0f; }
    size_t size() const { return items.size(); }
};