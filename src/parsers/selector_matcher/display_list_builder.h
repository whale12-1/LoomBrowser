#pragma once
#include "display_list.h"
#include "../layout/headers/layout_node.h"
#include "style_storage_soa.h"

class DisplayListBuilder {
public:
    static DisplayList build(const LayoutNode* root,
        const StyleStorageSoA& storage)
    {
        DisplayList out;
        if (!root) return out;

        out.document_width = root->geometry.width;
        out.document_height = root->geometry.height;

        walk(root, storage, out);
        return out;
    }

private:
    // Text/AnonymousBlock �� ����� style_soa_idx � ����������� �����.
    static uint32_t effective_style_idx(const LayoutNode* node) {
        for (const LayoutNode* n = node; n; n = n->parent)
            if (n->style_soa_idx != UINT32_MAX) return n->style_soa_idx;
        return 0;
    }

    static void walk(const LayoutNode* node,
        const StyleStorageSoA& storage, DisplayList& out)
    {
        const float ax = node->geometry.x;
        const float ay = node->geometry.y;
        const float w = node->geometry.width;
        const float h = node->geometry.height;

        // <br> — нет своего бокса, но обход детей продолжаем.
        if (node->style_soa_idx != UINT32_MAX) {
            const DOMNode* dom = storage.dom_nodes[node->style_soa_idx];
            if (dom && std::string_view(dom->tag_name.data(), dom->tag_name.size()) == "br") {
                for (const auto& c : node->children)
                    walk(c.get(), storage, out);
                return;
            }
        }

        // ---- Visibility и opacity ----
        // visibility наследуется, поэтому для Text берём эффективный
        // style_soa_idx (придёт от родителя).
        // opacity не наследуется как свойство каскада; но для Text
        // также используем effective — так текст визуально тускнеет
        // внутри родителя с opacity < 1. Композитинг не реализован,
        // это компромисс (TODO).
        bool node_hidden = false;
        float item_opacity = 1.0f;
        {
            const uint32_t si_eff =
                (node->style_soa_idx != UINT32_MAX)
                ? node->style_soa_idx
                : (node->type == BoxType::Text)
                ? effective_style_idx(node)
                : UINT32_MAX;
            if (si_eff != UINT32_MAX) {
                const auto vis = storage.visibilities[si_eff];
                node_hidden = (vis == style::Visibility::Hidden
                    || vis == style::Visibility::Collapse);
                item_opacity = storage.opacity_q[si_eff] / 255.0f;
            }
        }

        // Background + borders — только для видимых real-element'ов.
        if (!node_hidden
            && (node->type == BoxType::Block
                || node->type == BoxType::Inline
                || node->type == BoxType::Image)
            && node->style_soa_idx != UINT32_MAX)
        {
            const uint32_t si = node->style_soa_idx;

            const uint32_t bg = storage.background_colors[si];
            if ((bg & 0xFFu) != 0u) {
                DisplayItem it;
                it.type = DisplayItemType::Rect;
                it.x = ax; it.y = ay; it.width = w; it.height = h;
                it.color = bg;
                it.opacity = item_opacity;
                out.items.push_back(std::move(it));
            }

            // background-image рисуется поверх background-color.
            // Пока всегда растягивается на border-box — без
            // background-size / -repeat / -position (TODO).
            const std::string& bg_img = storage.background_images[si];
            if (!bg_img.empty()) {
                DisplayItem it;
                it.type = DisplayItemType::Image;
                it.x = ax; it.y = ay; it.width = w; it.height = h;
                it.image_path = bg_img;
                it.opacity = item_opacity;
                out.items.push_back(std::move(it));
            }

            const auto& g = node->geometry;
            const uint32_t top_color = storage.border_top_colors[si];
            const uint32_t right_color = storage.border_right_colors[si];
            const uint32_t bottom_color = storage.border_bottom_colors[si];
            const uint32_t left_color = storage.border_left_colors[si];

            auto push_border = [&](float bx, float by, float bw, float bh,
                uint32_t color) {
                    if ((color & 0xFFu) == 0u) return;
                    DisplayItem bi;
                    bi.type = DisplayItemType::Rect;
                    bi.x = bx; bi.y = by; bi.width = bw; bi.height = bh;
                    bi.color = color;
                    bi.opacity = item_opacity;
                    out.items.push_back(std::move(bi));
                };

            if (g.border_top > 0.0f)
                push_border(ax, ay, w, g.border_top, top_color);
            if (g.border_bottom > 0.0f)
                push_border(ax, ay + h - g.border_bottom,
                    w, g.border_bottom, bottom_color);
            if (g.border_left > 0.0f)
                push_border(ax, ay, g.border_left, h, left_color);
            if (g.border_right > 0.0f)
                push_border(ax + w - g.border_right, ay,
                    g.border_right, h, right_color);
        }

        // Text — только если узел видим.
        if (!node_hidden && node->type == BoxType::Text) {
            const uint32_t si = effective_style_idx(node);
            DisplayItem it;
            it.type = DisplayItemType::Text;
            it.x = ax;
            it.y = ay;
            it.width = w;
            it.height = h;
            it.color = storage.text_colors[si];
            it.text = node->text_content;
            it.font_size = storage.font_sizes[si];
            it.bold = storage.font_weights[si] >= 700;

            const auto fstyle = storage.font_styles[si];
            it.italic = (fstyle == style::FontStyle::Italic
                || fstyle == style::FontStyle::Oblique);
            it.letter_spacing = storage.letter_spacings[si];

            const auto dec = storage.text_decorations[si];
            it.underline = (static_cast<uint8_t>(dec) & 1u) != 0u;
            it.overline = (static_cast<uint8_t>(dec) & 2u) != 0u;
            it.line_through = (static_cast<uint8_t>(dec) & 4u) != 0u;

            it.opacity = item_opacity;
            out.items.push_back(std::move(it));
        }

        // Image — если узел видим и путь не пуст.
        if (!node_hidden && node->type == BoxType::Image
            && !node->image_src.empty()) {
            DisplayItem it;
            it.type = DisplayItemType::Image;
            it.x = ax;
            it.y = ay;
            it.width = w;
            it.height = h;
            it.image_path = node->image_src;
            it.opacity = item_opacity;
            out.items.push_back(std::move(it));
        }

        for (const auto& c : node->children)
            walk(c.get(), storage, out);
    }
};