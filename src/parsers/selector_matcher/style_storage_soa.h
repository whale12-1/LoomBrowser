#pragma once
#include <vector>
#include <cstdint>
#include <cstring>
#include "style_types.h"
#include "./html_parser/headers/dom.h"

constexpr uint32_t INVALID_INDEX = UINT32_MAX;

using style::Position;  using style::Float;
using style::Overflow; using style::Visibility; using style::BoxSizing;
using style::TextAlign; using style::FontStyle; using style::WhiteSpace;
using style::FlexDir;  using style::Justify;    using style::AlignItems;
using style::BackgroundRepeat; using style::BackgroundSize;
using style::BackgroundPosition; using style::BgSizeKind;
using style::VerticalAlign;using style::Clear;
using style::Length;   using style::Unit;       using style::Color;
using style::Prop;     using style::PROP_MASK_WORDS;


// style_storage_soa.h (������ ��� ����� �� struct StyleStorageSoA)


struct StyleStorageSoA {
    // =========================================================
    // 1. ��������� (������� �� �����, � �� ���������)
    // =========================================================
    std::vector<uint32_t> parent_indices;
    std::vector<uint32_t> first_child_indices;
    std::vector<uint32_t> last_child_indices;    // �������� :last-child, ������
    std::vector<uint32_t> next_sibling_indices;
    std::vector<DOMNode*> dom_nodes;

    // =========================================================
    // 2. ����������� �����
    // =========================================================
    std::vector<style::Display>    displays;
    std::vector<Position>   positions;
    std::vector<Float>      floats;
    std::vector<Clear> clears;
    std::vector<Overflow>   overflows;
    std::vector<Visibility> visibilities;
    std::vector<BoxSizing>  box_sizings;
    std::vector<TextAlign>  text_aligns;
    std::vector<style::TextDecoration> text_decorations;
    std::vector<FontStyle>  font_styles;
    std::vector<WhiteSpace> white_spaces;
    std::vector<FlexDir>    flex_directions;
    std::vector<Justify>    justify_contents;
    std::vector<AlignItems> align_items;
    std::vector<VerticalAlign> vertical_aligns;
    std::vector<std::unordered_map<std::string, std::string>> custom_props;

    std::vector<uint16_t>   font_weights;         // 100..900
    std::vector<uint8_t>    opacity_q;            // opacity * 255
    std::vector<int32_t>    z_indices;

    std::vector<uint32_t>   text_colors;          // 0xRRGGBBAA
    std::vector<uint32_t>   background_colors;

    std::vector<std::string> background_images;
    // ������� (�� ��������� �� layout � em/rem/% �������� ��� ����)
    std::vector<Length>     widths, heights;
    std::vector<Length>     min_widths, max_widths, min_heights, max_heights;
    std::vector<Length>     top_insets, right_insets, bottom_insets, left_insets;
    std::vector<Length>     margin_top, margin_right, margin_bottom, margin_left;
    std::vector<Length>     padding_top, padding_right, padding_bottom, padding_left;
    std::vector<Length>     border_top_width, border_right_width, border_bottom_width, border_left_width;
    std::vector<uint32_t>   border_top_colors, border_right_colors, border_bottom_colors, border_left_colors;
    // border-radius: четыре угла, Length (px/em/% — резолвится
// в display_list_builder'е, где известны размеры).
    std::vector<Length>   border_radius_tl, border_radius_tr,
        border_radius_br, border_radius_bl;

    // background-*.
    std::vector<BackgroundSize>     background_sizes;
    std::vector<BackgroundPosition> background_positions;
    std::vector<BackgroundRepeat>   background_repeats;
    std::vector<Length>     flex_bases;
    std::vector<float>      flex_grows, flex_shrinks;
    std::vector<style::FlexWrap> flex_wraps;
    std::vector<style::AlignContent> align_contents;
    std::vector<style::AlignSelf> align_selves;
    std::vector<int32_t> flex_orders;
    std::vector<Length> row_gaps, column_gaps;

    // ����� � �������� � px (����� ��� resolve em � �����)
    std::vector<float>      font_sizes;
    std::vector<std::string> font_families;       // Наследуемое свойство
    std::vector<float>      line_heights;         // 0.0 = normal
    std::vector<float>      letter_spacings;

    // =========================================================
    // 3. ��������� ��������
    // =========================================================
    // explicit_mask[idx] � ����� �������� ���� ������ � CSS ����
    // (������� ����� �� Prop::COUNT). ����� ��� ������������.
    std::vector<uint64_t>   explicit_mask; // PROP_MASK_WORDS ���� �� ����

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

        displays.push_back(style::Display::Inline);
        positions.push_back(Position::Static);
        floats.push_back(Float::None);
        clears.push_back(Clear::None);
        overflows.push_back(Overflow::Visible);
        visibilities.push_back(Visibility::Visible);
        box_sizings.push_back(BoxSizing::ContentBox);
        text_aligns.push_back(TextAlign::Start);
        text_decorations.push_back(style::TextDecoration::None);
        font_styles.push_back(FontStyle::Normal);
        white_spaces.push_back(WhiteSpace::Normal);
        flex_directions.push_back(FlexDir::Row);
        justify_contents.push_back(Justify::Start);
        align_items.push_back(AlignItems::Stretch);
        vertical_aligns.push_back(VerticalAlign::Baseline);

        font_weights.push_back(400);
        opacity_q.push_back(255);
        z_indices.push_back(0);

        text_colors.push_back(0x000000FFu);
        background_colors.push_back(0x00000000u);
        background_images.push_back(std::string{});

        widths.push_back({ -1.0f, Unit::Auto });
        heights.push_back({ -1.0f, Unit::Auto });
        min_widths.push_back({ 0.0f, Unit::Px });
        max_widths.push_back({ 0.0f, Unit::None });
        min_heights.push_back({ 0.0f, Unit::Px });
        max_heights.push_back({ 0.0f, Unit::None });
        top_insets.push_back({ 0.0f, Unit::Auto });
        right_insets.push_back({ 0.0f, Unit::Auto });
        bottom_insets.push_back({ 0.0f, Unit::Auto });
        left_insets.push_back({ 0.0f, Unit::Auto });
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
        // Дефолт border-color — чёрный (как в браузерах).
        border_top_colors.push_back(0x000000FFu);
        border_right_colors.push_back(0x000000FFu);
        border_bottom_colors.push_back(0x000000FFu);
        border_left_colors.push_back(0x000000FFu);
        border_radius_tl.push_back({ 0.0f, Unit::Px });
        border_radius_tr.push_back({ 0.0f, Unit::Px });
        border_radius_br.push_back({ 0.0f, Unit::Px });
        border_radius_bl.push_back({ 0.0f, Unit::Px });

        background_sizes.push_back({});      // Auto
        background_positions.push_back({});  // 0% 0%
        background_repeats.push_back(BackgroundRepeat::Repeat);
        flex_bases.push_back({ -1.0f, Unit::Auto });
        flex_grows.push_back(0.0f);
        flex_shrinks.push_back(1.0f);

        flex_wraps.push_back(style::FlexWrap::Nowrap);
        align_contents.push_back(style::AlignContent::Stretch);
        align_selves.push_back(style::AlignSelf::Auto);
        flex_orders.push_back(0);
        row_gaps.push_back({ 0.0f, Unit::Px });
        column_gaps.push_back({ 0.0f, Unit::Px });

        font_sizes.push_back(16.0f);
        font_families.push_back(""); // пустая строка = дефолтный шрифт системы
        line_heights.push_back(0.0f);
        letter_spacings.push_back(0.0f);
        custom_props.push_back({});
        explicit_mask.resize((i + 1) * PROP_MASK_WORDS, 0ull);
        return i;
    }

    // --- ������ � ������ ����� ������ ---
    void mark_explicit(uint32_t idx, Prop p) {
        uint64_t* word = &explicit_mask[idx * PROP_MASK_WORDS + (style::prop_index(p) >> 6)];
        *word |= (1ull << (style::prop_index(p) & 63));
    }
    bool is_explicit(uint32_t idx, Prop p) const {
        const uint64_t word = explicit_mask[idx * PROP_MASK_WORDS + (style::prop_index(p) >> 6)];
        return (word >> (style::prop_index(p) & 63)) & 1ull;
    }
};

namespace style {
    inline float resolve_line_height(const StyleStorageSoA& s, uint32_t idx) {
        const float lh = s.line_heights[idx];
        const float fs = s.font_sizes[idx];
        if (lh == 0.0f) return fs * 1.2f;   // normal
        if (lh < 0.0f) return -lh * fs;    // unitless multiplier
        return lh;                          // px
    }
}