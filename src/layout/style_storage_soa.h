#pragma once
#include <vector>
#include <cstdint>
#include <cstring>
#include "style_types.h"

constexpr uint32_t INVALID_INDEX = UINT32_MAX;

using style::Display;  using style::Position;  using style::Float;
using style::Overflow; using style::Visibility; using style::BoxSizing;
using style::TextAlign; using style::FontStyle; using style::WhiteSpace;
using style::FlexDir;  using style::Justify;    using style::AlignItems;
using style::Length;   using style::Unit;       using style::Color;
using style::Prop;     using style::PROP_MASK_WORDS;

struct StyleStorageSoA {
    // =========================================================
    // 1. Топология (индексы по узлам, а не указатели)
    // =========================================================
    std::vector<uint32_t> parent_indices;
    std::vector<uint32_t> first_child_indices;
    std::vector<uint32_t> last_child_indices;    // ускоряет :last-child, реверс
    std::vector<uint32_t> next_sibling_indices;
    std::vector<DOMNode*> dom_nodes;

    // =========================================================
    // 2. Вычисленные стили
    // =========================================================
    std::vector<Display>    displays;
    std::vector<Position>   positions;
    std::vector<Float>      floats;
    std::vector<Overflow>   overflows;
    std::vector<Visibility> visibilities;
    std::vector<BoxSizing>  box_sizings;
    std::vector<TextAlign>  text_aligns;
    std::vector<FontStyle>  font_styles;
    std::vector<WhiteSpace> white_spaces;
    std::vector<FlexDir>    flex_directions;
    std::vector<Justify>    justify_contents;
    std::vector<AlignItems> align_items;

    std::vector<uint16_t>   font_weights;         // 100..900
    std::vector<uint8_t>    opacity_q;            // opacity * 255
    std::vector<int32_t>    z_indices;

    std::vector<uint32_t>   text_colors;          // 0xRRGGBBAA
    std::vector<uint32_t>   background_colors;

    // Размеры (не разрешены до layout — em/rem/% остаются как есть)
    std::vector<Length>     widths, heights;
    std::vector<Length>     min_widths, max_widths, min_heights, max_heights;
    std::vector<Length>     margin_top, margin_right, margin_bottom, margin_left;
    std::vector<Length>     padding_top, padding_right, padding_bottom, padding_left;
    std::vector<Length>     border_top_width, border_right_width, border_bottom_width, border_left_width;
    std::vector<Length>     flex_bases;
    std::vector<float>      flex_grows, flex_shrinks;

    // Текст — разрешён в px (нужен для resolve em у детей)
    std::vector<float>      font_sizes;
    std::vector<float>      line_heights;         // 0.0 = normal
    std::vector<float>      letter_spacings;

    // =========================================================
    // 3. Служебные битмаски
    // =========================================================
    // explicit_mask[idx] — какие свойства были заданы в CSS явно
    // (битовая маска по Prop::COUNT). Нужна для наследования.
    std::vector<uint64_t>   explicit_mask; // PROP_MASK_WORDS слов на узел

    // =========================================================
    // API
    // =========================================================
    size_t size() const { return parent_indices.size(); }

    uint32_t allocate_node(DOMNode* node, uint32_t parent_idx) {
        const uint32_t i = static_cast<uint32_t>(parent_indices.size());

        parent_indices.push_back(parent_idx);
        first_child_indices.push_back(INVALID_INDEX);
        last_child_indices.push_back(INVALID_INDEX);
        next_sibling_indices.push_back(INVALID_INDEX);
        dom_nodes.push_back(node);

        displays.push_back(Display::Inline);
        positions.push_back(Position::Static);
        floats.push_back(Float::None);
        overflows.push_back(Overflow::Visible);
        visibilities.push_back(Visibility::Visible);
        box_sizings.push_back(BoxSizing::ContentBox);
        text_aligns.push_back(TextAlign::Start);
        font_styles.push_back(FontStyle::Normal);
        white_spaces.push_back(WhiteSpace::Normal);
        flex_directions.push_back(FlexDir::Row);
        justify_contents.push_back(Justify::Start);
        align_items.push_back(AlignItems::Stretch);

        font_weights.push_back(400);
        opacity_q.push_back(255);
        z_indices.push_back(0);

        text_colors.push_back(0x000000FFu);
        background_colors.push_back(0x00000000u);

        widths.push_back({ -1.0f, Unit::Auto });
        heights.push_back({ -1.0f, Unit::Auto });
        min_widths.push_back({ 0.0f, Unit::Px });
        max_widths.push_back({ 0.0f, Unit::None });
        min_heights.push_back({ 0.0f, Unit::Px });
        max_heights.push_back({ 0.0f, Unit::None });
        margin_top.push_back({ 0.0f, Unit::Px });
        margin_right.push_back({ 0.0f, Unit::Px });
        margin_bottom.push_back({ 0.0f, Unit::Px });
        margin_left.push_back({ 0.0f, Unit::Px });
        padding_top.push_back({ 0.0f, Unit::Px });
        padding_right.push_back({ 0.0f, Unit::Px });
        padding_bottom.push_back({ 0.0f, Unit::Px });
        padding_left.push_back({ 0.0f, Unit::Px });
        border_top_width.push_back({ 0.0f, Unit::Px });
        border_right_width.push_back({ 0.0f, Unit::Px });
        border_bottom_width.push_back({ 0.0f, Unit::Px });
        border_left_width.push_back({ 0.0f, Unit::Px });
        flex_bases.push_back({ -1.0f, Unit::Auto });
        flex_grows.push_back(0.0f);
        flex_shrinks.push_back(1.0f);

        font_sizes.push_back(16.0f);
        line_heights.push_back(0.0f);
        letter_spacings.push_back(0.0f);

        explicit_mask.resize((i + 1) * PROP_MASK_WORDS, 0ull);
        return i;
    }

    // --- Работа с маской «явно задано» ---
    void mark_explicit(uint32_t idx, Prop p) {
        uint64_t* word = &explicit_mask[idx * PROP_MASK_WORDS + (style::prop_index(p) >> 6)];
        *word |= (1ull << (style::prop_index(p) & 63));
    }
    bool is_explicit(uint32_t idx, Prop p) const {
        const uint64_t word = explicit_mask[idx * PROP_MASK_WORDS + (style::prop_index(p) >> 6)];
        return (word >> (style::prop_index(p) & 63)) & 1ull;
    }
};