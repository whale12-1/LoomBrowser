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
#include "float_context.h"
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
            const std::string& font_family,
            bool  bold,
            bool  italic)> measure_width;

        std::function<float(float font_size,
            const std::string& font_family,
            bool  bold)> line_spacing;

        std::function<float(float font_size, const std::string& font_family, bool bold)> ascent;
        std::function<float(float font_size, const std::string& font_family, bool bold)> descent;

        bool hasWidth()    const { return static_cast<bool>(measure_width); }
        bool hasHeight()   const { return static_cast<bool>(line_spacing); }
        bool hasBaseline() const {
            return static_cast<bool>(ascent) && static_cast<bool>(descent);
        }
    };

    // =================================================================
//  Абстракция intrinsic size для картинок.
//  Layout не знает про Qt. Владелец передаёт callback, который
//  резолвит путь и читает размеры файла. Если callback не задан —
//  intrinsic size = 0, и картинка получает 0×0.
// =================================================================
    struct ImageMetrics {
        std::function<std::pair<float, float>(const std::string& src)>
            intrinsic_size;

        bool hasIntrinsic() const {
            return static_cast<bool>(intrinsic_size);
        }
    };
    // =================================================================
    //  Phase 1 — построение дерева боксов
    // =================================================================
    static std::unique_ptr<LayoutNode> build(const StyleStorageSoA& storage,
        const ImageMetrics& img_metrics = {})
    {
        if (storage.size() == 0) return nullptr;

        std::unordered_map<const DOMNode*, uint32_t> dom_to_soa;
        dom_to_soa.reserve(storage.size() * 2);
        for (uint32_t i = 0; i < storage.size(); ++i) {
            const DOMNode* d = storage.dom_nodes[i];
            if (d) dom_to_soa[d] = i;
        }

        auto root = build_node(0, storage, dom_to_soa, img_metrics);
        if (!root) return nullptr;

        wrap_anonymous_blocks(root.get(), storage);
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

        FloatContext root_fc;
        layout_node(root, 0.0f, 0.0f, viewport.width,
            viewport, storage, root_fs, metrics, &root_fc);

        fixup_block_positions(root, storage);
        apply_position_offsets(root, viewport, storage, root_fs);
        layout_absolute_descendants(root, viewport, storage, root_fs, metrics);
    }

private:
    struct IntrinsicSize {
        float content_width = 0.0f;
        float content_height = 0.0f;
    };

    // Сдвигает весь поддерево на (dx, dy). Используется там, где
// положение становится известно только после полной раскладки
// (flex и float).
    static void shift_subtree(LayoutNode* node, float dx, float dy) {
        if (!node) return;
        node->geometry.x += dx;
        node->geometry.y += dy;
        node->geometry.flow_x += dx;
        node->geometry.flow_y += dy;
        for (auto& c : node->children) shift_subtree(c.get(), dx, dy);
    }

    // Измеряет max-content размер узла без изменения его geometry.
    static IntrinsicSize measure_intrinsic(const LayoutNode* node,
        const StyleStorageSoA& storage,
        float root_fs,
        const TextMetrics& tm)
    {
        if (!node) return {};

        const uint32_t si = effective_style_idx(node);
        const float fs = storage.font_sizes[si];
        const std::string& family = storage.font_families[si];
        const bool bold = storage.font_weights[si] >= 700;
        const auto fstyle = storage.font_styles[si];
        const bool italic = (fstyle == style::FontStyle::Italic
            || fstyle == style::FontStyle::Oblique);
        const Viewport vp{};
        const auto line_height = [&]() {
            if (storage.line_heights[si] == 0.0f && tm.hasHeight())
                return tm.line_spacing(fs, family, bold);
            return style::resolve_line_height(storage, si);
            };
        const auto box_extra = [&]() {
            return IntrinsicSize{
                resolve_px(storage.padding_left[si], fs, root_fs, vp, 0.0f)
                    + resolve_px(storage.padding_right[si], fs, root_fs, vp, 0.0f)
                    + resolve_px(storage.border_left_width[si], fs, root_fs, vp, 0.0f)
                    + resolve_px(storage.border_right_width[si], fs, root_fs, vp, 0.0f),
                resolve_px(storage.padding_top[si], fs, root_fs, vp, 0.0f)
                    + resolve_px(storage.padding_bottom[si], fs, root_fs, vp, 0.0f)
                    + resolve_px(storage.border_top_width[si], fs, root_fs, vp, 0.0f)
                    + resolve_px(storage.border_bottom_width[si], fs, root_fs, vp, 0.0f)
            };
            };

        if (node->type == BoxType::Text) {
            const float width = tm.hasWidth()
                ? tm.measure_width(node->text_content, fs, family, bold, italic)
                : node->text_content.size() * std::max(1.0f, fs * 0.55f);
            return { width, line_height() };
        }

        const bool inline_flow = node->type == BoxType::Inline
            || node->type == BoxType::AnonymousBlock;
        if (inline_flow) {
            IntrinsicSize result;
            for (const auto& child : node->children) {
                if (child->style_soa_idx != UINT32_MAX
                    && storage.positions[child->style_soa_idx] == style::Position::Absolute)
                    continue;
                const IntrinsicSize child_size = measure_intrinsic(child.get(), storage, root_fs, tm);
                result.content_width += child_size.content_width;
                result.content_height = std::max(result.content_height, child_size.content_height);
            }
            const IntrinsicSize extra = box_extra();
            result.content_width += extra.content_width;
            result.content_height += extra.content_height;
            return result;
        }

        IntrinsicSize result;
        float inline_group_width = 0.0f;
        float inline_group_height = 0.0f;
        bool has_inline_group = false;
        for (const auto& child : node->children) {
            if (child->style_soa_idx != UINT32_MAX
                && storage.positions[child->style_soa_idx] == style::Position::Absolute)
                continue;
            const IntrinsicSize child_size = measure_intrinsic(child.get(), storage, root_fs, tm);
            if (child->type == BoxType::Block) {
                result.content_width = std::max(result.content_width, child_size.content_width);
                result.content_height += child_size.content_height;
            }
            else {
                has_inline_group = true;
                inline_group_width += child_size.content_width;
                inline_group_height = std::max(inline_group_height, child_size.content_height);
            }
        }
        if (has_inline_group) {
            result.content_width = std::max(result.content_width, inline_group_width);
            result.content_height += inline_group_height;
        }
        const IntrinsicSize extra = box_extra();
        result.content_width += extra.content_width;
        result.content_height += extra.content_height;
        return result;
    }

    // Возвращает content base main size; -1.0f означает auto.
    static float resolve_flex_basis(LayoutNode* item,
        const StyleStorageSoA& storage,
        float container_content_width,
        float root_fs,
        const TextMetrics& tm)
    {
        if (item->style_soa_idx == UINT32_MAX)
            return measure_intrinsic(item, storage, root_fs, tm).content_width;

        const uint32_t si = item->style_soa_idx;
        const float fs = storage.font_sizes[si];
        const style::Length& fb = storage.flex_bases[si];
        const style::Length& w = storage.widths[si];
        const Viewport vp{};
        const auto resolve = [&](const style::Length& length) {
            return length.unit == Unit::Percent
                ? length.value * 0.01f * container_content_width
                : resolve_px(length, fs, root_fs, vp, container_content_width);
            };

        if (!fb.is_auto() && !fb.is_none()) return resolve(fb);
        if (!w.is_auto() && !w.is_none()) return resolve(w);
        return measure_intrinsic(item, storage, root_fs, tm).content_width;
    }

    // =================================================================
    //  Phase 1 — построение
    // =================================================================

    static std::unique_ptr<LayoutNode> build_node(
        uint32_t soa_idx,
        const StyleStorageSoA& storage,
        const std::unordered_map<const DOMNode*, uint32_t>& dom_to_soa,
        const ImageMetrics& img_metrics)
    {
        if (storage.displays[soa_idx] == style::Display::None) return nullptr;

        const DOMNode* dom = storage.dom_nodes[soa_idx];
        if (!dom) return nullptr;

        auto node = std::make_unique<LayoutNode>();
        node->style_soa_idx = soa_idx;
        node->type = classify_box(storage.displays[soa_idx]);
        // <img> — replaced element. Заполняем src и intrinsic size,
        // больше не заходим в детей (их нет).

        const std::string_view tag(dom->tag_name.data(), dom->tag_name.size());

        if (tag == "td" || tag == "th") {
            for (const auto& kv : dom->attributes) {
                std::string_view k(kv.first.data(), kv.first.size());
                if (k == "colspan") {
                    try { node->colspan = std::max(1, std::stoi(std::string(kv.second))); }
                    catch (...) {}
                }
                if (k == "rowspan") {
                    try { node->rowspan = std::max(1, std::stoi(std::string(kv.second))); }
                    catch (...) {}
                }
            }
        }

        if (tag == "img") {
            node->type = BoxType::Image;

            std::string src;
            for (const auto& kv : dom->attributes) {
                std::string_view k(kv.first.data(), kv.first.size());
                if (k == "src") {
                    src.assign(kv.second.data(), kv.second.size());
                }
                // Читаем размеры из HTML-атрибутов
                if (k == "width") {
                    try { node->intrinsic_width = std::stof(std::string(kv.second)); }
                    catch (...) {}
                }
                if (k == "height") {
                    try { node->intrinsic_height = std::stof(std::string(kv.second)); }
                    catch (...) {}
                }
            }
            node->image_src = src;

            // Если размеры не были заданы в HTML, пытаемся узнать их из файла
            if (!src.empty() && img_metrics.hasIntrinsic() && (node->intrinsic_width == 0 || node->intrinsic_height == 0)) {
                auto [w, h] = img_metrics.intrinsic_size(src);
                if (node->intrinsic_width == 0) node->intrinsic_width = w;
                if (node->intrinsic_height == 0) node->intrinsic_height = h;
            }
            return node;
        }

        // CSS 2.1 §9.7: position: absolute и float: left/right
        // принудительно blockifies inline / inline-block в block-level.
        if (storage.positions[soa_idx] == style::Position::Absolute
            || storage.floats[soa_idx] != style::Float::None) {
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

                auto sub = build_node(it->second, storage, dom_to_soa, img_metrics);
                if (sub) node->add_child(std::move(sub));
            }
        }


        return node;
    }

    // Разбивает текстовую ноду на атомарные Text-узлы: слова и одиночные пробелы.
    // Подряд идущие пробелы схлопываются в один (white-space: normal).
    static void split_text_into_words(std::string_view text, LayoutNode* parent) {
        size_t i = 0, n = text.size();
        while (i < n) {
            size_t ws = i;
            while (i < n && std::isspace((unsigned char)text[i])) ++i;
            if (i > ws) {
                auto sp = std::make_unique<LayoutNode>();
                sp->type = BoxType::Text;
                sp->style_soa_idx = UINT32_MAX;
                sp->text_content = " ";
                parent->add_child(std::move(sp));
            }

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
            return BoxType::Block;
        case style::Display::Table:      return BoxType::Table;
        case style::Display::TableRow:   return BoxType::TableRow;
        case style::Display::TableCell:  return BoxType::TableCell;
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

    static void wrap_anonymous_blocks(LayoutNode* node,
        const StyleStorageSoA& storage) {
        if (!node) return;

        for (auto& c : node->children) wrap_anonymous_blocks(c.get(), storage);

        if (is_flex_container(node, storage)) {
            normalize_flex_children(node, storage);
            return;
        }

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

    static void normalize_flex_children(LayoutNode* node,
        const StyleStorageSoA& storage) {
        std::vector<std::unique_ptr<LayoutNode>> out;
        out.reserve(node->children.size());

        std::unique_ptr<LayoutNode> group;
        for (auto& child : node->children) {
            const bool is_absolute = child->style_soa_idx != UINT32_MAX
                && storage.positions[child->style_soa_idx] == style::Position::Absolute;

            if (is_absolute) {
                if (group) { out.push_back(std::move(group)); group.reset(); }
                out.push_back(std::move(child));
                continue;
            }

            if (child->is_inline_level()) {
                if (!group) {
                    group = std::make_unique<LayoutNode>();
                    group->type = BoxType::AnonymousBlock;
                    group->style_soa_idx = UINT32_MAX;
                    group->parent = node;
                }
                child->parent = group.get();
                group->children.push_back(std::move(child));
            }
            else {
                if (group) { out.push_back(std::move(group)); group.reset(); }
                out.push_back(std::move(child));
            }
        }
        if (group) out.push_back(std::move(group));
        node->children = std::move(out);
    }

    // =================================================================
    //  Phase 1: индекс containing blocks для absolute
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
    // CSS 2.1 §9.4.1: какие узлы устанавливают собственный BFC.
// Обычный блок (overflow: visible, position: static, float: none) —
// НЕ устанавливает, его float'ы «протекают» в родительский BFC.
    static bool establishes_bfc(const LayoutNode* node,
        const StyleStorageSoA& storage)
    {
        if (!node || node->style_soa_idx == UINT32_MAX) return false;
        const uint32_t si = node->style_soa_idx;

        if (storage.floats[si] != style::Float::None) return true;

        const auto pos = storage.positions[si];
        if (pos == style::Position::Absolute ||
            pos == style::Position::Fixed) return true;

        if (storage.overflows[si] != style::Overflow::Visible) return true;

        const auto d = storage.displays[si];
        if (d == style::Display::InlineBlock ||
            d == style::Display::TableCell ||
            d == style::Display::InlineFlex ||
            d == style::Display::Flex ||
            d == style::Display::Grid ||
            d == style::Display::InlineGrid) return true;

        return false;
    }
    // Определяет, является ли узел float'ом (left или right).
    static bool is_float(const LayoutNode* node,
        const StyleStorageSoA& storage)
    {
        if (!node || node->style_soa_idx == UINT32_MAX) return false;
        return storage.floats[node->style_soa_idx] != style::Float::None;
    }

    // =================================================================
    //  Flex-контейнер
    //
    //  Реализовано в 5 проходов:
    //    1) Собрать не-absolute детей и их базовые размеры.
    //    2) Распределить свободное место через flex-grow / flex-shrink.
    //    3) Разложить каждого ребёнка с финальной шириной.
    //    4) Позиционирование по главной оси (justify-content).
    //    5) Выравнивание по поперечной оси (align-items).
    //
    //  NOTE: предполагается, что в StyleStorageSoA есть поля
    //        flex_grows, flex_shrinks, justify_contents, align_items.
    //        Если их нет — добавьте или замените имена на свои.
    // =================================================================
        // =================================================================
    //  Flex-контейнер
    // =================================================================
    static void layout_flex_container(LayoutNode* node,
        float parent_width,
        const Viewport& vp,
        const StyleStorageSoA& storage,
        float root_fs,
        const TextMetrics& tm)
    {
        auto& g = node->geometry;
        resolve_box_model(node, storage, vp, parent_width, root_fs);

        const uint32_t node_si = (node->style_soa_idx != UINT32_MAX)
            ? node->style_soa_idx
            : effective_style_idx(node);

        // -------- content_width контейнера --------
        const float h_extra = g.margin_left + g.margin_right
            + g.padding_left + g.padding_right
            + g.border_left + g.border_right;
        float content_width = std::max(0.0f, parent_width - h_extra);

        // FIX: определяем, auto ли ширина контейнера — от этого зависит
        // shrink-to-fit ниже. Тесты (и inline-flex по спеке) ожидают,
        // что auto-width flex-контейнер сжимается по содержимому.
        bool container_width_auto = true;

        if (node->style_soa_idx != UINT32_MAX) {
            const float fs = storage.font_sizes[node_si];
            const bool border_box = (storage.box_sizings[node_si] == BoxSizing::BorderBox);
            const float pb_h = g.padding_left + g.padding_right
                + g.border_left + g.border_right;

            const style::Length& w = storage.widths[node_si];
            if (!w.is_auto() && !w.is_none()) {
                float spec = resolve_px(w, fs, root_fs, vp, parent_width);
                if (border_box) spec -= pb_h;
                content_width = std::max(0.0f, spec);
                container_width_auto = false;
            }

            const style::Length& maxw = storage.max_widths[node_si];
            if (!maxw.is_none()) {
                float mw = resolve_px(maxw, fs, root_fs, vp, parent_width);
                if (border_box) mw -= pb_h;
                if (mw > 0.0f) content_width = std::min(content_width, mw);
            }

            const style::Length& minw = storage.min_widths[node_si];
            if (!minw.is_none()) {
                float mw = resolve_px(minw, fs, root_fs, vp, parent_width);
                if (border_box) mw -= pb_h;
                if (mw > 0.0f) content_width = std::max(content_width, mw);
            }
        }

        const float inner_x = g.border_left + g.padding_left;
        const float inner_y = g.border_top + g.padding_top;

        // -------- Pass 1: собрать items и базовые размеры --------
        struct FlexItem {
            LayoutNode* node = nullptr;
            float base_main = 0.0f;
            float outer_main = 0.0f;
            float cross_outer = 0.0f;
            float grow = 0.0f;
            float shrink = 1.0f;
        };

        std::vector<FlexItem> items;
        items.reserve(node->children.size());

        for (auto& child : node->children) {
            if (child->style_soa_idx != UINT32_MAX
                && storage.positions[child->style_soa_idx] == style::Position::Absolute) {
                child->geometry.flow_x = inner_x;
                child->geometry.flow_y = inner_y;
                continue;
            }

            resolve_box_model(child.get(), storage, vp, content_width, root_fs);

            const float base = resolve_flex_basis(
                child.get(), storage, content_width, root_fs, tm);

            const float pb_h = child->geometry.padding_left
                + child->geometry.padding_right
                + child->geometry.border_left
                + child->geometry.border_right;

            float outer_main = base;
            float grow = 0.0f;
            float shrink = 1.0f;
            if (child->style_soa_idx != UINT32_MAX) {
                const bool border_box =
                    storage.box_sizings[child->style_soa_idx] == BoxSizing::BorderBox;
                if (!border_box) outer_main = base + pb_h;
                grow = storage.flex_grows[child->style_soa_idx];
                shrink = storage.flex_shrinks[child->style_soa_idx];
            }

            FlexItem item;
            item.node = child.get();
            item.base_main = base;
            item.outer_main = outer_main;
            item.cross_outer = 0.0f;
            item.grow = grow;
            item.shrink = shrink;
            items.push_back(item);
        }

        // -------- Pass 2: free space и распределение --------
        float total_outer = 0.0f;
        float total_grow = 0.0f;
        float total_shrink_weight = 0.0f;
        for (const auto& it : items) {
            total_outer += it.outer_main
                + it.node->geometry.margin_left
                + it.node->geometry.margin_right;
            total_grow += it.grow;
            total_shrink_weight += it.shrink * it.base_main;
        }

        // FIX: при auto-ширине контейнера "бюджет" = сумма базовых размеров
        // (shrink-to-fit). Тогда free_space = 0, grow-распределение не
        // срабатывает, а justify-content тоже не сдвигает (нет свободного
        // места). Иначе бюджет — явная content_width.
        const float budget = container_width_auto ? total_outer : content_width;
        const float free_space = budget - total_outer;

        if (free_space > 0.0f && total_grow > 0.0f) {
            for (auto& it : items) {
                if (it.grow <= 0.0f) continue;
                it.outer_main += free_space * (it.grow / total_grow);
            }
        }
        else if (free_space < 0.0f && total_shrink_weight > 0.0f) {
            const float deficit = -free_space;
            for (auto& it : items) {
                if (it.shrink <= 0.0f) continue;
                const float weight = it.shrink * it.base_main;
                const float share = deficit * (weight / total_shrink_weight);
                it.outer_main = std::max(0.0f, it.outer_main - share);
            }
        }

        // -------- Pass 3: layout каждого item на (0,0) — только чтобы
        // узнать итоговые размеры. Позиции расставим на Pass 5.
        for (auto& it : items) {
            const float pb_h = it.node->geometry.padding_left
                + it.node->geometry.padding_right
                + it.node->geometry.border_left
                + it.node->geometry.border_right;
            const float override_content_w = std::max(0.0f, it.outer_main - pb_h);

            // Flex item — blockified, устанавливает свой BFC.
            FloatContext item_fc;
            layout_node(it.node, 0.0f, 0.0f, content_width, vp, storage,
                root_fs, tm, &item_fc, override_content_w);

            it.cross_outer = it.node->geometry.height
                + it.node->geometry.margin_top
                + it.node->geometry.margin_bottom;
        }

        float max_cross_outer = 0.0f;
        for (const auto& it : items) {
            max_cross_outer = std::max(max_cross_outer, it.cross_outer);
        }

        // FIX: считаем реальный использованный main-размер и (при auto)
        // подрезаем content_width контейнера до него.
        float used_main = 0.0f;
        for (const auto& it : items) {
            used_main += it.node->geometry.width
                + it.node->geometry.margin_left
                + it.node->geometry.margin_right;
        }

        if (container_width_auto) {
            content_width = used_main;

            // Переприменяем min/max к финальному значению.
            if (node->style_soa_idx != UINT32_MAX) {
                const float fs = storage.font_sizes[node_si];
                const bool border_box = (storage.box_sizings[node_si] == BoxSizing::BorderBox);
                const float pb_h = g.padding_left + g.padding_right
                    + g.border_left + g.border_right;

                const style::Length& maxw = storage.max_widths[node_si];
                if (!maxw.is_none()) {
                    float mw = resolve_px(maxw, fs, root_fs, vp, parent_width);
                    if (border_box) mw -= pb_h;
                    if (mw > 0.0f) content_width = std::min(content_width, mw);
                }

                const style::Length& minw = storage.min_widths[node_si];
                if (!minw.is_none()) {
                    float mw = resolve_px(minw, fs, root_fs, vp, parent_width);
                    if (border_box) mw -= pb_h;
                    if (mw > 0.0f) content_width = std::max(content_width, mw);
                }
            }
        }

        // -------- Pass 4: justify-content (главная ось) --------
        const float remaining = content_width - used_main;
        float main_offset = 0.0f;
        float item_gap = 0.0f;

        style::Justify justify = style::Justify::Start;
        if (node->style_soa_idx != UINT32_MAX) {
            justify = storage.justify_contents[node->style_soa_idx];
        }

        if (remaining > 0.0f && !items.empty()) {
            switch (justify) {
            case style::Justify::End:
                main_offset = remaining;
                break;
            case style::Justify::Center:
                main_offset = remaining * 0.5f;
                break;
            case style::Justify::SpaceBetween:
                if (items.size() > 1) item_gap = remaining / (items.size() - 1);
                break;
            case style::Justify::SpaceAround:
                item_gap = remaining / items.size();
                main_offset = item_gap * 0.5f;
                break;
            case style::Justify::SpaceEvenly:
                item_gap = remaining / (items.size() + 1);
                main_offset = item_gap;
                break;
            case style::Justify::Start:
            default:
                break;
            }
        }

        // -------- Pass 5: align-items (поперечная ось) + позиции --------
        style::AlignItems align = style::AlignItems::FlexStart;
        if (node->style_soa_idx != UINT32_MAX) {
            align = storage.align_items[node->style_soa_idx];
        }

        const float flex_origin_x = g.x + inner_x;
        const float flex_origin_y = g.y + inner_y;

        float x_cursor = main_offset;
        for (auto& it : items) {
            LayoutNode* child = it.node;

            float cross_offset = 0.0f;
            if (align == style::AlignItems::Center) {
                cross_offset = (max_cross_outer - it.cross_outer) * 0.5f;
            }
            else if (align == style::AlignItems::FlexEnd) {
                cross_offset = max_cross_outer - it.cross_outer;
            }

            // Ребёнок сейчас в (0,0). Сдвигаем весь поддерево в целевую
            // абсолютную позицию.
            const float target_x = flex_origin_x + x_cursor + child->geometry.margin_left;
            const float target_y = flex_origin_y + cross_offset + child->geometry.margin_top;
            shift_subtree(child, target_x - child->geometry.x,
                target_y - child->geometry.y);

            x_cursor += child->geometry.margin_left
                + child->geometry.width
                + child->geometry.margin_right
                + item_gap;
        }

        // -------- content_height контейнера --------
        float content_height = max_cross_outer;

        if (node->style_soa_idx != UINT32_MAX) {
            const float fs = storage.font_sizes[node_si];
            const bool border_box = (storage.box_sizings[node_si] == BoxSizing::BorderBox);
            const float pb_v = g.padding_top + g.padding_bottom
                + g.border_top + g.border_bottom;

            const style::Length& h = storage.heights[node_si];
            if (!h.is_auto() && !h.is_none()) {
                float spec = resolve_px(h, fs, root_fs, vp, parent_width);
                if (border_box) spec -= pb_v;
                content_height = std::max(0.0f, spec);
            }

            const style::Length& maxh = storage.max_heights[node_si];
            if (!maxh.is_none()) {
                float mh = resolve_px(maxh, fs, root_fs, vp, parent_width);
                if (border_box) mh -= pb_v;
                if (mh > 0.0f) content_height = std::min(content_height, mh);
            }

            const style::Length& minh = storage.min_heights[node_si];
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

    // Замер минимальной (самое длинное слово) и максимальной (вся строка) ширины
    static std::pair<float, float> measure_cell_min_max(const LayoutNode* node,
        const StyleStorageSoA& storage, float root_fs, const TextMetrics& tm)
    {
        if (!node) return { 0.0f, 0.0f };

        if (node->type == BoxType::Text) {
            const uint32_t si = effective_style_idx(node);
            const float fs = storage.font_sizes[si];
            const std::string& family = storage.font_families[si];
            const bool bold = storage.font_weights[si] >= 700;
            const auto fstyle = storage.font_styles[si];
            const bool italic = (fstyle == style::FontStyle::Italic || fstyle == style::FontStyle::Oblique);

            float w = tm.hasWidth() ? tm.measure_width(node->text_content, fs, family, bold, italic)
                : node->text_content.size() * std::max(1.0f, fs * 0.55f);
            // Для одного слова min и max совпадают
            return { w, w };
        }

        // Для контейнеров (Inline, Block и т.д.) собираем суммы
        float max_w = 0.0f;
        float min_w = 0.0f;
        float inline_group_max = 0.0f;
        float inline_group_min = 0.0f;
        bool has_inline = false;

        const uint32_t si = effective_style_idx(node);
        const float fs = storage.font_sizes[si];
        const Viewport vp{};

        for (const auto& child : node->children) {
            if (child->style_soa_idx != UINT32_MAX && storage.positions[child->style_soa_idx] == style::Position::Absolute) continue;

            auto [c_min, c_max] = measure_cell_min_max(child.get(), storage, root_fs, tm);

            if (child->is_block_level()) {
                if (has_inline) {
                    max_w = std::max(max_w, inline_group_max);
                    min_w = std::max(min_w, inline_group_min);
                    has_inline = false;
                    inline_group_max = 0.0f;
                    inline_group_min = 0.0f;
                }
                max_w = std::max(max_w, c_max);
                min_w = std::max(min_w, c_min);
            }
            else {
                has_inline = true;
                inline_group_max += c_max;
                // Для min-content слова переносятся, поэтому берем максимум из слов
                inline_group_min = std::max(inline_group_min, c_min);
            }
        }

        if (has_inline) {
            max_w = std::max(max_w, inline_group_max);
            min_w = std::max(min_w, inline_group_min);
        }

        // Добавляем padding/border
        const float extra = resolve_px(storage.padding_left[si], fs, root_fs, vp, 0.0f)
            + resolve_px(storage.padding_right[si], fs, root_fs, vp, 0.0f)
            + resolve_px(storage.border_left_width[si], fs, root_fs, vp, 0.0f)
            + resolve_px(storage.border_right_width[si], fs, root_fs, vp, 0.0f);

        return { min_w + extra, max_w + extra };
    }

    static void layout_table(LayoutNode* table,
        float parent_width,
        const Viewport& vp,
        const StyleStorageSoA& storage,
        float root_fs,
        const TextMetrics& tm)
    {
        auto& g = table->geometry;
        resolve_box_model(table, storage, vp, parent_width, root_fs);

        const uint32_t si = table->style_soa_idx;
        const float fs = (si != UINT32_MAX) ? storage.font_sizes[si] : 16.0f;

        float h_extra = g.margin_left + g.margin_right + g.padding_left + g.padding_right + g.border_left + g.border_right;
        float content_width = std::max(0.0f, parent_width - h_extra);

        bool width_auto = true;
        if (si != UINT32_MAX) {
            const style::Length& w = storage.widths[si];
            if (!w.is_auto() && !w.is_none()) {
                width_auto = false;
                float spec = resolve_px(w, fs, root_fs, vp, parent_width);
                if (storage.box_sizings[si] == BoxSizing::BorderBox)
                    spec -= (g.padding_left + g.padding_right + g.border_left + g.border_right);
                content_width = std::max(0.0f, spec);
            }
        }

        float cell_padding = 0.0f;
        if (si != UINT32_MAX) {
            const DOMNode* dom = storage.dom_nodes[si];
            if (dom) {
                for (const auto& kv : dom->attributes) {
                    std::string_view k(kv.first.data(), kv.first.size());
                    if (k == "cellpadding") {
                        try { cell_padding = std::stof(std::string(kv.second)); }
                        catch (...) {}
                    }
                }
            }
        }

        std::vector<LayoutNode*> rows;
        std::function<void(LayoutNode*)> collect_rows = [&](LayoutNode* n) {
            for (auto& c : n->children) {
                if (c->type == BoxType::TableRow) rows.push_back(c.get());
                else if (c->type == BoxType::Block) collect_rows(c.get());
            }
            };
        collect_rows(table);

        if (rows.empty()) {
            g.width = g.border_left + g.padding_left + content_width + g.padding_right + g.border_right;
            g.height = g.border_top + g.padding_top + g.border_bottom + g.padding_bottom;
            return;
        }

        struct CellPlacement {
            LayoutNode* cell;
            int r, c, colspan, rowspan;
        };
        std::vector<CellPlacement> placements;
        int num_cols = 0;
        std::vector<std::vector<int>> occupied;

        // 1. Строим сетку
        for (int r = 0; r < (int)rows.size(); ++r) {
            int c = 0;
            for (auto& child : rows[r]->children) {
                if (child->type != BoxType::TableCell) continue;

                while (true) {
                    if (r >= (int)occupied.size()) occupied.resize(r + 1);
                    if (c >= (int)occupied[r].size()) occupied[r].resize(c + 1, 0);
                    if (occupied[r][c] == 0) break;
                    c++;
                }

                int colspan = std::max(1, child->colspan);
                int rowspan = std::max(1, child->rowspan);

                for (int rr = 0; rr < rowspan; ++rr) {
                    if (r + rr >= (int)occupied.size()) occupied.resize(r + rr + 1);
                    for (int cc = 0; cc < colspan; ++cc) {
                        if (c + cc >= (int)occupied[r + rr].size()) occupied[r + rr].resize(c + cc + 1, 0);
                        occupied[r + rr][c + cc] = 1;
                    }
                }
                placements.push_back({ child.get(), r, c, colspan, rowspan });
                num_cols = std::max(num_cols, c + colspan);
                c += colspan;
            }
        }

        // 2. Рассчитываем Min и Max ширины для каждой колонки
        std::vector<float> col_min(num_cols, 0.0f);
        std::vector<float> col_max(num_cols, 0.0f);

        for (auto& p : placements) {
            auto [c_min, c_max] = measure_cell_min_max(p.cell, storage, root_fs, tm);
            // Влияние явной ширины ячейки
            if (p.cell->style_soa_idx != UINT32_MAX) {
                const auto& w = storage.widths[p.cell->style_soa_idx];
                if (!w.is_auto() && !w.is_none()) {
                    float spec = resolve_px(w, storage.font_sizes[p.cell->style_soa_idx], root_fs, vp, content_width);
                    c_min = std::max(c_min, spec);
                    c_max = std::max(c_max, spec);
                }
            }
            // Распределяем требования к ширине по колонкам (включая colspan)
            float per_col_min = c_min / p.colspan;
            float per_col_max = c_max / p.colspan;
            for (int i = 0; i < p.colspan; ++i) {
                col_min[p.c + i] = std::max(col_min[p.c + i], per_col_min);
                col_max[p.c + i] = std::max(col_max[p.c + i], per_col_max);
            }
        }

        // 3. Распределяем доступное пространство
        float total_min = 0.0f, total_max = 0.0f;
        for (int i = 0; i < num_cols; ++i) {
            total_min += col_min[i];
            total_max += col_max[i];
        }

        std::vector<float> col_widths(num_cols, 0.0f);

        if (width_auto) {
            // Если ширина таблицы auto, она занимает ровно total_max (но не меньше parent_width)
            content_width = std::max(total_max, std::min(content_width, parent_width));
        }

        if (content_width <= total_min) {
            // Места мало — берем min и сжимаем пропорционально
            float scale = (total_min > 0) ? (content_width / total_min) : 1.0f;
            for (int i = 0; i < num_cols; ++i) col_widths[i] = col_min[i] * scale;
        }
        else if (content_width >= total_max) {
            // Места много — берем max и распределяем излишек пропорционально max_width
            for (int i = 0; i < num_cols; ++i) col_widths[i] = col_max[i];
            float extra = content_width - total_max;
            if (total_max > 0) {
                for (int i = 0; i < num_cols; ++i) col_widths[i] += extra * (col_max[i] / total_max);
            }
            else {
                for (int i = 0; i < num_cols; ++i) col_widths[i] += extra / num_cols;
            }
        }
        else {
            // Золотая середина: интерполируем между min и max
            float ratio = (content_width - total_min) / (total_max - total_min);
            for (int i = 0; i < num_cols; ++i) {
                col_widths[i] = col_min[i] + ratio * (col_max[i] - col_min[i]);
            }
        }

        // 4. Расстановка ячеек
        float inner_x = g.x + g.border_left + g.padding_left;
        float y_cursor = g.y + g.border_top + g.padding_top;
        std::vector<float> row_y(rows.size(), 0.0f);

        for (int r = 0; r < (int)rows.size(); ++r) {
            row_y[r] = y_cursor;
            float row_height = 0;

            // Сначала раскладываем ячейки, чтобы узнать их реальную высоту
            for (auto& p : placements) {
                if (p.r == r) {
                    float cell_x = inner_x;
                    for (int i = 0; i < p.c; ++i) cell_x += col_widths[i];

                    float cell_w = 0;
                    for (int i = 0; i < p.colspan; ++i) cell_w += col_widths[p.c + i];

                    p.cell->geometry.x = cell_x;
                    p.cell->geometry.y = y_cursor;

                    layout_block_container(p.cell, cell_w, vp, storage, root_fs, tm, nullptr, cell_w, cell_padding);
                    row_height = std::max(row_height, p.cell->geometry.height);
                }
            }

            // Выравниваем ячейки по вертикали (valign)
            for (auto& p : placements) {
                if (p.r == r) {
                    float actual_h = p.cell->geometry.height;
                    float offset = 0.0f;

                    if (p.cell->style_soa_idx != UINT32_MAX) {
                        auto va = storage.vertical_aligns[p.cell->style_soa_idx];
                        if (va == style::VerticalAlign::Middle) {
                            offset = (row_height - actual_h) * 0.5f;
                        }
                        else if (va == style::VerticalAlign::Bottom) {
                            offset = row_height - actual_h;
                        }
                        // Top — offset остается 0
                    }

                    // Принудительно задаем высоту строки
                    p.cell->geometry.height = row_height;

                    // Сдвигаем внутренний контент (детей), если надо
                    if (offset > 0.0f) {
                        for (auto& child : p.cell->children) {
                            shift_subtree(child.get(), 0.0f, offset);
                        }
                    }
                }
            }
            y_cursor += row_height;
        }

        g.width = g.border_left + g.padding_left + content_width + g.padding_right + g.border_right;
        g.height = y_cursor - g.y + g.padding_bottom + g.border_bottom;

        for (int r = 0; r < (int)rows.size(); ++r) {
            rows[r]->geometry.x = inner_x;
            rows[r]->geometry.y = row_y[r];
            rows[r]->geometry.width = content_width;
            rows[r]->geometry.height = (r + 1 < (int)rows.size()) ? (row_y[r + 1] - row_y[r]) : (y_cursor - row_y[r]);
        }
    }
    // Flex-контейнер — новый путь в диспетчере.
    // Не добавляем BoxType::Flex: flex — это режим layout,
    // а не тип бокса. Внешне контейнер остаётся Block.
        // abs_x, abs_y — АБСОЛЮТНАЯ позиция border-box узла.
    // layout_node присваивает её в node->geometry.x/y в самом начале,
    // чтобы все вложенные FC могли пользоваться абсолютными координатами.
    static void layout_node(LayoutNode* node,
        float abs_x, float abs_y,
        float parent_width,
        const Viewport& vp,
        const StyleStorageSoA& storage,
        float root_fs,
        const TextMetrics& tm,
        FloatContext* parent_fc,
        float override_content_width = -1.0f)
    {
        node->geometry.x = abs_x;
        node->geometry.y = abs_y;

        // --- НОВОЕ: Диспетчеризация таблиц ---
        if (node->type == BoxType::Table) {
            layout_table(node, parent_width, vp, storage, root_fs, tm);
            return;
        }
        // -------------------------------------

        if (is_flex_container(node, storage)) {
            layout_flex_container(node, parent_width, vp, storage, root_fs, tm);
            return;
        }

        switch (node->type) {
        case BoxType::AnonymousBlock:
            layout_ifc(node, parent_width, vp, storage, root_fs, tm, parent_fc);
            node->geometry.width = parent_width;
            break;
        case BoxType::Block:
        case BoxType::TableCell: // <--- Добавили
        case BoxType::TableRow:  // <--- Добавили
            layout_block_container(node, parent_width, vp, storage, root_fs,
                tm, parent_fc, override_content_width);
            break;
        case BoxType::Inline:
            layout_inline(node, parent_width, vp, storage, root_fs, tm, parent_fc);
            break;
        case BoxType::Text:
            layout_text(node, parent_width, vp, storage, root_fs, tm);
            break;
        case BoxType::Image:
            layout_image(node, parent_width, vp, storage, root_fs);
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
        if (si == UINT32_MAX) return;
        if (storage.positions[si] != style::Position::Relative) return;

        const float fs = storage.font_sizes[si];
        const style::Length& top_inset = storage.top_insets[si];
        const style::Length& left_inset = storage.left_insets[si];
        const style::Length& bottom_inset = storage.bottom_insets[si];
        const style::Length& right_inset = storage.right_insets[si];

        const float top = resolve_px(top_inset, fs, root_fs, vp, node->geometry.width);
        const float left = resolve_px(left_inset, fs, root_fs, vp, node->geometry.width);
        const float bottom = resolve_px(bottom_inset, fs, root_fs, vp, node->geometry.height);
        const float right = resolve_px(right_inset, fs, root_fs, vp, node->geometry.width);

        const float offset_x = !left_inset.is_auto() ? left
            : (!right_inset.is_auto() ? -right : 0.0f);
        const float offset_y = !top_inset.is_auto() ? top
            : (!bottom_inset.is_auto() ? -bottom : 0.0f);

        if (offset_x == 0.0f && offset_y == 0.0f) return;

        // Сдвигаем только собственный border-box, сохраняя flow_x/flow_y.
        node->geometry.x += offset_x;
        node->geometry.y += offset_y;

        // Потомки (включая absolute static-positions) — целиком.
        for (auto& c : node->children)
            shift_subtree(c.get(), offset_x, offset_y);
    }

    // =================================================================
    //  Пост-проход: сохранить flow-позицию и применить relative
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

        if (node->style_soa_idx != UINT32_MAX
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

    static void layout_absolute_descendants(LayoutNode* cb, const Viewport& vp,
        const StyleStorageSoA& storage, float root_fs, const TextMetrics& tm)
    {
        if (!cb) return;

        const float cb_content_x = cb->geometry.x
            + cb->geometry.border_left + cb->geometry.padding_left;
        const float cb_content_y = cb->geometry.y
            + cb->geometry.border_top + cb->geometry.padding_top;
        const float cb_content_width = cb->geometry.width
            - cb->geometry.border_left - cb->geometry.padding_left
            - cb->geometry.padding_right - cb->geometry.border_right;
        const float cb_content_height = cb->geometry.height
            - cb->geometry.border_top - cb->geometry.padding_top
            - cb->geometry.padding_bottom - cb->geometry.border_bottom;

        for (LayoutNode* child : cb->absolute_children) {
            if (!child || child->style_soa_idx == UINT32_MAX) continue;
            layout_absolute(child, cb_content_x, cb_content_y,
                cb_content_width, cb_content_height,
                vp, storage, root_fs, tm);
            layout_absolute_descendants(child, vp, storage, root_fs, tm);
        }

        for (auto& child : cb->children) {
            const bool is_abs =
                child->style_soa_idx != UINT32_MAX
                && storage.positions[child->style_soa_idx] == style::Position::Absolute;
            if (is_abs) continue;   // их обход делает цикл по cb->absolute_children
            layout_absolute_descendants(child.get(), vp, storage, root_fs, tm);
        }
    }

    static void layout_absolute(LayoutNode* node,
        float cb_content_x, float cb_content_y,
        float cb_content_width, float cb_content_height,
        const Viewport& vp,
        const StyleStorageSoA& storage,
        float root_fs,
        const TextMetrics& tm)
    {
        const uint32_t si = node->style_soa_idx;
        if (si == UINT32_MAX) return;
        if (storage.positions[si] != style::Position::Absolute) return;

        const float fs = storage.font_sizes[si];
        auto& g = node->geometry;

        // 1. Box model — обязательно ДО чтения margin_left/right.
        resolve_box_model(node, storage, vp, cb_content_width, root_fs);

        const bool border_box = (storage.box_sizings[si] == BoxSizing::BorderBox);
        const float pb_h = g.padding_left + g.padding_right
            + g.border_left + g.border_right;
        const float pb_v = g.padding_top + g.padding_bottom
            + g.border_top + g.border_bottom;

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

        // 2. content-width (внутренняя ширина контейнера).
        float content_width;
        if (!width_auto) {
            float w = resolve_px(width_spec, fs, root_fs, vp, cb_content_width);
            content_width = border_box ? std::max(0.0f, w - pb_h) : std::max(0.0f, w);
        }
        else if (has_left && has_right) {
            const float bw = std::max(0.0f, cb_content_width - left - right
                - g.margin_left - g.margin_right);
            content_width = std::max(0.0f, bw - pb_h);
        }
        else {
            // shrink-to-fit
            const float intrinsic = measure_intrinsic(node, storage, root_fs, tm).content_width;
            content_width = intrinsic;

            if (has_left) {
                content_width = std::min(content_width,
                    std::max(0.0f, cb_content_width - left - g.margin_left
                        - g.margin_right - pb_h));
            }
            else if (has_right) {
                content_width = std::min(content_width,
                    std::max(0.0f, cb_content_width - right - g.margin_left
                        - g.margin_right - pb_h));
            }
            else {
                content_width = std::min(content_width,
                    std::max(0.0f, cb_content_width - g.margin_left
                        - g.margin_right - pb_h));
            }
        }

        // auto-margin по горизонтали.
        if (has_left && has_right && !width_auto) {
            const float bw = content_width + pb_h;
            const float used = left + right + bw;
            const float free = cb_content_width - used;
            const bool ml_auto = storage.margin_left[si].is_auto();
            const bool mr_auto = storage.margin_right[si].is_auto();
            if (free > 0.0f && (ml_auto || mr_auto)) {
                if (ml_auto && mr_auto) {
                    g.margin_left = free * 0.5f;
                    g.margin_right = free - g.margin_left;
                }
                else if (ml_auto) g.margin_left = free;
                else              g.margin_right = free;
            }
        }

        // 3. Абсолютный x.
        float abs_x;
        if (has_left) {
            abs_x = cb_content_x + left + g.margin_left;
        }
        else if (has_right) {
            const float bw = content_width + pb_h;
            abs_x = cb_content_x + cb_content_width - right - g.margin_right - bw;
        }
        else {
            abs_x = node->geometry.flow_x;
        }

        // 4. Ограничения высоты + y.
        float computed_bh = 0.0f;
        bool  height_from_constraints = false;

        if (!height_auto) {
            float h = resolve_px(height_spec, fs, root_fs, vp, cb_content_width);
            computed_bh = border_box ? h : (h + pb_v);
            height_from_constraints = true;
        }
        else if (has_top && has_bottom && cb_content_height >= 0.0f) {
            computed_bh = std::max(0.0f, cb_content_height - top - bottom
                - g.margin_top - g.margin_bottom);
            height_from_constraints = true;
        }

        float abs_y;
        if (has_top) {
            abs_y = cb_content_y + top + g.margin_top;
        }
        else if (has_bottom && height_from_constraints) {
            abs_y = cb_content_y + cb_content_height - bottom - g.margin_bottom
                - computed_bh;
        }
        else {
            abs_y = node->geometry.flow_y;
        }

        // 5. Раскладка контейнера.
// absolute устанавливает BFC → передаём nullptr,
// layout_block_container сам создаст local_fc.
        node->geometry.x = abs_x;
        node->geometry.y = abs_y;

        layout_block_container(node, content_width, vp, storage, root_fs, tm,
            /*parent_fc=*/nullptr, /*override_content_width=*/content_width);

        // Восстанавливаем позицию (layout_block_container её не пишет, но на всякий).
        node->geometry.x = abs_x;
        node->geometry.y = abs_y;

        if (height_from_constraints) {
            node->geometry.height = computed_bh;
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

    static bool is_br(const LayoutNode* node, const StyleStorageSoA& storage) {
        if (!node || node->style_soa_idx == UINT32_MAX) return false;
        const DOMNode* dom = storage.dom_nodes[node->style_soa_idx];
        if (!dom) return false;
        const std::string_view tag(dom->tag_name.data(), dom->tag_name.size());
        return tag == "br";
    }


    // =================================================================
    //  Inline Formatting Context
    //
    //  Раскладывает inline-level детей по строкам с переносом.
    //  Каждая строка выравнивается по baseline детей, а не по верхам.
    //
    //  Проход по детям: копим их в буфер строки (ширина + ascent +
    //  descent), пока не накопится переполнение или <br>. y пока не
    //  ставим.
    //
    //  flush_line: по максимальным ascent/descent строки вычисляем
    //  высоту, применяем text-align (offset по x), расставляем y
    //  каждого ребёнка так, чтобы baseline'ы совпали.
    // =================================================================
    static void layout_ifc(LayoutNode* node, float parent_width,
        const Viewport& vp,
        const StyleStorageSoA& storage,
        float root_fs,
        const TextMetrics& tm,
        FloatContext* fc)
    {
        auto& g = node->geometry;

        const float inner_x_abs = g.x + g.border_left + g.padding_left;
        const float inner_y_abs = g.y + g.border_top + g.padding_top;

        const float h_extra = g.margin_left + g.margin_right
            + g.padding_left + g.padding_right
            + g.border_left + g.border_right;
        const float content_avail = std::max(0.0f, parent_width - h_extra);

        const uint32_t si_align = effective_style_idx(node);
        const style::TextAlign align = storage.text_aligns[si_align];

        // Strut (без изменений).
        const float strut_fs = storage.font_sizes[si_align];
        const std::string& strut_family = storage.font_families[si_align];
        const bool  strut_bold = storage.font_weights[si_align] >= 700;
        const float strut_lh = style::resolve_line_height(storage, si_align);
        float strut_asc, strut_desc;
        if (tm.hasBaseline()) {
            strut_asc = tm.ascent(strut_fs, strut_family, strut_bold);
            strut_desc = tm.descent(strut_fs, strut_family, strut_bold);
        }
        else {
            strut_asc = strut_fs * 0.8f;
            strut_desc = strut_fs * 0.2f;
        }
        const float strut_half_leading =
            std::max(0.0f, (strut_lh - strut_asc - strut_desc) * 0.5f);
        const float strut_ascent = strut_half_leading + strut_asc;
        const float strut_descent = strut_half_leading + strut_desc;

        struct LineItem {
            LayoutNode* node;
            float width;
            float ascent;
            float descent;
            style::VerticalAlign va;
        };
        std::vector<LineItem> line;

        float line_x = 0.0f;
        float line_top_y = 0.0f;
        float line_max_ascent = 0.0f;
        float line_max_descent = 0.0f;
        float max_line_width = 0.0f;
        float last_line_ascent = 0.0f;
        float last_line_descent = 0.0f;

        // Текущие границы свободного x-диапазона строки.
        // При отсутствии float'ов line_left == inner_x_abs, avail == content_avail.
        float line_left = inner_x_abs;
        float avail = content_avail;

        auto recompute_line_range = [&]() {
            float L = inner_x_abs;
            float R = inner_x_abs + content_avail;
            if (fc) {
                fc->available_range(inner_y_abs + line_top_y,
                    /*cb_left=*/inner_x_abs,
                    /*cb_right=*/inner_x_abs + content_avail,
                    L, R);
            }
            if (L > R) L = R;
            line_left = L;
            avail = R - L;
            };
        recompute_line_range();

        auto flush_line = [&](bool forced) {
            if (line.empty() && !forced) return;

            const float ma = std::max(line_max_ascent, strut_ascent);
            const float md = std::max(line_max_descent, strut_descent);
            const float line_h = ma + md;

            float offset = 0.0f;
            bool  justify_mode = false;
            float per_space = 0.0f;

            if (align == style::TextAlign::Right || align == style::TextAlign::End) {
                offset = std::max(0.0f, avail - line_x);
            }
            else if (align == style::TextAlign::Center) {
                offset = std::max(0.0f, (avail - line_x) * 0.5f);
            }
            else if (align == style::TextAlign::Justify && !forced) {
                const float extra = avail - line_x;
                if (extra > 0.0f) {
                    int spaces = 0;
                    for (const auto& it : line) {
                        if (it.node->type == BoxType::Text
                            && it.node->text_content == " ") ++spaces;
                    }
                    if (spaces > 2) {
                        per_space = extra / float(spaces);
                        justify_mode = true;
                    }
                }
            }

            const float baseline_y = line_top_y + ma;
            float x_cursor = offset;

            for (auto& it : line) {
                float y_from_content;
                switch (it.va) {
                case style::VerticalAlign::Top:
                    y_from_content = line_top_y + it.node->geometry.margin_top;
                    break;
                case style::VerticalAlign::Bottom:
                    y_from_content = line_top_y + line_h
                        - it.node->geometry.height
                        - it.node->geometry.margin_bottom;
                    break;
                case style::VerticalAlign::Middle: {
                    const float x_half = strut_fs * 0.25f;
                    y_from_content = baseline_y - x_half
                        - it.node->geometry.height * 0.5f;
                    break;
                }
                case style::VerticalAlign::TextTop:
                    y_from_content = baseline_y - strut_asc;
                    break;
                case style::VerticalAlign::TextBottom:
                    y_from_content = baseline_y + strut_desc
                        - it.node->geometry.height;
                    break;
                default:
                    y_from_content = baseline_y - it.ascent;
                    break;
                }

                const float target_x = line_left + x_cursor
                    + it.node->geometry.margin_left;
                const float target_y = inner_y_abs + y_from_content;

                shift_subtree(it.node,
                    target_x - it.node->geometry.x,
                    target_y - it.node->geometry.y);

                x_cursor += it.width;
                if (justify_mode
                    && it.node->type == BoxType::Text
                    && it.node->text_content == " ") {
                    it.node->geometry.width += per_space;
                    x_cursor += per_space;
                }
            }

            max_line_width = std::max(max_line_width, line_x);
            last_line_ascent = ma;
            last_line_descent = md;
            line_top_y += line_h;

            line.clear();
            line_x = 0.0f;
            line_max_ascent = 0.0f;
            line_max_descent = 0.0f;
            };

        for (auto& child : node->children) {
            // Внутренние размеры inline-детей считаем от полной ширины
            // IFC (content_avail). Их собственные строки сузятся через
            // собственный layout_ifc (см. рекурсию в layout_inline).
            layout_node(child.get(), 0.0f, 0.0f, content_avail, vp, storage,
                root_fs, tm, fc);

            if (is_br(child.get(), storage)) {
                flush_line(/*forced=*/true);
                recompute_line_range();
                child->geometry.x = line_left;
                child->geometry.y = inner_y_abs + line_top_y;
                child->geometry.width = 0.0f;
                child->geometry.height = 0.0f;
                continue;
            }

            const float w = child->geometry.width
                + child->geometry.margin_left + child->geometry.margin_right;

            const bool is_space = (child->type == BoxType::Text
                && child->text_content == " ");

            if (is_space && line_x == 0.0f) {
                child->geometry.x = line_left;
                child->geometry.y = inner_y_abs + line_top_y;
                child->geometry.width = 0.0f;
                continue;
            }

            if (line_x > 0.0f && line_x + w > avail) {
                flush_line(/*forced=*/false);
                recompute_line_range();
                if (is_space) {
                    child->geometry.x = line_left;
                    child->geometry.y = inner_y_abs + line_top_y;
                    child->geometry.width = 0.0f;
                    continue;
                }
            }

            float cba = child->geometry.baseline_ascent;
            float cbd = child->geometry.baseline_descent;
            if (cba + cbd <= 0.0f) {
                cba = child->geometry.height;
                cbd = 0.0f;
            }

            style::VerticalAlign va = style::VerticalAlign::Baseline;
            if (child->style_soa_idx != UINT32_MAX) {
                va = storage.vertical_aligns[child->style_soa_idx];
            }

            line.push_back({ child.get(), w, cba, cbd, va });
            line_x += w;
            line_max_ascent = std::max(line_max_ascent, cba);
            line_max_descent = std::max(line_max_descent, cbd);
        }

        flush_line(/*forced=*/false);

        g.baseline_ascent = g.border_top + g.padding_top
            + line_top_y - last_line_descent;
        g.baseline_descent = g.padding_bottom + g.border_bottom
            + last_line_descent;

        g.width = g.border_left + g.padding_left + max_line_width
            + g.padding_right + g.border_right;
        g.height = g.border_top + g.padding_top + line_top_y
            + g.padding_bottom + g.border_bottom;
    }

    // =================================================================
//  Float (подэтап A).
//
//  Float выходит из нормального потока BFC:
//    * позиционируется у левого/правого края content-box BFC,
//    * несколько float'ов укладываются в ряд, если помещаются,
//    * y_cursor BFC НЕ сдвигается (родитель не растягивается).
//
//  Внутренности float'а — отдельный BFC (inner_fc). Родительский
//  FloatContext туда не проникает.
//
//  Координаты fx/fy — в системе content-box BFC (0,0 — верхний
//  левый угол content-области). Позиция ребёнка — border-box;
//  для margin-box left/top добавляем margin_left/margin_top.
//
//  TODO (B): обтекание текстом. Сейчас обычные блоки в BFC
//            не сужаются от float'ов — они идут на всю ширину.
//  TODO (C): clear: left/right/both. max_bottom() для autoheight
//            родителя через overflow: hidden.
// =================================================================
    static void layout_float(LayoutNode* child,
        float parent_width,
        const Viewport& vp,
        const StyleStorageSoA& storage,
        float root_fs,
        const TextMetrics& tm,
        FloatContext& fc,
        float cb_left, float cb_right,
        float bfc_y_cursor)
    {
        // Shrink-to-fit ТОЛЬКО для float без явной width.
        // Если width задан — override_content_width = -1, чтобы
        // layout_block_container взял width из CSS-спеки ребёнка.
        float override_w = -1.0f;
        if (child->style_soa_idx != UINT32_MAX) {
            const uint32_t si = child->style_soa_idx;
            if (storage.widths[si].is_auto() || storage.widths[si].is_none()) {
                resolve_box_model(child, storage, vp, parent_width, root_fs);
                const float intrinsic =
                    measure_intrinsic(child, storage, root_fs, tm).content_width;
                const float h_margin = child->geometry.margin_left
                    + child->geometry.margin_right;
                const float avail = std::max(0.0f, parent_width - h_margin);
                override_w = std::min(intrinsic, avail);
            }
        }

        // Float — сам устанавливает BFC → его внутренности не видят
        // родительский fc.
        layout_block_container(child, parent_width, vp, storage, root_fs, tm,
            /*parent_fc=*/nullptr, override_w);

        const auto& g = child->geometry;
        const float outer_w = g.margin_left + g.width + g.margin_right;
        const float outer_h = g.margin_top + g.height + g.margin_bottom;

        const bool is_left =
            (storage.floats[child->style_soa_idx] == style::Float::Left);

        float fy = bfc_y_cursor;
        float fx = 0.0f;
        constexpr int kMaxIterations = 64;
        for (int it = 0; it < kMaxIterations; ++it) {
            float avail_left = 0.0f, avail_right = 0.0f;
            fc.available_range(fy, cb_left, cb_right, avail_left, avail_right);

            if (is_left) {
                fx = avail_left;
                if (fx + outer_w <= avail_right) break;
            }
            else {
                fx = avail_right - outer_w;
                if (fx >= avail_left) break;
            }

            float next_y = fy + outer_h;
            for (const auto& f : fc.all()) {
                if (f.bottom > fy && f.bottom < next_y) next_y = f.bottom;
            }
            if (next_y <= fy) break;
            fy = next_y;
        }

        FloatContext::ActiveFloat af;
        af.left = fx;
        af.right = fx + outer_w;
        af.top = fy;
        af.bottom = fy + outer_h;
        af.is_left = is_left;
        fc.add(af);

        const float target_x = fx + g.margin_left;
        const float target_y = fy + g.margin_top;
        shift_subtree(child, target_x - child->geometry.x,
            target_y - child->geometry.y);
    }
    // =================================================================
    //  Block container — обычный BFC layout
    //
    //  FIX #2 (часть): новый параметр override_content_width. Если >= 0,
    //  он жёстко задаёт content-box ширину, минуя собственную
    //  `width` узла. Это нужно, чтобы flex-распределение не затиралось
    //  явной шириной ребёнка.
    // =================================================================
    static void layout_block_container(LayoutNode* node, float parent_width,
        const Viewport& vp,
        const StyleStorageSoA& storage,
        float root_fs,
        const TextMetrics& tm,
        FloatContext* parent_fc,
        float override_content_width = -1.0f,
        float extra_padding = 0.0f) // <--- ДОБАВЬ ЭТО
    {
        auto& g = node->geometry;
        const bool is_real = (node->type == BoxType::Block || node->type == BoxType::TableCell);

        if (is_real) resolve_box_model(node, storage, vp, parent_width, root_fs);

        // --- НОВОЕ: Применяем cellpadding ---
        if (is_real && extra_padding > 0.0f) {
            g.padding_top += extra_padding;
            g.padding_right += extra_padding;
            g.padding_bottom += extra_padding;
            g.padding_left += extra_padding;
        }

        const float h_extra = g.margin_left + g.margin_right
            + g.padding_left + g.padding_right
            + g.border_left + g.border_right;

        float content_width;
        if (override_content_width >= 0.0f) {
            content_width = override_content_width;
        }
        else {
            content_width = std::max(0.0f, parent_width - h_extra);
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
        }

        if (is_real && override_content_width < 0.0f) {
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

        const float inner_x = g.border_left + g.padding_left;
        const float inner_y = g.border_top + g.padding_top;

        // ---- Определяем fc, которым будут пользоваться дети ----
        //  * если этот блок устанавливает BFC (или вызван с parent_fc==nullptr
        //    — например, absolute, float) → свежий local_fc;
        //  * иначе — тот же fc, что и у родителя (float'ы «протекают»).
        FloatContext local_fc;
        FloatContext* child_fc = parent_fc;
        const bool is_bfc_root = (parent_fc == nullptr)
            || establishes_bfc(node, storage);
        if (is_bfc_root) child_fc = &local_fc;

        // CB для float'ов-детей — content-box ЭТОГО блока.
        const float cb_left = g.x + inner_x;
        const float cb_right = cb_left + content_width;

        // ---- margin collapse flags ----
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
            if (child->style_soa_idx != UINT32_MAX
                && storage.positions[child->style_soa_idx] == style::Position::Absolute)
            {
                resolve_box_model(child.get(), storage, vp, content_width, root_fs);
                child->geometry.flow_x = g.x + inner_x + child->geometry.margin_left;
                child->geometry.flow_y = g.y + inner_y + y_cursor;
                continue;
            }

            if (is_float(child.get(), storage)) {
                float float_y = g.y + inner_y + y_cursor;

                // clear на float'е — сдвигаем его вниз от активных float'ов.
                if (child->style_soa_idx != UINT32_MAX) {
                    const auto cl = storage.clears[child->style_soa_idx];
                    if (cl != style::Clear::None) {
                        const bool cl_left = (cl == style::Clear::Left
                            || cl == style::Clear::Both);
                        const bool cl_right = (cl == style::Clear::Right
                            || cl == style::Clear::Both);
                        float_y = child_fc->clear_y(cl_left, cl_right, float_y);
                    }
                }

                layout_float(child.get(), content_width, vp, storage, root_fs,
                    tm, *child_fc, cb_left, cb_right, float_y);
                continue;
            }

            resolve_box_model(child.get(), storage, vp, content_width, root_fs);
            const float cmt = child->geometry.margin_top;
            const float cmb = child->geometry.margin_bottom;
            const float cml = child->geometry.margin_left;
            const bool collapse_this_top = first_child && collapse_top_with_child;

            const float gap = first_child
                ? (collapse_this_top ? 0.0f : cmt)
                : collapse_margins(prev_bottom, cmt);

            float child_abs_y = g.y + inner_y + y_cursor + gap;
            float child_abs_x = g.x + inner_x + cml;
            float pass_parent_width = content_width;

            // clear: сдвигаем ребёнка вниз от активных float'ов до его
            // раскладки, чтобы layout_ifc внутри сразу знал правильный y.
            if (child->style_soa_idx != UINT32_MAX) {
                const auto cl = storage.clears[child->style_soa_idx];
                if (cl != style::Clear::None) {
                    const bool cl_left = (cl == style::Clear::Left
                        || cl == style::Clear::Both);
                    const bool cl_right = (cl == style::Clear::Right
                        || cl == style::Clear::Both);
                    child_abs_y = child_fc->clear_y(cl_left, cl_right, child_abs_y);
                }
            }

            // BFC-устанавливающие дети НЕ должны пересекаться с float'ами.
            // Обычные блоки — не сдвигаются (по спеке только их line boxes
            // сужаются, а это делает layout_ifc).
            if (establishes_bfc(child.get(), storage)) {
                float L = 0.0f, R = 0.0f;
                child_fc->available_range(child_abs_y, cb_left, cb_right, L, R);
                pass_parent_width = std::max(0.0f, R - L);
                child_abs_x = L + cml;
            }

            layout_node(child.get(), child_abs_x, child_abs_y,
                pass_parent_width, vp, storage, root_fs, tm, child_fc);

            const float ch = child->geometry.height;
            // y_cursor пересчитываем от РЕАЛЬНОЙ позиции ребёнка:
            // это автоматически учитывает сдвиг от clear.
            y_cursor = (child_abs_y - (g.y + inner_y)) + ch;
            prev_bottom = cmb;
            first_child = false;
        }

        if (collapse_top_with_child)
            g.margin_top = collapse_margins(g.margin_top,
                node->children.front()->geometry.margin_top);

        if (collapse_bottom_with_child)
            g.margin_bottom = collapse_margins(g.margin_bottom, prev_bottom);
        else
            y_cursor += prev_bottom;

        // ---- content_height ----
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

        // BFC-root растягивается под свои float'ы (§10.6.7).
        if (is_real && is_bfc_root) {
            const float fb = local_fc.max_bottom();
            if (fb > 0.0f) {
                const float rel = fb - (g.y + inner_y);
                content_height = std::max(content_height, rel);
            }
        }

        g.width = g.border_left + g.padding_left + content_width
            + g.padding_right + g.border_right;
        g.height = g.border_top + g.padding_top + content_height
            + g.padding_bottom + g.border_bottom;
    }

    // =================================================================
    //  Inline element (<span>)
    // =================================================================
    static void layout_inline(LayoutNode* node, float parent_width,
        const Viewport& vp,
        const StyleStorageSoA& storage,
        float root_fs,
        const TextMetrics& tm,
        FloatContext* parent_fc)
    {
        resolve_box_model(node, storage, vp, parent_width, root_fs);
        layout_ifc(node, parent_width, vp, storage, root_fs, tm, parent_fc);

        auto& g = node->geometry;
        const uint32_t si = node->style_soa_idx;
        const float fs = storage.font_sizes[si];
        const bool border_box = (storage.box_sizings[si] == BoxSizing::BorderBox);
        const float pb_h = g.padding_left + g.padding_right
            + g.border_left + g.border_right;
        const float pb_v = g.padding_top + g.padding_bottom
            + g.border_top + g.border_bottom;

        const style::Length& w = storage.widths[si];
        if (!w.is_auto() && !w.is_none()) {
            float spec = resolve_px(w, fs, root_fs, vp, parent_width);
            if (border_box) spec -= pb_h;
            g.width = pb_h + std::max(0.0f, spec);
        }

        const style::Length& h = storage.heights[si];
        if (!h.is_auto() && !h.is_none()) {
            float spec = resolve_px(h, fs, root_fs, vp, parent_width);
            if (border_box) spec -= pb_v;
            g.height = pb_v + std::max(0.0f, spec);
        }

        const style::Length& maxw = storage.max_widths[si];
        if (!maxw.is_none()) {
            float mw = resolve_px(maxw, fs, root_fs, vp, parent_width);
            if (border_box) mw -= pb_h;
            if (mw > 0.0f) {
                const float content_w = std::min(g.width - pb_h, mw);
                g.width = pb_h + std::max(0.0f, content_w);
            }
        }

        const style::Length& minw = storage.min_widths[si];
        if (!minw.is_none()) {
            float mw = resolve_px(minw, fs, root_fs, vp, parent_width);
            if (border_box) mw -= pb_h;
            if (mw > 0.0f) {
                const float content_w = std::max(g.width - pb_h, mw);
                g.width = pb_h + content_w;
            }
        }

        // Если span получил явную высоту, но baseline не был установлен
// layout_ifc'ом (например, у него пустое содержимое) —
// прижимаем baseline к низу бокса.
        if (g.baseline_ascent + g.baseline_descent <= 0.0f) {
            g.baseline_ascent = g.height;
            g.baseline_descent = 0.0f;
        }
    }

    // =================================================================
    //  Text — атомарный узел: одно слово или один пробел.
    // =================================================================
    static void layout_text(LayoutNode* node, float /*parent_width*/,
        const Viewport& /*vp*/,
        const StyleStorageSoA& storage,
        float /*root_fs*/,
        const TextMetrics& tm)
    {
        const uint32_t si = effective_style_idx(node);
        const float fs = storage.font_sizes[si];
        const std::string& family = storage.font_families[si];
        const bool bold = storage.font_weights[si] >= 700;
        const auto fstyle = storage.font_styles[si];
        const bool italic = fstyle == style::FontStyle::Italic
            || fstyle == style::FontStyle::Oblique;

        float lh;
        const float raw_lh = storage.line_heights[si];
        if (raw_lh == 0.0f && tm.hasHeight()) {
            lh = tm.line_spacing(fs,family, bold);
        }
        else {
            lh = style::resolve_line_height(storage, si);
        }

        float text_w;
        if (tm.hasWidth()) {
            text_w = tm.measure_width(node->text_content, fs, family, bold, italic);
        }
        else {
            text_w = node->text_content.size() * std::max(1.0f, fs * 0.55f);
        }

        // TODO: for non-ASCII text, size() is not the codepoint count.
        text_w += storage.letter_spacings[si]
            * static_cast<float>(node->text_content.size());

        node->geometry.width = text_w;
        node->geometry.height = lh;

        // ---- baseline ----
        // ascent/descent берём из метрик (или эвристики 0.8/0.2 от fs).
        // Разницу line-height и (ascent+descent) распределяем как
        // half-leading сверху и снизу — CSS 2.1 §10.8.
        float asc, desc;
        if (tm.hasBaseline()) {
            asc = tm.ascent(fs, family, bold);
            desc = tm.descent(fs, family, bold);
        }
        else {
            asc = fs * 0.8f;
            desc = fs * 0.2f;
        }
        const float half_leading = std::max(0.0f, (lh - asc - desc) * 0.5f);
        node->geometry.baseline_ascent = half_leading + asc;
        node->geometry.baseline_descent = half_leading + desc;
    }
    // =================================================================
    //  Image (replaced element).
    //
    //  1) оба width/height заданы — используем их;
    //  2) только width — height = width * (ih / iw);
    //  3) только height — width = height / (ih / iw);
    //  4) оба auto — intrinsic size (или 0×0, если файл не найден).
    //
    //  TODO: max-width: 100%, object-fit, aspect-ratio.
    // =================================================================
    static void layout_image(LayoutNode* node, float parent_width,
        const Viewport& vp,
        const StyleStorageSoA& storage,
        float root_fs)
    {
        resolve_box_model(node, storage, vp, parent_width, root_fs);

        auto& g = node->geometry;
        const uint32_t si = node->style_soa_idx;
        const float fs = storage.font_sizes[si];
        const bool border_box = (storage.box_sizings[si] == BoxSizing::BorderBox);
        const float pb_h = g.padding_left + g.padding_right
            + g.border_left + g.border_right;
        const float pb_v = g.padding_top + g.padding_bottom
            + g.border_top + g.border_bottom;

        const style::Length& w_spec = storage.widths[si];
        const style::Length& h_spec = storage.heights[si];
        const bool has_w = !w_spec.is_auto() && !w_spec.is_none();
        const bool has_h = !h_spec.is_auto() && !h_spec.is_none();

        const float iw = node->intrinsic_width;
        const float ih = node->intrinsic_height;
        const bool has_intrinsic = (iw > 0.0f && ih > 0.0f);
        const float ratio = has_intrinsic ? (ih / iw) : 0.0f;

        float content_w = 0.0f;
        float content_h = 0.0f;

        if (has_w && has_h) {
            content_w = std::max(0.0f, resolve_px(w_spec, fs, root_fs, vp, parent_width)
                - (border_box ? pb_h : 0.0f));
            content_h = std::max(0.0f, resolve_px(h_spec, fs, root_fs, vp, parent_width)
                - (border_box ? pb_v : 0.0f));
        }
        else if (has_w) {
            content_w = std::max(0.0f, resolve_px(w_spec, fs, root_fs, vp, parent_width)
                - (border_box ? pb_h : 0.0f));
            content_h = has_intrinsic ? content_w * ratio : 0.0f;
        }
        else if (has_h) {
            content_h = std::max(0.0f, resolve_px(h_spec, fs, root_fs, vp, parent_width)
                - (border_box ? pb_v : 0.0f));
            content_w = has_intrinsic ? content_h / ratio : 0.0f;
        }
        else {
            content_w = iw;
            content_h = ih;
            if (border_box) {
                content_w = std::max(0.0f, content_w - pb_h);
                content_h = std::max(0.0f, content_h - pb_v);
            }
        }

        // ---- max-width / max-height ----
 // По CSS 2.1 §10.4 для replaced elements: применяем последовательно,
 // сохраняя пропорции. Если оба ограничения срабатывают — берём
 // меньшее масштабирование (второй шаг может ещё уменьшить размер).

        const style::Length& maxw_spec = storage.max_widths[si];
        if (!maxw_spec.is_none() && content_w > 0.0f) {
            float mw = resolve_px(maxw_spec, fs, root_fs, vp, parent_width);
            if (border_box) mw -= pb_h;
            if (mw > 0.0f && content_w > mw) {
                const float scale = mw / content_w;
                content_w = mw;
                content_h *= scale;
            }
        }

        const style::Length& maxh_spec = storage.max_heights[si];
        if (!maxh_spec.is_none() && content_h > 0.0f) {
            float mh = resolve_px(maxh_spec, fs, root_fs, vp, parent_width);
            if (border_box) mh -= pb_v;
            if (mh > 0.0f && content_h > mh) {
                const float scale = mh / content_h;
                content_h = mh;
                content_w *= scale;
            }
        }

        g.width = pb_h + content_w;
        g.height = pb_v + content_h;

        // Baseline replaced element — на нижней грани content-box
        // (CSS 2.1 §10.8.1).
        g.baseline_ascent = g.height;
        g.baseline_descent = 0.0f;
    }
    // Идёт после основного layout-прохода и пере-выставляет y блочных детей
// согласно фактическим margin'ам (которые могли измениться из-за
// margin-collapse уже ПОСЛЕ того как родитель их расставил).
// Top-down: родитель сначала выравнивает своих детей через shift_subtree,
// затем рекурсия идёт по уже сдвинутым детям.
    static void fixup_block_positions(LayoutNode* node,
        const StyleStorageSoA& storage)
    {
        if (!node) return;

        if (node->type == BoxType::Block && !is_flex_container(node, storage)) {
            const float content_top = node->geometry.y
                + node->geometry.border_top + node->geometry.padding_top;

            const bool top_open = node->geometry.border_top == 0.0f
                && node->geometry.padding_top == 0.0f;
            const bool is_root = (node->parent == nullptr);

            float cursor = content_top;
            float prev_mb = 0.0f;
            bool  first = true;

            for (auto& child : node->children) {
                if (child->style_soa_idx != UINT32_MAX
                    && storage.positions[child->style_soa_idx] == style::Position::Absolute)
                    continue;
                if (is_float(child.get(), storage)) continue;
                if (!child->is_block_level()) continue;

                const float ctm = child->geometry.margin_top;
                const float cbm = child->geometry.margin_bottom;

                float desired_y;
                if (first) {
                    // Схлопывание верхнего margin'а родителя с первым
                    // блочным ребёнком: если у родителя нет border/padding
                    // сверху — ребёнок встаёт на сам border-box родителя.
                    if (top_open && !is_root) desired_y = node->geometry.y;
                    else                      desired_y = content_top + ctm;
                }
                else {
                    desired_y = cursor + collapse_margins(prev_mb, ctm);
                }

                // Если ребёнок сдвинут через clear — не двигаем его вверх,
                // иначе clear сломается. Вниз — можно (rare edge case:
                // margin collapse съел больше, чем ожидалось).
                const bool is_cleared = child->style_soa_idx != UINT32_MAX
                    && storage.clears[child->style_soa_idx] != style::Clear::None;

                float dy = desired_y - child->geometry.y;
                if (is_cleared && dy < 0.0f) dy = 0.0f;

                if (std::abs(dy) > 0.001f)
                    shift_subtree(child.get(), 0.0f, dy);

                cursor = child->geometry.y + child->geometry.height;
                prev_mb = cbm;
                first = false;
            }
        }

        for (auto& c : node->children)
            fixup_block_positions(c.get(), storage);
    }
};

