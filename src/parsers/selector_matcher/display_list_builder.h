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

        walk(root, 0.0f, 0.0f, storage, out);
        return out;
    }

private:
    // Text/AnonymousBlock не имеют style_soa_idx Ч поднимаемс€ вверх.
    static uint32_t effective_style_idx(const LayoutNode* node) {
        for (const LayoutNode* n = node; n; n = n->parent)
            if (n->style_soa_idx != UINT32_MAX) return n->style_soa_idx;
        return 0;
    }

    static void walk(const LayoutNode* node, float ox, float oy,
        const StyleStorageSoA& storage, DisplayList& out)
    {
        const float ax = ox + node->geometry.x;
        const float ay = oy + node->geometry.y;
        const float w = node->geometry.width;
        const float h = node->geometry.height;

        // Background + borders Ч только у реальных Element-узлов.
        if ((node->type == BoxType::Block || node->type == BoxType::Inline)
            && node->style_soa_idx != UINT32_MAX)
        {
            const uint32_t si = node->style_soa_idx;

            const uint32_t bg = storage.background_colors[si];
            if ((bg & 0xFFu) != 0u) {  // alpha > 0
                DisplayItem it;
                it.type = DisplayItemType::Rect;
                it.x = ax; it.y = ay; it.width = w; it.height = h;
                it.color = bg;
                out.items.push_back(std::move(it));
            }

            const auto& g = node->geometry;
            constexpr uint32_t kBorderColor = 0x000000FFu;  // TODO: border-color

            auto push_border = [&](float bx, float by, float bw, float bh) {
                DisplayItem bi;
                bi.type = DisplayItemType::Rect;
                bi.x = bx; bi.y = by; bi.width = bw; bi.height = bh;
                bi.color = kBorderColor;
                out.items.push_back(std::move(bi));
                };

            if (g.border_top > 0.0f) push_border(ax, ay, w, g.border_top);
            if (g.border_bottom > 0.0f) push_border(ax, ay + h - g.border_bottom,
                w, g.border_bottom);
            if (g.border_left > 0.0f) push_border(ax, ay, g.border_left, h);
            if (g.border_right > 0.0f) push_border(ax + w - g.border_right, ay,
                g.border_right, h);
        }

        // “екст.
        if (node->type == BoxType::Text) {
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
            out.items.push_back(std::move(it));
        }

        for (const auto& c : node->children)
            walk(c.get(), ax, ay, storage, out);
    }
};