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
        build_absolute_index(root.get(), storage);
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
        const float root_fs = storage.font_sizes.empty() ? 16.0f : storage.font_sizes[0];

        // 1. Flow-раскладка. Absolute-дети пропускаются, их static position
        //    сохраняется в layout_block_container.
        layout_node(root, viewport.width, viewport, storage, root_fs, metrics);

        // 2. Пост-проход: сохранить flow_x/flow_y для не-absolute узлов,
        //    применить relative-сдвиг. Absolute-узлы не трогаем — их
        //    flow-координаты уже сохранены в родителе.
        apply_position_offsets(root, viewport, storage, root_fs);

        // 3. Раскладка absolute-потомков относительно их содержащего блока.
        layout_absolute_descendants(root, viewport, storage, root_fs, metrics);
    }

private:
    // =================================================================
    //  Phase 1 — построение
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

        // CSS 2.1 §9.7: position: absolute принудительно blockifies
        // inline / inline-block в block-level.
        if (storage.positions[soa_idx] == style::Position::Absolute) {
            if (node->type == BoxType::Inline) {
                node->type = BoxType::Block;
            }
        }

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
    //  Phase 1: индекс containing blocks для absolute
    //
    //  Для каждого absolute-узла находим ближайшего предка с position
    //  relative или absolute и регистрируем узел в его absolute_children.
    //  Если positioned-предка нет — CB становится root (fallback viewport).
    // =================================================================

    static void build_absolute_index(LayoutNode* root,
        const StyleStorageSoA& storage) {
        if (!root) return;
        collect_absolute(root, root, storage);
    }

    static void collect_absolute(LayoutNode* node,
        LayoutNode* current_cb,
        const StyleStorageSoA& storage) {
        if (!node) return;

        LayoutNode* cb_for_children = current_cb;
        if (node->style_soa_idx != UINT32_MAX) {
            const auto position = storage.positions[node->style_soa_idx];
            if (position == style::Position::Relative
                || position == style::Position::Absolute) {
                cb_for_children = node;
            }

            if (position == style::Position::Absolute) {
                current_cb->absolute_children.push_back(node);
            }
        }

        for (auto& child : node->children) {
            collect_absolute(child.get(), cb_for_children, storage);
        }
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

    // Определяет, является ли узел flex-контейнером по его стилю.
    static bool is_flex_container(const LayoutNode* node,
        const StyleStorageSoA& storage)
    {
        if (!node || node->style_soa_idx == UINT32_MAX) return false;
        return storage.displays[node->style_soa_idx] == style::Display::Flex;
    }

    static void layout_flex_container(LayoutNode* node,
        float parent_width,
        const Viewport& vp,
        const StyleStorageSoA& storage,
        float root_fs,
        const TextMetrics& tm)
    {
        // TODO(A2): заменить на реальный flex algorithm.
        // Пока — fallback на block layout, чтобы display: flex
        // не ломал существующие тесты и другие компоненты.
        layout_block_container(node, parent_width, vp, storage, root_fs, tm);
    }
    // Flex-контейнер — новый путь в диспетчере.
    // Не добавляем BoxType::Flex: flex — это режим layout,
    // а не тип бокса. Внешне контейнер остаётся Block.
    static void layout_node(LayoutNode* node, float parent_width,
        const Viewport& vp,
        const StyleStorageSoA& storage,
        float root_fs,
        const TextMetrics& tm)
    {
        if (is_flex_container(node, storage)) {
            layout_flex_container(node, parent_width, vp, storage, root_fs, tm);
            return;
        }

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

    // =================================================================
    //  Relative positioning (пост-обработка flow-координат)
    // =================================================================

    static void layout_relative(LayoutNode* node,
        const Viewport& vp,
        const StyleStorageSoA& storage,
        float root_fs)
    {
        const uint32_t si = node->style_soa_idx;
        if (storage.positions[si] != style::Position::Relative) return;

        const float fs = storage.font_sizes[si];
        const style::Length& top_inset = storage.top_insets[si];
        const style::Length& left_inset = storage.left_insets[si];
        const style::Length& bottom_inset = storage.bottom_insets[si];
        const style::Length& right_inset = storage.right_insets[si];

        const float top = resolve_px(
            top_inset, fs, root_fs, vp, node->geometry.width);
        const float left = resolve_px(
            left_inset, fs, root_fs, vp, node->geometry.width);
        const float bottom = resolve_px(
            bottom_inset, fs, root_fs, vp, node->geometry.height);
        const float right = resolve_px(
            right_inset, fs, root_fs, vp, node->geometry.width);

        // Если оба inset на оси заданы — первая версия использует left/top.
        const float offset_x = !left_inset.is_auto() ? left
            : (!right_inset.is_auto() ? -right : 0.0f);
        const float offset_y = !top_inset.is_auto() ? top
            : (!bottom_inset.is_auto() ? -bottom : 0.0f);

        node->geometry.x += offset_x;
        node->geometry.y += offset_y;
    }

    // =================================================================
    //  Пост-проход: сохранить flow-позицию и применить relative
    //
    //  Для absolute-узлов flow_x/flow_y УЖЕ сохранены родителем
    //  в layout_block_container. Не перезаписываем их здесь —
    //  их geometry.x/y пока не посчитаны.
    // =================================================================

    static void apply_position_offsets(LayoutNode* node, const Viewport& vp,
        const StyleStorageSoA& storage, float root_fs)
    {
        if (!node) return;

        const bool is_absolute =
            node->style_soa_idx != UINT32_MAX &&
            storage.positions[node->style_soa_idx] == style::Position::Absolute;

        if (!is_absolute) {
            node->geometry.flow_x = node->geometry.x;
            node->geometry.flow_y = node->geometry.y;
        }

        if (node->type == BoxType::Block && node->style_soa_idx != UINT32_MAX
            && storage.positions[node->style_soa_idx] == style::Position::Relative) {
            layout_relative(node, vp, storage, root_fs);
        }

        for (auto& c : node->children) {
            apply_position_offsets(c.get(), vp, storage, root_fs);
        }
    }

    // =================================================================
    //  Absolute positioning
    // =================================================================

    // Рекурсивный обход: для каждого узла разложить его absolute_children
    // относительно его content-бокса, затем спуститься к детям.
    static void layout_absolute_descendants(LayoutNode* cb, const Viewport& vp,
        const StyleStorageSoA& storage, float root_fs, const TextMetrics& tm)
    {
        if (!cb) return;

        const float cb_content_width = cb->geometry.width
            - cb->geometry.border_left - cb->geometry.padding_left
            - cb->geometry.padding_right - cb->geometry.border_right;
        const float cb_content_height = cb->geometry.height
            - cb->geometry.border_top - cb->geometry.padding_top
            - cb->geometry.padding_bottom - cb->geometry.border_bottom;

        for (LayoutNode* child : cb->absolute_children) {
            if (!child || child->style_soa_idx == UINT32_MAX) continue;
            layout_absolute(child, cb_content_width, cb_content_height,
                vp, storage, root_fs, tm);
            // У absolute-узла могут быть свои absolute-потомки.
            layout_absolute_descendants(child, vp, storage, root_fs, tm);
        }

        for (auto& child : cb->children) {
            layout_absolute_descendants(child.get(), vp, storage, root_fs, tm);
        }
    }

    // Раскладка одного absolute-узла относительно content-бокса его CB.
    static void layout_absolute(LayoutNode* node, float cb_content_width,
        float cb_content_height, const Viewport& vp,
        const StyleStorageSoA& storage, float root_fs, const TextMetrics& tm)
    {
        const uint32_t si = node->style_soa_idx;
        if (si == UINT32_MAX) return;
        if (storage.positions[si] != style::Position::Absolute) return;

        const float fs = storage.font_sizes[si];
        const style::Length& left_inset = storage.left_insets[si];
        const style::Length& right_inset = storage.right_insets[si];
        const style::Length& top_inset = storage.top_insets[si];
        const style::Length& bottom_inset = storage.bottom_insets[si];

        const bool has_left = !left_inset.is_auto();
        const bool has_right = !right_inset.is_auto();
        const bool has_top = !top_inset.is_auto();
        const bool has_bottom = !bottom_inset.is_auto();

        const float left = has_left
            ? resolve_px(left_inset, fs, root_fs, vp, cb_content_width) : 0.0f;
        const float right = has_right
            ? resolve_px(right_inset, fs, root_fs, vp, cb_content_width) : 0.0f;
        const float top = (has_top && cb_content_height >= 0.0f)
            ? resolve_px(top_inset, fs, root_fs, vp, cb_content_height) : 0.0f;
        const float bottom = (has_bottom && cb_content_height >= 0.0f)
            ? resolve_px(bottom_inset, fs, root_fs, vp, cb_content_height) : 0.0f;

        const style::Length& width_spec = storage.widths[si];
        const style::Length& height_spec = storage.heights[si];
        const bool width_auto = width_spec.is_auto() || width_spec.is_none();
        const bool height_auto = height_spec.is_auto() || height_spec.is_none();

        // -------- width (упрощённая матрица CSS 2.1 §10.3.7) --------
        float computed_width;
        if (!width_auto) {
            computed_width = resolve_px(width_spec, fs, root_fs, vp, cb_content_width);
        }
        else if (has_left && has_right) {
            computed_width = std::max(0.0f, cb_content_width - left - right);
        }
        else if (has_left) {
            computed_width = std::max(0.0f, cb_content_width - left);
        }
        else if (has_right) {
            computed_width = std::max(0.0f, cb_content_width - right);
        }
        else {
            computed_width = cb_content_width;
        }

        // -------- height --------
        float computed_height = node->geometry.height;   // пока 0, если не auto
        if (!height_auto) {
            computed_height = resolve_px(height_spec, fs, root_fs, vp, cb_content_width);
        }
        else if (has_top && has_bottom && cb_content_height >= 0.0f) {
            computed_height = std::max(0.0f, cb_content_height - top - bottom);
        }
        else if (has_top && cb_content_height >= 0.0f) {
            computed_height = std::max(0.0f, cb_content_height - top);
        }
        else if (has_bottom && cb_content_height >= 0.0f) {
            computed_height = std::max(0.0f, cb_content_height - bottom);
        }
        // Если height auto и нет top/bottom, оставляем как посчитает
        // layout_block_container по содержимому.

        // -------- x --------
        float x;
        if (has_left) {
            x = left;
        }
        else if (has_right && !width_auto) {
            x = cb_content_width - right - computed_width;
        }
        else {
            x = node->geometry.flow_x;
        }

        // -------- y --------
        float y;
        if (has_top) {
            y = top;
        }
        else if (has_bottom && !height_auto && cb_content_height >= 0.0f) {
            y = cb_content_height - bottom - computed_height;
        }
        else {
            y = node->geometry.flow_y;
        }

        // Разложить содержимое absolute-узла как обычный блок.
        layout_block_container(node, computed_width, vp, storage, root_fs, tm);

        // Установить итоговые координаты и размеры.
        node->geometry.x = x;
        node->geometry.y = y;
        node->geometry.width = computed_width;
        if (!height_auto) {
            node->geometry.height = computed_height;
        }
    }

    // =================================================================
    //  Box model
    // =================================================================

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
        //  Схлопывание вертикальных margin между соседними детьми и через
        //  границы родителя (CSS 2.1 §8.3.1).
        //  Parent-child collapse переносит margin потомка на родителя,
        //  поэтому сам потомок позиционируется без этого margin.
        // ------------------------------------------------------------
        const bool has_children = !node->children.empty();
        bool collapse_top_with_child = false;
        bool collapse_bottom_with_child = false;
        const bool top_edge_open = g.border_top == 0.0f && g.padding_top == 0.0f;
        const bool bottom_edge_open = g.border_bottom == 0.0f && g.padding_bottom == 0.0f;
        bool auto_height = !is_real;
        bool zero_min_height = true;

        if (is_real) {
            const uint32_t si = node->style_soa_idx;
            auto_height = storage.heights[si].is_auto() || storage.heights[si].is_none();
            const style::Length& min_h = storage.min_heights[si];
            zero_min_height = min_h.value == 0.0f || min_h.is_none();
        }

        const bool is_root = (node->parent == nullptr);

        if (!is_root && has_children && node->children.front()->is_block_level()
            && top_edge_open)
            collapse_top_with_child = true;

        if (!is_root && has_children && node->children.back()->is_block_level()
            && bottom_edge_open && auto_height && zero_min_height)
            collapse_bottom_with_child = true;

        float y_cursor = 0.0f;
        float prev_bottom = 0.0f;
        bool first_child = true;

        for (auto& child : node->children) {
            // Absolute-дети вырываются из потока:
            // - не вызываем layout_node
            // - не увеличиваем y_cursor
            // - сохраняем static position относительно родителя
            if (child->style_soa_idx != UINT32_MAX
                && storage.positions[child->style_soa_idx] == style::Position::Absolute) {
                child->geometry.flow_x = inner_x + child->geometry.margin_left;
                child->geometry.flow_y = inner_y + y_cursor;
                continue;
            }

            layout_node(child.get(), content_width, vp, storage, root_fs, tm);

            const float cmt = child->geometry.margin_top;
            const float cmb = child->geometry.margin_bottom;
            const float cml = child->geometry.margin_left;
            const float ch = child->geometry.height;
            const bool collapse_this_top = first_child && collapse_top_with_child;

            const float gap = first_child
                ? (collapse_this_top ? 0.0f : cmt)
                : collapse_margins(prev_bottom, cmt);

            child->geometry.x = inner_x + cml;
            child->geometry.y = inner_y + y_cursor + gap;

            y_cursor += gap + ch;
            prev_bottom = cmb;
            first_child = false;
        }

        if (collapse_top_with_child)
            g.margin_top = collapse_margins(g.margin_top, node->children.front()->geometry.margin_top);

        // При схлопывании с последним ребёнком его нижний margin выходит
        // за content box и объединяется с margin родителя.
        if (collapse_bottom_with_child) {
            g.margin_bottom = collapse_margins(g.margin_bottom, prev_bottom);
        }
        else {
            y_cursor += prev_bottom;
        }

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