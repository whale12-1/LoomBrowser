#pragma once

#include <memory>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <cctype>
#include <cmath>
#include <functional>
#include <string_view>
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
    //  Абстракция текстовых метрик.
    //  LayoutTreeBuilder не знает о Qt. Если нужно точное совпадение с
    //  растеризатором (QPainter), владелец layout'а передаёт колбэки,
    //  которые дергают QFontMetricsF. Если колбэки не заданы — работает
    //  грубая эвристика (0.55 * font_size на символ, line-height = 1.2*fs).
    // =================================================================
    struct TextMetrics {
        std::function<float(const std::string& text,
            float font_size,
            bool  bold)> measure_width;

        std::function<float(float font_size,
            bool  bold)> line_spacing;

        bool hasWidth()  const { return static_cast<bool>(measure_width); }
        bool hasHeight() const { return static_cast<bool>(line_spacing); }
    };

    // =================================================================
    //  Phase 1 — построение дерева боксов
    // =================================================================
    static std::unique_ptr<LayoutNode> build(const StyleStorageSoA& storage) {
        if (storage.size() == 0) return nullptr;

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
    //  Phase 2 — расчёт геометрии
    // =================================================================
    static void compute_layout(LayoutNode* root, const Viewport& viewport,
        const StyleStorageSoA& storage,
        const TextMetrics& metrics = {})
    {
        if (!root) return;
        const float root_fs = storage.font_sizes.empty()
            ? 16.0f : storage.font_sizes[0];
        layout_node(root, viewport.width, viewport, storage, root_fs, metrics);
    }

private:
    // =================================================================
    //  Phase 1
    // =================================================================

    static std::unique_ptr<LayoutNode> build_node(
        uint32_t soa_idx,
        const StyleStorageSoA& storage,
        const std::unordered_map<const DOMNode*, uint32_t>& dom_to_soa)
    {
        if (storage.displays[soa_idx] == style::Display::None) return nullptr;

        const DOMNode* dom = storage.dom_nodes[soa_idx];
        if (!dom) return nullptr;

        auto node = std::make_unique<LayoutNode>();
        node->style_soa_idx = soa_idx;
        node->type = classify_box(storage.displays[soa_idx]);

        for (const DOMNode* child_dom : dom->children) {
            if (!child_dom) continue;

            if (child_dom->type == NodeType::Element &&
                isNonRendered(child_dom->tag_name)) continue;

            if (child_dom->type == NodeType::Text) {
                if (!has_visible_text(child_dom->text_content)) continue; 
                split_text_into_words(child_dom->text_content, node.get());
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

    // Разбивает текстовую ноду на атомарные Text-узлы: слова и одиночные пробелы.
    // Подряд идущие пробелы схлопываются в один (white-space: normal).
    //
    //   "hello world"  →  [Text "hello"] [Text " "] [Text "world"]
    //   "a   b"        →  [Text "a"]     [Text " "] [Text "b"]
    //
    // IFC родитель потом укладывает эти атомы по строкам.
    static void split_text_into_words(std::string_view text, LayoutNode* parent) {
        size_t i = 0, n = text.size();
        while (i < n) {
            // Проглатываем пробелы — создаём один узел пробела
            size_t ws = i;
            while (i < n && std::isspace((unsigned char)text[i])) ++i;
            if (i > ws) {
                auto sp = std::make_unique<LayoutNode>();
                sp->type = BoxType::Text;
                sp->style_soa_idx = UINT32_MAX;
                sp->text_content = " ";
                parent->add_child(std::move(sp));
            }

            // Слово — до следующего пробела
            size_t ws2 = i;
            while (i < n && !std::isspace((unsigned char)text[i])) ++i;
            if (i > ws2) {
                auto word = std::make_unique<LayoutNode>();
                word->type = BoxType::Text;
                word->style_soa_idx = UINT32_MAX;
                word->text_content.assign(text.data() + ws2, i - ws2);
                parent->add_child(std::move(word));
            }
        }
    }

    static bool has_visible_text(std::string_view s) {
        for (char c : s) if (!std::isspace((unsigned char)c)) return true;
        return false;
    }

    static bool isNonRendered(std::string_view tag) {
        return tag == "head" || tag == "style" || tag == "script" ||
            tag == "title" || tag == "meta" || tag == "link" ||
            tag == "base" || tag == "noscript" || tag == "template";
    }

    static BoxType classify_box(style::Display d) {
        switch (d) {
        case style::Display::Block:
        case style::Display::Flex:
        case style::Display::Grid:
        case style::Display::Table:
        case style::Display::TableRow:
        case style::Display::TableCell:
            return BoxType::Block;

        case style::Display::Inline:
        case style::Display::InlineBlock:
        case style::Display::InlineFlex:
        case style::Display::InlineGrid:
        case style::Display::Contents:
            return BoxType::Inline;

        case style::Display::None:
            break;
        }
        return BoxType::Block;
    }

    // Все подряд идущие inline-дети оборачиваются в AnonymousBlock.
    // Обёртка создаётся, даже если block-детей нет вовсе — иначе
    // текст в блоке не получит inline formatting context и уложится
    // вертикально по одной ноде на строку.
    static void wrap_anonymous_blocks(LayoutNode* node) {
        if (!node) return;

        for (auto& c : node->children) wrap_anonymous_blocks(c.get());

        if (node->type != BoxType::Block) return;

        bool has_inline = false;
        for (auto& c : node->children) {
            if (c->is_inline_level()) { has_inline = true; break; }
        }
        if (!has_inline) return;

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
                if (run) { out.push_back(std::move(run)); run.reset(); }
                out.push_back(std::move(c));
            }
        }
        if (run) out.push_back(std::move(run));
        node->children = std::move(out);
    }

    // =================================================================
    //  Phase 2 — диспетчер
    // =================================================================

    static uint32_t effective_style_idx(const LayoutNode* node) {
        for (const LayoutNode* n = node; n; n = n->parent) {
            if (n->style_soa_idx != UINT32_MAX) return n->style_soa_idx;
        }
        return 0;
    }

    static void layout_node(LayoutNode* node, float parent_width,
        const Viewport& vp,
        const StyleStorageSoA& storage,
        float root_fs,
        const TextMetrics& tm)
    {
        switch (node->type) {
        case BoxType::AnonymousBlock:
            layout_ifc(node, parent_width, vp, storage, root_fs, tm);
            node->geometry.width = parent_width;
            break;
        case BoxType::Block:
            layout_block_container(node, parent_width, vp, storage, root_fs, tm);
            break;
        case BoxType::Inline:
            layout_inline(node, parent_width, vp, storage, root_fs, tm);
            break;
        case BoxType::Text:
            layout_text(node, parent_width, vp, storage, root_fs, tm);
            break;
        }
    }

    // =================================================================
    //  Утилиты единиц
    // =================================================================

    static float resolve_px(const style::Length& l, float font_size,
        float root_fs, const Viewport& vp, float percentage_base)
    {
        switch (l.unit) {
        case Unit::Px:      return l.value;
        case Unit::Em:      return l.value * font_size;
        case Unit::Rem:     return l.value * root_fs;
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

    static float collapse_margins(float a, float b) {
        if (a >= 0.0f && b >= 0.0f) return std::max(a, b);
        if (a <= 0.0f && b <= 0.0f) return std::min(a, b);
        return a + b;
    }


    static void resolve_box_model(LayoutNode* node,
        const StyleStorageSoA& storage,
        const Viewport& vp,
        float parent_width, float root_fs)
    {
        const uint32_t si = effective_style_idx(node);
        const float fs = storage.font_sizes[si];
        auto& g = node->geometry;

        g.margin_top = resolve_px(storage.margin_top[si], fs, root_fs, vp, parent_width);
        g.margin_right = resolve_px(storage.margin_right[si], fs, root_fs, vp, parent_width);
        g.margin_bottom = resolve_px(storage.margin_bottom[si], fs, root_fs, vp, parent_width);
        g.margin_left = resolve_px(storage.margin_left[si], fs, root_fs, vp, parent_width);

        g.padding_top = resolve_px(storage.padding_top[si], fs, root_fs, vp, parent_width);
        g.padding_right = resolve_px(storage.padding_right[si], fs, root_fs, vp, parent_width);
        g.padding_bottom = resolve_px(storage.padding_bottom[si], fs, root_fs, vp, parent_width);
        g.padding_left = resolve_px(storage.padding_left[si], fs, root_fs, vp, parent_width);

        g.border_top = resolve_px(storage.border_top_width[si], fs, root_fs, vp, parent_width);
        g.border_right = resolve_px(storage.border_right_width[si], fs, root_fs, vp, parent_width);
        g.border_bottom = resolve_px(storage.border_bottom_width[si], fs, root_fs, vp, parent_width);
        g.border_left = resolve_px(storage.border_left_width[si], fs, root_fs, vp, parent_width);
    }

    // =================================================================
    //  Inline Formatting Context
    //
    //  Раскладывает inline-level детей по строкам слева направо с переносом.
    //  Используется:
    //    * для AnonymousBlock — без box model (padding/border = 0);
    //    * для <span> (BoxType::Inline) — с box model, после resolve_box_model.
    //
    //  Дети позиционируются относительно content-области (внутри padding/border).
    //  В конце записывает intrinsic size в node->geometry.width/height.
    //
    //  Переносы:
    //    * Пробел в начале строки схлопывается (width не учитывается).
    //    * Если слово не влезает — переносится на новую строку целиком.
    //    * Одиночное слово, которое шире avail, вылезет за границы (по CSS).
    // =================================================================
    static void layout_ifc(LayoutNode* node, float parent_width,
        const Viewport& vp,
        const StyleStorageSoA& storage,
        float root_fs,
        const TextMetrics& tm)
    {
        auto& g = node->geometry;

        const float inner_x = g.border_left + g.padding_left;
        const float inner_y = g.border_top + g.padding_top;

        const float h_extra = g.margin_left + g.margin_right
            + g.padding_left + g.padding_right
            + g.border_left + g.border_right;
        const float avail = std::max(0.0f, parent_width - h_extra);

        float line_x = 0;
        float line_y = 0;
        float line_height = 0;
        float max_line_width = 0;

        for (auto& child : node->children) {
            layout_node(child.get(), avail, vp, storage, root_fs, tm);

            const float w = child->geometry.width
                + child->geometry.margin_left
                + child->geometry.margin_right;
            const float h = child->geometry.height
                + child->geometry.margin_top
                + child->geometry.margin_bottom;

            const bool is_space = (child->type == BoxType::Text
                && child->text_content == " ");

            // Пробел в начале строки — схлопываем: ставим в текущую
            // позицию, но не увеличиваем курсор.
            if (is_space && line_x == 0) {
                child->geometry.x = inner_x;
                child->geometry.y = inner_y + line_y;
                continue;
            }

            // Не влезает — перенос на новую строку.
            if (line_x > 0 && line_x + w > avail) {
                max_line_width = std::max(max_line_width, line_x);
                line_y += line_height;
                line_x = 0;
                line_height = 0;

                // Если это пробел — на новой строке его тоже схлопываем.
                if (is_space) {
                    child->geometry.x = inner_x;
                    child->geometry.y = inner_y + line_y;
                    continue;
                }
            }

            child->geometry.x = inner_x + line_x + child->geometry.margin_left;
            child->geometry.y = inner_y + line_y + child->geometry.margin_top;

            line_x += w;
            line_height = std::max(line_height, h);
        }

        // Закрываем последнюю строку.
        max_line_width = std::max(max_line_width, line_x);
        line_y += line_height;

        g.width = g.border_left + g.padding_left + max_line_width
            + g.padding_right + g.border_right;
        g.height = g.border_top + g.padding_top + line_y
            + g.padding_bottom + g.border_bottom;
    }

    // =================================================================
    //  Block container — обычный BFC layout
    // =================================================================
    static void layout_block_container(LayoutNode* node, float parent_width,
        const Viewport& vp,
        const StyleStorageSoA& storage,
        float root_fs,
        const TextMetrics& tm)
    {
        auto& g = node->geometry;
        const bool is_real = (node->type == BoxType::Block);

        if (is_real) resolve_box_model(node, storage, vp, parent_width, root_fs);

        const float h_extra = g.margin_left + g.margin_right
            + g.padding_left + g.padding_right
            + g.border_left + g.border_right;

        // -------- content_width --------
        float content_width = std::max(0.0f, parent_width - h_extra);

        if (is_real) {
            const uint32_t si = node->style_soa_idx;
            const float fs = storage.font_sizes[si];
            const bool border_box = (storage.box_sizings[si] == BoxSizing::BorderBox);
            const float pb_h = g.padding_left + g.padding_right
                + g.border_left + g.border_right;

            const style::Length& w = storage.widths[si];
            if (!w.is_auto() && !w.is_none()) {
                float spec = resolve_px(w, fs, root_fs, vp, parent_width);
                if (border_box) spec -= pb_h;
                content_width = std::max(0.0f, spec);
            }

            const style::Length& maxw = storage.max_widths[si];
            if (!maxw.is_none()) {
                float mw = resolve_px(maxw, fs, root_fs, vp, parent_width);
                if (border_box) mw -= pb_h;
                if (mw > 0.0f) content_width = std::min(content_width, mw);
            }

            const style::Length& minw = storage.min_widths[si];
            if (!minw.is_none()) {
                float mw = resolve_px(minw, fs, root_fs, vp, parent_width);
                if (border_box) mw -= pb_h;
                if (mw > 0.0f) content_width = std::max(content_width, mw);
            }
        }

        // -------- auto-margin (горизонтальный) --------
        if (is_real) {
            const uint32_t si = node->style_soa_idx;
            const bool ml_auto = storage.margin_left[si].is_auto();
            const bool mr_auto = storage.margin_right[si].is_auto();

            if (ml_auto || mr_auto) {
                const float used = content_width
                    + g.border_left + g.padding_left
                    + g.padding_right + g.border_right;

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

        // -------- дети --------
        const float inner_x = g.border_left + g.padding_left;
        const float inner_y = g.border_top + g.padding_top;

        // ------------------------------------------------------------
        //  Укладка блочных детей с margin collapsing (§ 8.3.1).
        //
        //  Между соседними детьми зазор = collapse(prev.margin_bottom, cur.margin_top).
        //  У первого ребёнка сверху соседа нет — берётся его собственный margin_top.
        //  После последнего ребёнка остаётся его margin_bottom (учитывается
        //  в content_height — как это было и раньше).
        //
        //  Ограничение: пока НЕ реализовано parent-child collapsing и
        //  схлопывание через пустые блоки.
        // ------------------------------------------------------------
        float y_cursor = 0.0f;
        float prev_bottom = 0.0f;
        bool  first_child = true;

        for (auto& child : node->children) {
            layout_node(child.get(), content_width, vp, storage, root_fs, tm);

            const float cmt = child->geometry.margin_top;
            const float cmb = child->geometry.margin_bottom;
            const float cml = child->geometry.margin_left;
            const float ch = child->geometry.height;

            const float gap = first_child
                ? cmt
                : collapse_margins(prev_bottom, cmt);

            child->geometry.x = inner_x + cml;
            child->geometry.y = inner_y + y_cursor + gap;

            y_cursor += gap + ch;
            prev_bottom = cmb;
            first_child = false;
        }

        // Нижний margin последнего ребёнка всё ещё входит в высоту контейнера.
        y_cursor += prev_bottom;

        // -------- content_height --------
        float content_height = y_cursor;

        if (is_real) {
            const uint32_t si = node->style_soa_idx;
            const float fs = storage.font_sizes[si];
            const bool border_box = (storage.box_sizings[si] == BoxSizing::BorderBox);
            const float pb_v = g.padding_top + g.padding_bottom
                + g.border_top + g.border_bottom;

            const style::Length& h = storage.heights[si];
            if (!h.is_auto() && !h.is_none()) {
                float spec = resolve_px(h, fs, root_fs, vp, parent_width);
                if (border_box) spec -= pb_v;
                content_height = std::max(0.0f, spec);
            }

            const style::Length& maxh = storage.max_heights[si];
            if (!maxh.is_none()) {
                float mh = resolve_px(maxh, fs, root_fs, vp, parent_width);
                if (border_box) mh -= pb_v;
                if (mh > 0.0f) content_height = std::min(content_height, mh);
            }

            const style::Length& minh = storage.min_heights[si];
            if (!minh.is_none()) {
                float mh = resolve_px(minh, fs, root_fs, vp, parent_width);
                if (border_box) mh -= pb_v;
                if (mh > 0.0f) content_height = std::max(content_height, mh);
            }
        }

        g.width = g.border_left + g.padding_left + content_width
            + g.padding_right + g.border_right;
        g.height = g.border_top + g.padding_top + content_height
            + g.padding_bottom + g.border_bottom;
    }

    // =================================================================
    //  Inline element (<span>)
    //
    //  1. Считаем box model (margin/padding/border).
    //  2. layout_ifc укладывает детей внутри content-области и ставит
    //     intrinsic width/height.
    //  3. Явный width/height и min/max переопределяют intrinsic.
    //
    //  TODO: если задан явный width, дети уже уложены по intrinsic.
    //        Принципиально надо двухпроходный layout. Пока не делаем —
    //        дети могут вылезти за пределы <span>.
    // =================================================================
    static void layout_inline(LayoutNode* node, float parent_width,
        const Viewport& vp,
        const StyleStorageSoA& storage,
        float root_fs,
        const TextMetrics& tm)
    {
        resolve_box_model(node, storage, vp, parent_width, root_fs);

        // Вложенный IFC: укладывает детей и ставит node->geometry.width/height
        // как intrinsic size содержимого (с учётом padding/border).
        layout_ifc(node, parent_width, vp, storage, root_fs, tm);

        auto& g = node->geometry;
        const uint32_t si = node->style_soa_idx;
        const float fs = storage.font_sizes[si];
        const bool border_box = (storage.box_sizings[si] == BoxSizing::BorderBox);
        const float pb_h = g.padding_left + g.padding_right
            + g.border_left + g.border_right;
        const float pb_v = g.padding_top + g.padding_bottom
            + g.border_top + g.border_bottom;

        // Явный width
        const style::Length& w = storage.widths[si];
        if (!w.is_auto() && !w.is_none()) {
            float spec = resolve_px(w, fs, root_fs, vp, parent_width);
            if (border_box) spec -= pb_h;
            g.width = pb_h + std::max(0.0f, spec);
        }

        // Явный height
        const style::Length& h = storage.heights[si];
        if (!h.is_auto() && !h.is_none()) {
            float spec = resolve_px(h, fs, root_fs, vp, parent_width);
            if (border_box) spec -= pb_v;
            g.height = pb_v + std::max(0.0f, spec);
        }

        // max-width
        const style::Length& maxw = storage.max_widths[si];
        if (!maxw.is_none()) {
            float mw = resolve_px(maxw, fs, root_fs, vp, parent_width);
            if (border_box) mw -= pb_h;
            if (mw > 0.0f) {
                const float content_w = std::min(g.width - pb_h, mw);
                g.width = pb_h + std::max(0.0f, content_w);
            }
        }

        // min-width
        const style::Length& minw = storage.min_widths[si];
        if (!minw.is_none()) {
            float mw = resolve_px(minw, fs, root_fs, vp, parent_width);
            if (border_box) mw -= pb_h;
            if (mw > 0.0f) {
                const float content_w = std::max(g.width - pb_h, mw);
                g.width = pb_h + content_w;
            }
        }

        // max-height / min-height — аналогично, если понадобится.
    }

    // =================================================================
    //  Text — атомарный узел: одно слово или один пробел.
    //  Ширина — ширина этого текста при текущем font-size/weight.
    //  Высота — line-height одной строки.
    //  Переносы по строкам делает родительский IFC, а не сам текст.
    // =================================================================
    static void layout_text(LayoutNode* node, float /*parent_width*/,
        const Viewport& /*vp*/,
        const StyleStorageSoA& storage,
        float /*root_fs*/,
        const TextMetrics& tm)
    {
        const uint32_t si = effective_style_idx(node);
        const float fs = storage.font_sizes[si];
        const bool bold = storage.font_weights[si] >= 700;

        float lh;
        const float raw_lh = storage.line_heights[si];
        if (raw_lh == 0.0f && tm.hasHeight()) {
            lh = tm.line_spacing(fs, bold);
        }
        else {
            lh = style::resolve_line_height(storage, si);
        }

        float text_w;
        if (tm.hasWidth()) {
            text_w = tm.measure_width(node->text_content, fs, bold);
        }
        else {
            text_w = node->text_content.size() * std::max(1.0f, fs * 0.55f);
        }

        node->geometry.width = text_w;
        node->geometry.height = lh;
    }
};