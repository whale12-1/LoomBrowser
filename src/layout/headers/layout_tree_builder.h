#pragma once

#include <memory>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <cctype>
#include <cmath>
#include <algorithm>

#include "layout_node.h"
#include "./parsers/selector_matcher/style_storage_soa.h"
#include "./parsers/html_parser/headers/dom.h"


class LayoutTreeBuilder {
public:

    struct Viewport {
        float width = 1024.0f;
        float height = 768.0f;
    };
    // =================================================================
    //  Phase 1: построение чистого Layout-дерева из SoA + DOM
    // =================================================================
    static std::unique_ptr<LayoutNode> build(const StyleStorageSoA& storage) {
        if (storage.size() == 0) return nullptr;

        // Обратный индекс DOMNode* -> soa_idx, чтобы из DOM-детей быстро
        // попадать в SoA-стили. O(N).
        std::unordered_map<const DOMNode*, uint32_t> dom_to_soa;
        dom_to_soa.reserve(storage.size() * 2);
        for (uint32_t i = 0; i < storage.size(); ++i) {
            const DOMNode* d = storage.dom_nodes[i];
            if (d) dom_to_soa[d] = i;
        }

        auto root = build_node(0, storage, dom_to_soa);
        if (!root) return nullptr;

        wrap_anonymous_blocks(root.get());
        return root;
    }

    // =================================================================
    //  Phase 2: BFC layout. initial_parent_width — обычно ширина viewport'а.
    // =================================================================
    static void compute_layout(LayoutNode* root, const Viewport& viewport,
        const StyleStorageSoA& storage)
    {
        if (!root) return;
        const float root_fs = storage.font_sizes.empty() ? 16.0f : storage.font_sizes[0];
        layout_node(root, viewport.width, viewport, storage, root_fs);
    }

private:
    // -----------------------------------------------------------------
    //  Phase 1: построение
    // -----------------------------------------------------------------
    static std::unique_ptr<LayoutNode> build_node(
        uint32_t soa_idx,
        const StyleStorageSoA& storage,
        const std::unordered_map<const DOMNode*, uint32_t>& dom_to_soa)
    {
        // Шаг 1: display:none — пропускаем узел и всех потомков
        if (storage.displays[soa_idx] == Display::None) return nullptr;

        const DOMNode* dom = storage.dom_nodes[soa_idx];
        if (!dom) return nullptr;

        auto node = std::make_unique<LayoutNode>();
        node->style_soa_idx = soa_idx;
        node->type = classify_box(storage.displays[soa_idx]);

        // Шаг 3: рекурсивно обходим DOM-детей, чтобы сохранить порядок
        // текстовых и элементных узлов (нужен для корректной обёртки
        // inline-ранов в анонимные блоки).
        for (const DOMNode* child_dom : dom->children) {
            if (!child_dom) continue;

            if (child_dom->type == NodeType::Element &&
                isNonRendered(child_dom->tag_name)) continue;

            if (child_dom->type == NodeType::Text) {
                if (!has_visible_text(child_dom->text_content)) continue;

                auto text = std::make_unique<LayoutNode>();
                text->type = BoxType::Text;
                text->style_soa_idx = UINT32_MAX;   // стили унаследуются
                text->text_content = child_dom->text_content;
                node->add_child(std::move(text));
            }
            else if (child_dom->type == NodeType::Element) {
                auto it = dom_to_soa.find(child_dom);
                if (it == dom_to_soa.end()) continue;

                auto sub = build_node(it->second, storage, dom_to_soa);
                if (sub) node->add_child(std::move(sub));
            }
        }
        return node;
    }

    static bool has_visible_text(const std::string& s) {
        for (char c : s) if (!std::isspace((unsigned char)c)) return true;
        return false;
    }

    // Элементы, которые не создают layout-узлов и не рендерятся.
// Их содержимое либо служебное (title, meta), либо предназначено
// для других подсистем (style → CSS, script → JS).
    static bool isNonRendered(const std::string& tag) {
        return tag == "head" || tag == "style" || tag == "script" ||
            tag == "title" || tag == "meta" || tag == "link" ||
            tag == "base" || tag == "noscript" || tag == "template";
    }

    // Шаг 2: display -> BoxType
    static BoxType classify_box(Display d) {
        switch (d) {
        case Display::Block:
        case Display::Flex:
        case Display::Grid:
        case Display::Table:
        case Display::TableRow:
        case Display::TableCell:
            return BoxType::Block;

        case Display::Inline:
        case Display::InlineBlock:
        case Display::InlineFlex:
        case Display::InlineGrid:
        case Display::Contents:
            return BoxType::Inline;

        case Display::None:
            break; // недостижимо — отфильтровано в build_node
        }
        return BoxType::Block;
    }

    // -----------------------------------------------------------------
    //  Шаг 3 (по ТЗ): анонимные блоки
    //  Если у Block есть одновременно inline/text и block дети —
    //  подряд идущие inline/text оборачиваются в AnonymousBlock.
    // -----------------------------------------------------------------
    static void wrap_anonymous_blocks(LayoutNode* node) {
        if (!node) return;

        // Сначала — рекурсивно вглубь
        for (auto& c : node->children) wrap_anonymous_blocks(c.get());

        if (node->type != BoxType::Block) return;

        bool has_inline = false, has_block = false;
        for (auto& c : node->children) {
            if (c->is_inline_level()) has_inline = true;
            else if (c->is_block_level()) has_block = true;
        }
        if (!has_inline || !has_block) return;  // смешивания нет — не трогаем

        std::vector<std::unique_ptr<LayoutNode>> out;
        out.reserve(node->children.size());

        std::unique_ptr<LayoutNode> run;
        for (auto& c : node->children) {
            if (c->is_inline_level()) {
                if (!run) {
                    run = std::make_unique<LayoutNode>();
                    run->type = BoxType::AnonymousBlock;
                    run->style_soa_idx = UINT32_MAX;
                    run->parent = node;
                }
                c->parent = run.get();
                run->children.push_back(std::move(c));
            }
            else {
                if (run) { 
                    out.push_back(std::move(run)); 
                    run.reset(); 
                }
                out.push_back(std::move(c));
            }
        }
        if (run) out.push_back(std::move(run));
        node->children = std::move(out);
    }

    // -----------------------------------------------------------------
    //  Phase 2: BFC layout
    // -----------------------------------------------------------------

    // Возвращает реальный style-index, поднимаясь по предкам от
    // AnonymousBlock/Text к ближайшему Block/Inline.
    static uint32_t effective_style_idx(const LayoutNode* node) {
        for (const LayoutNode* n = node; n; n = n->parent) {
            if (n->style_soa_idx != UINT32_MAX) return n->style_soa_idx;
        }
        return 0;
    }

    static void layout_node(LayoutNode* node, float parent_width,
        const Viewport& vp, 
        const StyleStorageSoA& storage, float root_fs)
    {
        switch (node->type) {
        case BoxType::Block:
        case BoxType::AnonymousBlock:
            layout_block_container(node, parent_width, vp, storage, root_fs);
            break;
        case BoxType::Inline:
            layout_inline(node, parent_width,vp,  storage, root_fs);
            break;
        case BoxType::Text:
            layout_text(node, parent_width, vp, storage, root_fs);
            break;
        }
    }

    // --- Length -> px ---
    static float resolve_px(const style::Length& l, float font_size,
        float root_fs, const Viewport& vp, float percentage_base)
    {
        switch (l.unit) {
        case Unit::Px:      return l.value;
        case Unit::Em:      return l.value * font_size;
        case Unit::Rem:     return l.value * /* root_fs */ 16.0f;
        case Unit::Percent: return l.value * 0.01f * percentage_base;
        case Unit::Vw:      return l.value * 0.01f * vp.width;
        case Unit::Vh:      return l.value * 0.01f * vp.height;
        case Unit::Vmin:    return l.value * 0.01f * std::min(vp.width, vp.height);
        case Unit::Vmax:    return l.value * 0.01f * std::max(vp.width, vp.height);
        case Unit::Pt:      return l.value * 96.0f / 72.0f;
        case Unit::Cm:      return l.value * 96.0f / 2.54f;
        case Unit::Mm:      return l.value * 96.0f / 25.4f;
        case Unit::In:      return l.value * 96.0f;
        case Unit::Auto:
        case Unit::None:    return 0.0f;
        }
        return 0.0f;
    }

    static void resolve_box_model(LayoutNode* node,
        const StyleStorageSoA& storage,
        const Viewport& vp,
        float parent_width, float root_fs)
    {
        const uint32_t si = effective_style_idx(node);
        const float fs = storage.font_sizes[si];
        auto& g = node->geometry;

        g.margin_top = resolve_px(storage.margin_top[si], fs, root_fs,vp, parent_width);
        g.margin_right = resolve_px(storage.margin_right[si], fs, root_fs, vp, parent_width);
        g.margin_bottom = resolve_px(storage.margin_bottom[si], fs, root_fs, vp, parent_width);
        g.margin_left = resolve_px(storage.margin_left[si], fs, root_fs, vp, parent_width);

        g.padding_top = resolve_px(storage.padding_top[si], fs, root_fs, vp, parent_width);
        g.padding_right = resolve_px(storage.padding_right[si], fs, root_fs, vp, parent_width);
        g.padding_bottom = resolve_px(storage.padding_bottom[si], fs, root_fs,vp, parent_width);
        g.padding_left = resolve_px(storage.padding_left[si], fs, root_fs, vp, parent_width);

        g.border_top = resolve_px(storage.border_top_width[si], fs, root_fs, vp, parent_width);
        g.border_right = resolve_px(storage.border_right_width[si], fs, root_fs, vp, parent_width);
        g.border_bottom = resolve_px(storage.border_bottom_width[si], fs, root_fs, vp, parent_width);
        g.border_left = resolve_px(storage.border_left_width[si], fs, root_fs, vp, parent_width);
    }

    // --- Block container: вертикальный стек детей ---
    static void layout_block_container(LayoutNode* node, float parent_width,
        const Viewport& vp,
        const StyleStorageSoA& storage, float root_fs)
    {
        auto& g = node->geometry;
        const bool is_real = (node->type == BoxType::Block);

        if (is_real) resolve_box_model(node, storage, vp, parent_width, root_fs);
        // У AnonymousBlock margin/padding/border остаются 0.

        const float h_extra = g.margin_left + g.margin_right
            + g.padding_left + g.padding_right
            + g.border_left + g.border_right;

        // content_width
        float content_width = std::max(0.0f, parent_width - h_extra);
        if (is_real) {
            const uint32_t si = node->style_soa_idx;
            const float fs = storage.font_sizes[si];
            const style::Length& w = storage.widths[si];
            if (!w.is_auto() && !w.is_none()) {
                float spec = resolve_px(w, fs, root_fs, vp, parent_width);
                if (storage.box_sizings[si] == BoxSizing::BorderBox) {
                    spec -= (g.padding_left + g.padding_right
                        + g.border_left + g.border_right);
                }
                content_width = std::max(0.0f, spec);
            }
        }

        // Layout детей (block flow — друг под другом)
        const float inner_x = g.border_left + g.padding_left;
        const float inner_y = g.border_top + g.padding_top;

        float y_cursor = 0.0f;
        for (auto& child : node->children) {
            layout_node(child.get(), content_width, vp, storage, root_fs);

            const float cmt = child->geometry.margin_top;
            const float cmb = child->geometry.margin_bottom;
            const float cml = child->geometry.margin_left;
            const float ch = child->geometry.height;

            child->geometry.x = inner_x + cml;
            child->geometry.y = inner_y + y_cursor + cmt;

            y_cursor += cmt + ch + cmb;
        }

        // content_height
 // content_height
        float content_height = y_cursor;
        if (is_real) {
            const uint32_t si = node->style_soa_idx;
            const float fs = storage.font_sizes[si];
            const style::Length& h = storage.heights[si];
            if (!h.is_auto() && !h.is_none()) {
                float spec = resolve_px(h, fs, root_fs, vp, parent_width);   // ← +vp
                if (storage.box_sizings[si] == BoxSizing::BorderBox) {
                    spec -= (g.padding_top + g.padding_bottom
                        + g.border_top + g.border_bottom);
                }
                content_height = std::max(0.0f, spec);
            }
        }

        // Теперь auto-margin:
        if (is_real) {
            const uint32_t si = node->style_soa_idx;
            const bool ml_auto = storage.margin_left[si].is_auto();
            const bool mr_auto = storage.margin_right[si].is_auto();
            if (ml_auto || mr_auto) {
                const float used = content_width
                    + g.border_left + g.padding_left + g.padding_right + g.border_right;
                float free_space = std::max(0.0f, parent_width - used);
                if (!ml_auto) free_space -= g.margin_left;
                if (!mr_auto) free_space -= g.margin_right;

                if (ml_auto && mr_auto) {
                    g.margin_left = free_space * 0.5f;
                    g.margin_right = free_space - g.margin_left;
                }
                else if (ml_auto) g.margin_left = free_space;
                else              g.margin_right = free_space;
            }
        }

        g.width = g.border_left + g.padding_left + content_width
            + g.padding_right + g.border_right;
        g.height = g.border_top + g.padding_top + content_height
            + g.padding_bottom + g.border_bottom;
    }

    // --- Inline: упрощённо — shrink-to-fit, дети в строку ---
    static void layout_inline(LayoutNode* node, float parent_width,
        const Viewport& vp,
        const StyleStorageSoA& storage, float root_fs)
    {
        resolve_box_model(node, storage,vp,  parent_width, root_fs);
        auto& g = node->geometry;

        const uint32_t si = node->style_soa_idx;
        const float fs = storage.font_sizes[si];

        const float h_extra = g.margin_left + g.margin_right
            + g.padding_left + g.padding_right
            + g.border_left + g.border_right;
        const float avail = std::max(0.0f, parent_width - h_extra);

        const float inner_x = g.border_left + g.padding_left;
        const float inner_y = g.border_top + g.padding_top;

        // Сначала разложить детей, чтобы узнать их ширины
        float cursor_x = 0.0f;
        float max_h = 0.0f;
        for (auto& child : node->children) {
            layout_node(child.get(), avail, vp, storage, root_fs);

            child->geometry.x = inner_x + cursor_x + child->geometry.margin_left;
            child->geometry.y = inner_y + child->geometry.margin_top;

            cursor_x += child->geometry.margin_left + child->geometry.width
                + child->geometry.margin_right;
            max_h = std::max(max_h,
                child->geometry.margin_top + child->geometry.height
                + child->geometry.margin_bottom);
        }

        // content_width: явная или shrink-to-fit
        float content_width;
        const style::Length& w = storage.widths[si];
        if (!w.is_auto() && !w.is_none()) {
            float spec = resolve_px(w, fs, root_fs, vp, parent_width);
            if (storage.box_sizings[si] == BoxSizing::BorderBox) {
                spec -= (g.padding_left + g.padding_right
                    + g.border_left + g.border_right);
            }
            content_width = std::max(0.0f, spec);
        }
        else {
            content_width = cursor_x;
        }

        g.width = g.border_left + g.padding_left + content_width
            + g.padding_right + g.border_right;
        g.height = g.border_top + g.padding_top + max_h
            + g.padding_bottom + g.border_bottom;
    }

    // --- Text: однострочный расчёт + грубая оценка переносов ---
    static void layout_text(LayoutNode* node, float parent_width,
        const Viewport& vp,
        const StyleStorageSoA& storage,
        float root_fs)
    {
        const uint32_t si = effective_style_idx(node);
        const float fs = storage.font_sizes[si];
        const float lh = style::resolve_line_height(storage, si);

        // Заглушка для метрик шрифта — 0.5 * font-size на символ.
        const float char_w = std::max(1.0f, fs * 0.5f);
        const float one_line_w = node->text_content.size() * char_w;

        if (parent_width > 0.0f && one_line_w > parent_width) {
            const float chars_per_line = std::max(1.0f, std::floor(parent_width / char_w));
            const float lines = std::ceil(node->text_content.size() / chars_per_line);
            node->geometry.width = parent_width;
            node->geometry.height = lines * lh;
        }
        else {
            node->geometry.width = one_line_w;
            node->geometry.height = lh;
        }
        // margin/padding/border остаются 0 (initial значения).
    }
};