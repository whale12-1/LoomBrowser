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
            bool  bold,
            bool  italic)> measure_width;

        std::function<float(float font_size,
            bool  bold)> line_spacing;

        // Метрики для baseline alignment: расстояние от верха глифов
        // до базовой линии и от базовой линии до низа глифов.
        // Опциональны: если не заданы — работает эвристика 0.8 / 0.2 от fs.
        std::function<float(float font_size, bool bold)> ascent;
        std::function<float(float font_size, bool bold)> descent;

        bool hasWidth()    const { return static_cast<bool>(measure_width); }
        bool hasHeight()   const { return static_cast<bool>(line_spacing); }
        bool hasBaseline() const {
            return static_cast<bool>(ascent)
                && static_cast<bool>(descent);
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

        // 1. Flow-раскладка.
                // 1. Flow-раскладка.
        FloatContext root_fc;
        layout_node(root, viewport.width, viewport, storage, root_fs, metrics,
            root_fc);

        // 2. Пост-проход: сохранить flow_x/flow_y и применить relative.
        apply_position_offsets(root, viewport, storage, root_fs);

        // 3. Раскладка absolute-потомков.
        layout_absolute_descendants(root, viewport, storage, root_fs, metrics);
    }

private:
    struct IntrinsicSize {
        float content_width = 0.0f;
        float content_height = 0.0f;
    };

    // Измеряет max-content размер узла без изменения его geometry.
    static IntrinsicSize measure_intrinsic(const LayoutNode* node,
        const StyleStorageSoA& storage,
        float root_fs,
        const TextMetrics& tm)
    {
        if (!node) return {};

        const uint32_t si = effective_style_idx(node);
        const float fs = storage.font_sizes[si];
        const bool bold = storage.font_weights[si] >= 700;
        const auto fstyle = storage.font_styles[si];
        const bool italic = (fstyle == style::FontStyle::Italic
            || fstyle == style::FontStyle::Oblique);
        const Viewport vp{};
        const auto line_height = [&]() {
            if (storage.line_heights[si] == 0.0f && tm.hasHeight())
                return tm.line_spacing(fs, bold);
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
                ? tm.measure_width(node->text_content, fs, bold, italic)
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
        if (tag == "img") {
            node->type = BoxType::Image;

            std::string src;
            for (const auto& kv : dom->attributes) {
                std::string_view k(kv.first.data(), kv.first.size());
                if (k == "src") {
                    src.assign(kv.second.data(), kv.second.size());
                    break;
                }
            }
            node->image_src = src;

            if (!src.empty() && img_metrics.hasIntrinsic()) {
                auto [w, h] = img_metrics.intrinsic_size(src);
                node->intrinsic_width = w;
                node->intrinsic_height = h;
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
        const TextMetrics& tm,
        FloatContext& fc)
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

        // -------- Pass 3: layout каждого item с финальной шириной --------
        float max_cross_outer = 0.0f;
        for (auto& it : items) {
            const float pb_h = it.node->geometry.padding_left
                + it.node->geometry.padding_right
                + it.node->geometry.border_left
                + it.node->geometry.border_right;
            const float override_content_w = std::max(0.0f, it.outer_main - pb_h);

            layout_node(it.node, content_width, vp, storage, root_fs, tm,
                fc, override_content_w);

            it.cross_outer = it.node->geometry.height
                + it.node->geometry.margin_top
                + it.node->geometry.margin_bottom;
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

            child->geometry.x = inner_x + x_cursor + child->geometry.margin_left;
            child->geometry.y = inner_y + cross_offset + child->geometry.margin_top;

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


    // Flex-контейнер — новый путь в диспетчере.
    // Не добавляем BoxType::Flex: flex — это режим layout,
    // а не тип бокса. Внешне контейнер остаётся Block.
    static void layout_node(LayoutNode* node, float parent_width,
        const Viewport& vp,
        const StyleStorageSoA& storage,
        float root_fs,
        const TextMetrics& tm,
        FloatContext& fc,
        float override_content_width = -1.0f)
    {
        if (is_flex_container(node, storage)) {
            layout_flex_container(node, parent_width, vp, storage, root_fs,
                tm, fc);
            return;
        }

        switch (node->type) {
        case BoxType::AnonymousBlock:
            layout_ifc(node, parent_width, vp, storage, root_fs, tm, fc);
            node->geometry.width = parent_width;
            break;
        case BoxType::Block:
            layout_block_container(node, parent_width, vp, storage, root_fs,
                tm, fc, override_content_width);
            break;
        case BoxType::Inline:
            layout_inline(node, parent_width, vp, storage, root_fs, tm, fc);
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

        const float offset_x = !left_inset.is_auto() ? left
            : (!right_inset.is_auto() ? -right : 0.0f);
        const float offset_y = !top_inset.is_auto() ? top
            : (!bottom_inset.is_auto() ? -bottom : 0.0f);

        node->geometry.x += offset_x;
        node->geometry.y += offset_y;
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
            layout_absolute_descendants(child, vp, storage, root_fs, tm);
        }

        for (auto& child : cb->children) {
            layout_absolute_descendants(child.get(), vp, storage, root_fs, tm);
        }
    }

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

        float computed_height = node->geometry.height;
        bool height_from_constraints = false;
        if (!height_auto) {
            computed_height = resolve_px(height_spec, fs, root_fs, vp, cb_content_width);
            height_from_constraints = true;
        }
        else if (has_top && has_bottom && cb_content_height >= 0.0f) {
            computed_height = std::max(0.0f, cb_content_height - top - bottom);
            height_from_constraints = true;
        }

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

        // У absolute-элемента свой BFC — внутренний FloatContext
// не связан с родительским.
        FloatContext abs_fc;
        layout_block_container(node, computed_width, vp, storage, root_fs,
            tm, abs_fc);

        node->geometry.x = x;
        node->geometry.y = y;
        node->geometry.width = computed_width;
        if (height_from_constraints) {
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
        FloatContext& fc)
    {
        // fc не используется в подэтапе A. Обтекание текста — подэтап B.
        (void)fc;
        auto& g = node->geometry;

        const float inner_x = g.border_left + g.padding_left;
        const float inner_y = g.border_top + g.padding_top;

        const float h_extra = g.margin_left + g.margin_right
            + g.padding_left + g.padding_right
            + g.border_left + g.border_right;
        const float avail = std::max(0.0f, parent_width - h_extra);

        const uint32_t si_align = effective_style_idx(node);
        const style::TextAlign align = storage.text_aligns[si_align];

        // ---- Strut контейнера ----
        // Невидимая "буква" родителя участвует в формировании line box
        // (CSS 2.1 §10.8). Иначе <div style="font-size:32px"><span
        // style="font-size:12px">x</span></div> схлопнется по высоте
        // 12-пиксельного span'а.
        const float strut_fs = storage.font_sizes[si_align];
        const bool  strut_bold = storage.font_weights[si_align] >= 700;
        const float strut_lh = style::resolve_line_height(storage, si_align);
        float strut_asc, strut_desc;
        if (tm.hasBaseline()) {
            strut_asc = tm.ascent(strut_fs, strut_bold);
            strut_desc = tm.descent(strut_fs, strut_bold);
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
            style::VerticalAlign va;   // vertical-align для y-позиции
        };
        std::vector<LineItem> line;

        float line_x = 0.0f;             // накопленная ширина строки
        float line_top_y = 0.0f;         // верх текущей строки, от content-top
        float line_max_ascent = 0.0f;
        float line_max_descent = 0.0f;
        float max_line_width = 0.0f;

        // Baseline последней строки — нужен для g.baseline_ascent.
        float last_line_ascent = 0.0f;
        float last_line_descent = 0.0f;

        // Закрывает текущую строку. forced=true — даже если line пуст
        // (для <br>). forced=false — no-op если line пуст.
        auto flush_line = [&](bool forced) {
            if (line.empty() && !forced) return;

            // Строка содержит максимум из детей и strut.
            const float ma = std::max(line_max_ascent, strut_ascent);
            const float md = std::max(line_max_descent, strut_descent);
            const float line_h = ma + md;

            // text-align (по x).
            float offset = 0.0f;
            bool handled = false;

            if (align == style::TextAlign::Right
                || align == style::TextAlign::End) {
                offset = avail - line_x;
            }
            else if (align == style::TextAlign::Center) {
                offset = (avail - line_x) * 0.5f;
            }
            else if (align == style::TextAlign::Justify && !forced) {
                // Justify — только для не-последних строк. Признак
                // последней — forced=true (сюда приходит только <br>
                // и конец IFC; конец IFC шлёт forced=true).
                const float extra = avail - line_x;
                if (extra > 0.0f) {
                    int spaces = 0;
                    for (const auto& it : line) {
                        if (it.node->type == BoxType::Text
                            && it.node->text_content == " ") ++spaces;
                    }
                    if (spaces > 0) {
                        const float per_space = extra / float(spaces);
                        float acc = 0.0f;
                        for (auto& it : line) {
                            it.node->geometry.x += inner_x + acc;
                            // y — по vertical-align ребёнка.
                            {
                                const float baseline_y = line_top_y + ma;
                                float y;
                                switch (it.va) {
                                case style::VerticalAlign::Top:
                                    y = line_top_y + it.node->geometry.margin_top;
                                    break;
                                case style::VerticalAlign::Bottom:
                                    y = line_top_y + line_h
                                        - it.node->geometry.height
                                        - it.node->geometry.margin_bottom;
                                    break;
                                case style::VerticalAlign::Middle: {
                                    const float x_half = strut_fs * 0.25f;
                                    y = baseline_y - x_half
                                        - it.node->geometry.height * 0.5f;
                                    break;
                                }
                                case style::VerticalAlign::TextTop:
                                    y = baseline_y - strut_asc;
                                    break;
                                case style::VerticalAlign::TextBottom:
                                    y = baseline_y + strut_desc
                                        - it.node->geometry.height;
                                    break;
                                default: // Baseline
                                    y = baseline_y - it.ascent;
                                    break;
                                }
                                it.node->geometry.y = inner_y + y;
                            }
                            if (it.node->type == BoxType::Text
                                && it.node->text_content == " ") {
                                it.node->geometry.width += per_space;
                                acc += per_space;
                            }
                        }
                        handled = true;
                    }
                }
            }

            if (!handled) {
                if (offset < 0.0f) offset = 0.0f;
                for (auto& it : line) {
                    it.node->geometry.x += inner_x + offset;
                    const float baseline_y = line_top_y + ma;
                    float y;
                    switch (it.va) {
                    case style::VerticalAlign::Top:
                        y = line_top_y + it.node->geometry.margin_top;
                        break;
                    case style::VerticalAlign::Bottom:
                        y = line_top_y + line_h
                            - it.node->geometry.height
                            - it.node->geometry.margin_bottom;
                        break;
                    case style::VerticalAlign::Middle: {
                        const float x_half = strut_fs * 0.25f;
                        y = baseline_y - x_half
                            - it.node->geometry.height * 0.5f;
                        break;
                    }
                    case style::VerticalAlign::TextTop:
                        y = baseline_y - strut_asc;
                        break;
                    case style::VerticalAlign::TextBottom:
                        y = baseline_y + strut_desc
                            - it.node->geometry.height;
                        break;
                    default: // Baseline
                        y = baseline_y - it.ascent;
                        break;
                    }
                    it.node->geometry.y = inner_y + y;
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
            layout_node(child.get(), avail, vp, storage, root_fs, tm,fc);

            if (is_br(child.get(), storage)) {
                // <br> закрывает строку. Даже пустую — с высотой strut.
                flush_line(/*forced=*/true);
                child->geometry.x = inner_x;
                child->geometry.y = inner_y + line_top_y;
                child->geometry.width = 0.0f;
                child->geometry.height = 0.0f;
                continue;
            }

            const float w = child->geometry.width
                + child->geometry.margin_left + child->geometry.margin_right;

            const bool is_space = (child->type == BoxType::Text
                && child->text_content == " ");

            // Пробел в начале строки схлопывается.
            if (is_space && line_x == 0.0f) {
                child->geometry.x = inner_x;
                child->geometry.y = inner_y + line_top_y;
                child->geometry.width = 0.0f;
                continue;
            }

            // Переполнение — переносим на новую строку.
            if (line_x > 0.0f && line_x + w > avail) {
                flush_line(/*forced=*/false);
                if (is_space) {
                    child->geometry.x = inner_x;
                    child->geometry.y = inner_y + line_top_y;
                    child->geometry.width = 0.0f;
                    continue;
                }
            }

            // baseline ребёнка (из layout_text / layout_image /
            // вложенного layout_inline → layout_ifc).
            float cba = child->geometry.baseline_ascent;
            float cbd = child->geometry.baseline_descent;
            if (cba + cbd <= 0.0f) {
                // Fallback: считаем, что baseline на нижней грани.
                cba = child->geometry.height;
                cbd = 0.0f;
            }

            // x — временный, окончательный сдвиг делает flush_line.
            child->geometry.x = line_x + child->geometry.margin_left;
            // y — тоже временный; уточняется при flush.
            child->geometry.y = inner_y + line_top_y;

            style::VerticalAlign va = style::VerticalAlign::Baseline;
            if (child->style_soa_idx != UINT32_MAX) {
                va = storage.vertical_aligns[child->style_soa_idx];
            }
            line.push_back({ child.get(), w, cba, cbd, va });
            line_x += w;
            line_max_ascent = std::max(line_max_ascent, cba);
            line_max_descent = std::max(line_max_descent, cbd);
        }

        // Закрываем последнюю строку — если есть содержимое.
        // Пустая IFC (без детей) сохраняет height = 0.
        flush_line(/*forced=*/false);

        // Baseline самого IFC = baseline последней строки.
        // Из content-top: line_top_y - last_line_descent
        // Из border-box top: + border_top + padding_top
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
        float inner_x, float inner_y,
        float bfc_y_cursor)
    {
        // Внутренний BFC float'а.
        FloatContext inner_fc;
        layout_block_container(child, parent_width, vp, storage, root_fs, tm,
            inner_fc);

        const auto& g = child->geometry;
        const float outer_w = g.margin_left + g.width + g.margin_right;
        const float outer_h = g.margin_top + g.height + g.margin_bottom;

        const bool is_left =
            (storage.floats[child->style_soa_idx] == style::Float::Left);

        // Ищем свободный слот на плоскости BFC.
        float fy = bfc_y_cursor;
        float fx = 0.0f;
        constexpr int kMaxIterations = 64;
        for (int it = 0; it < kMaxIterations; ++it) {
            float avail_left = 0.0f;
            float avail_right = parent_width;
            fc.available_range(fy, parent_width, avail_left, avail_right);

            if (is_left) {
                fx = avail_left;
                if (fx + outer_w <= avail_right) break;
            }
            else {
                fx = avail_right - outer_w;
                if (fx >= avail_left) break;
            }

            // Не влезает — минимальный bottom активного float'а, > fy.
            float next_y = fy + outer_h;
            for (const auto& f : fc.all()) {
                if (f.bottom > fy && f.bottom < next_y) next_y = f.bottom;
            }
            if (next_y <= fy) break;   // защита от бесконечного цикла
            fy = next_y;
        }

        // Регистрируем во FloatContext.
        FloatContext::ActiveFloat af;
        af.left = fx;
        af.right = fx + outer_w;
        af.top = fy;
        af.bottom = fy + outer_h;
        af.is_left = is_left;
        fc.add(af);

        // Позиция ребёнка в системе border-box родителя.
        // fx/fy — левый-верхний угол margin-box float'а.
        child->geometry.x = inner_x + fx + g.margin_left;
        child->geometry.y = inner_y + fy + g.margin_top;

        // y_cursor родителя НЕ увеличивается.
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
        FloatContext& fc,
        float override_content_width = -1.0f)
    {
        auto& g = node->geometry;
        const bool is_real = (node->type == BoxType::Block);

        if (is_real) resolve_box_model(node, storage, vp, parent_width, root_fs);

        const float h_extra = g.margin_left + g.margin_right
            + g.padding_left + g.padding_right
            + g.border_left + g.border_right;

        // -------- content_width --------
        float content_width;

        if (override_content_width >= 0.0f) {
            // Flex override: жёстко фиксируем content-box.
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

        // -------- auto-margin (горизонтальный) --------
        // Применяем только когда нет flex-override: во flex auto-margins
        // имеют другое значение (поглощают свободное место вдоль оси).
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

        // -------- дети --------
        const float inner_x = g.border_left + g.padding_left;
        const float inner_y = g.border_top + g.padding_top;

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
                && storage.positions[child->style_soa_idx] == style::Position::Absolute) {
                child->geometry.flow_x = inner_x + child->geometry.margin_left;
                child->geometry.flow_y = inner_y + y_cursor;
                continue;
            }

            // Float: раскладываем в FloatContext, y_cursor не двигаем.
            if (is_float(child.get(), storage)) {
                layout_float(child.get(), content_width, vp, storage, root_fs,
                    tm, fc, inner_x, inner_y, y_cursor);
                continue;
            }

            layout_node(child.get(), content_width, vp, storage, root_fs, tm,
                fc);

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
    // =================================================================
    static void layout_inline(LayoutNode* node, float parent_width,
        const Viewport& vp,
        const StyleStorageSoA& storage,
        float root_fs,
        const TextMetrics& tm,
        FloatContext& fc)
    {
        resolve_box_model(node, storage, vp, parent_width, root_fs);

        layout_ifc(node, parent_width, vp, storage, root_fs, tm, fc);

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
        const bool bold = storage.font_weights[si] >= 700;
        const auto fstyle = storage.font_styles[si];
        const bool italic = fstyle == style::FontStyle::Italic
            || fstyle == style::FontStyle::Oblique;

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
            text_w = tm.measure_width(node->text_content, fs, bold, italic);
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
            asc = tm.ascent(fs, bold);
            desc = tm.descent(fs, bold);
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
};

