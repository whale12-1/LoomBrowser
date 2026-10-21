#pragma once
#include "display_list.h"
#include "../layout/headers/layout_node.h"
#include "style_storage_soa.h"
#include <algorithm>
#include <QUrl>
#include <QImage>
#include <QString>

class DisplayListBuilder {
public:
    static DisplayList build(const LayoutNode* root,
        const StyleStorageSoA& storage,
        const std::string& base_url = "",
        const std::unordered_map<std::string, std::shared_ptr<QImage>>& image_cache = {})
    {
        DisplayList out;
        if (!root) return out;

        float max_x = 0.0f;
        float max_y = 0.0f;
        compute_bounds(root, max_x, max_y);

        out.document_width = max_x;
        out.document_height = max_y;

        // Передаем base_url и image_cache в walk
        walk(root, storage, out, base_url, image_cache,
            /*has_clip=*/false, 0, 0, 0, 0);
        return out;
    }

private:
    static void compute_bounds(const LayoutNode* node, float& max_x, float& max_y) {
        if (!node) return;
        const float right = node->geometry.x + node->geometry.width + node->geometry.margin_right;
        const float bottom = node->geometry.y + node->geometry.height + node->geometry.margin_bottom;
        max_x = std::max(max_x, right);
        max_y = std::max(max_y, bottom);

        for (const auto& c : node->children) {
            compute_bounds(c.get(), max_x, max_y);
        }
    }

    static uint32_t effective_style_idx(const LayoutNode* node) {
        for (const LayoutNode* n = node; n; n = n->parent)
            if (n->style_soa_idx != UINT32_MAX) return n->style_soa_idx;
        return 0;
    }

    // Резолв радиуса: Length → px.  Проценты — от min(w,h).
    static float resolve_radius(const style::Length& l, float w, float h) {
        if (l.is_auto() || l.is_none()) return 0.0f;
        if (l.unit == Unit::Percent) {
            const float base = std::min(w, h);
            return l.value * 0.01f * base;
        }
        return l.value;   // px/em/rem уже в value после parse_length
    }

    static void fill_radius(DisplayItem& it,
        const LayoutNode* node, const StyleStorageSoA& storage,
        float w, float h)
    {
        if (node->style_soa_idx == UINT32_MAX) return;
        const uint32_t si = node->style_soa_idx;
        it.radius_tl = resolve_radius(storage.border_radius_tl[si], w, h);
        it.radius_tr = resolve_radius(storage.border_radius_tr[si], w, h);
        it.radius_br = resolve_radius(storage.border_radius_br[si], w, h);
        it.radius_bl = resolve_radius(storage.border_radius_bl[si], w, h);
    }

    static void fill_clip(DisplayItem& it,
        bool has_clip, float cx, float cy, float cw, float ch)
    {
        if (!has_clip) return;
        it.has_clip = true;
        it.clip_x = cx; it.clip_y = cy;
        it.clip_w = cw; it.clip_h = ch;
    }

    static void walk(const LayoutNode* node,
        const StyleStorageSoA& storage, DisplayList& out,
        const std::string& base_url,
        const std::unordered_map<std::string, std::shared_ptr<QImage>>& image_cache,
        bool parent_has_clip,
        float parent_clip_x, float parent_clip_y,
        float parent_clip_w, float parent_clip_h)
    {
        const float ax = node->geometry.x;
        const float ay = node->geometry.y;
        const float w = node->geometry.width;
        const float h = node->geometry.height;

        // Вспомогательная лямбда для поиска картинки в кэше
        auto get_img = [&](const std::string& src) -> std::shared_ptr<QImage> {
            if (src.empty()) return nullptr;
            QUrl base_qurl(QString::fromStdString(base_url));
            QUrl abs_qurl = base_qurl.resolved(QUrl(QString::fromStdString(src)));
            std::string abs_url = abs_qurl.toString().toStdString();
            auto it = image_cache.find(abs_url);
            return (it != image_cache.end()) ? it->second : nullptr;
            };

        // ---- <br> — пропускаем сам узел, но обходим детей ----
        if (node->style_soa_idx != UINT32_MAX) {
            const DOMNode* dom = storage.dom_nodes[node->style_soa_idx];
            if (dom && std::string_view(dom->tag_name.data(),
                dom->tag_name.size()) == "br") {
                for (const auto& c : node->children)
                    walk(c.get(), storage, out, base_url, image_cache,
                        parent_has_clip, parent_clip_x, parent_clip_y,
                        parent_clip_w, parent_clip_h);
                return;
            }
        }

        // ---- Обновляем clip-регион, если у узла overflow != visible ----
        bool has_clip = parent_has_clip;
        float clip_x = parent_clip_x, clip_y = parent_clip_y;
        float clip_w = parent_clip_w, clip_h = parent_clip_h;

        if (node->style_soa_idx != UINT32_MAX) {
            const uint32_t si = node->style_soa_idx;
            if (storage.overflows[si] != style::Overflow::Visible) {
                // Пересечение с border-box этого узла.
                if (has_clip) {
                    const float nx0 = std::max(clip_x, ax);
                    const float ny0 = std::max(clip_y, ay);
                    const float nx1 = std::min(clip_x + clip_w, ax + w);
                    const float ny1 = std::min(clip_y + clip_h, ay + h);
                    clip_x = nx0; clip_y = ny0;
                    clip_w = std::max(0.0f, nx1 - nx0);
                    clip_h = std::max(0.0f, ny1 - ny0);
                }
                else {
                    clip_x = ax; clip_y = ay;
                    clip_w = w;  clip_h = h;
                    has_clip = true;
                }
            }
        }

        // ---- visibility / opacity ----
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

        // ---- Background + borders ----
        if (!node_hidden
            && (node->type == BoxType::Block
                || node->type == BoxType::Inline
                || node->type == BoxType::Image
                || node->type == BoxType::Table
                || node->type == BoxType::TableRow
                || node->type == BoxType::TableCell)
            && node->style_soa_idx != UINT32_MAX)
        {
            const uint32_t si = node->style_soa_idx;

            // --- background-color ---
            const uint32_t bg = storage.background_colors[si];
            if ((bg & 0xFFu) != 0u && w > 0.0f && h > 0.0f) {
                DisplayItem it;
                it.type = DisplayItemType::Rect;
                it.x = ax; it.y = ay; it.width = w; it.height = h;
                it.color = bg;
                it.opacity = item_opacity;
                fill_radius(it, node, storage, w, h);
                fill_clip(it, has_clip, clip_x, clip_y, clip_w, clip_h);
                out.items.push_back(std::move(it));
            }

            // --- background-image ---
            const std::string& bg_img = storage.background_images[si];
            if (!bg_img.empty()) {
                DisplayItem it;
                it.type = DisplayItemType::Image;
                it.color = 0x00000000u;
                it.x = ax; it.y = ay; it.width = w; it.height = h;
                it.image_path = bg_img;
                it.image = get_img(bg_img); // <--- БЕРЕМ ИЗ КЭША
                if (it.image || !bg_img.empty()) {
                    it.opacity = item_opacity;
                    it.is_background = true;
                    fill_radius(it, node, storage, w, h);
                    fill_clip(it, has_clip, clip_x, clip_y, clip_w, clip_h);
                    // repeat
                    switch (storage.background_repeats[si]) {
                    case style::BackgroundRepeat::NoRepeat: it.bg_repeat = 3; break;
                    case style::BackgroundRepeat::RepeatX:  it.bg_repeat = 1; break;
                    case style::BackgroundRepeat::RepeatY:  it.bg_repeat = 2; break;
                    default:                                it.bg_repeat = 0; break;
                    }

                    // size — резолвим px/percent в builder'е, cover/contain
                    // оставляем рендереру.
                    const auto& bsz = storage.background_sizes[si];
                    switch (bsz.kind) {
                    case style::BgSizeKind::Auto:    it.bg_size_kind = 0; break;
                    case style::BgSizeKind::Cover:   it.bg_size_kind = 1; break;
                    case style::BgSizeKind::Contain: it.bg_size_kind = 2; break;
                    case style::BgSizeKind::Explicit: {
                        it.bg_size_kind = 3;
                        auto rw = [&](const style::Length& l, float base) {
                            if (l.is_auto() || l.is_none()) return 0.0f;
                            if (l.unit == Unit::Percent)
                                return l.value * 0.01f * base;
                            return l.value;
                            };
                        it.bg_size_w = rw(bsz.w, w);
                        it.bg_size_h = rw(bsz.h, h);
                        break;
                    }
                    }

                    // position
                    const auto& bp = storage.background_positions[si];
                    auto set_unit = [](const style::Length& l,
                        float& out_val, uint8_t& out_unit)
                        {
                            if (l.unit == Unit::Percent) {
                                out_val = l.value;
                                out_unit = 1;
                            }
                            else {
                                out_val = l.value;
                                out_unit = 0;
                            }
                        };
                    set_unit(bp.x, it.bg_pos_x, it.bg_pos_x_unit);
                    set_unit(bp.y, it.bg_pos_y, it.bg_pos_y_unit);

                    out.items.push_back(std::move(it));
                }
            }

            // --- borders (прямые прямоугольники; rounded-border
            //     в этой итерации рисуется как прямоугольник) ---
            const uint32_t top_color = storage.border_top_colors[si];
            const uint32_t right_color = storage.border_right_colors[si];
            const uint32_t bottom_color = storage.border_bottom_colors[si];
            const uint32_t left_color = storage.border_left_colors[si];

            auto push_border = [&](float bx, float by, float bw, float bh,
                uint32_t color)
                {
                    if ((color & 0xFFu) == 0u) return;
                    DisplayItem bi;
                    bi.type = DisplayItemType::Rect;
                    bi.x = bx; bi.y = by;
                    bi.width = bw; bi.height = bh;
                    bi.color = color;
                    bi.opacity = item_opacity;
                    fill_clip(bi, has_clip, clip_x, clip_y, clip_w, clip_h);
                    out.items.push_back(std::move(bi));
                };

            const auto& g = node->geometry;
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

        // ---- Text ----
        if (!node_hidden && node->type == BoxType::Text) {
            const uint32_t si = effective_style_idx(node);
            DisplayItem it;
            it.type = DisplayItemType::Text;
            it.x = ax; it.y = ay;
            it.width = w; it.height = h;
            it.color = storage.text_colors[si];
            it.text = node->text_content;
            it.font_size = storage.font_sizes[si];
            it.font_family = storage.font_families[si];
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
            fill_clip(it, has_clip, clip_x, clip_y, clip_w, clip_h);
            out.items.push_back(std::move(it));
        }

        // ---- <img> ----
        if (!node_hidden && node->type == BoxType::Image
            && !node->image_src.empty()) {
            DisplayItem it;
            it.type = DisplayItemType::Image;
            it.x = ax; it.y = ay;
            it.width = w; it.height = h;
            it.image_path = node->image_src;
            it.image = get_img(node->image_src);

            // ИСПРАВЛЕНИЕ: Пропускаем только если картинка загружена и это заглушка 1x1.
            // Если картинки нет в кэше (!it.image), мы всё равно добавляем её в список, 
            // чтобы рендерер загрузил её с диска/сети.
            if (!it.image || (it.image->width() > 1 || it.image->height() > 1)) {
                it.opacity = item_opacity;
                it.is_background = false;
                fill_radius(it, node, storage, w, h);
                fill_clip(it, has_clip, clip_x, clip_y, clip_w, clip_h);
                out.items.push_back(std::move(it));
            }
        }

        // ---- Дети ----
        for (const auto& c : node->children)
            walk(c.get(), storage, out, base_url, image_cache,
                has_clip, clip_x, clip_y, clip_w, clip_h);
    }
};