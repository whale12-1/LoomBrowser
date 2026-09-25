#pragma once
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdlib>
#include <cmath>
#include "style_storage_soa.h"
#include "selector_matcher.h"

// ============================================================
//  Индекс правил по «правому краю» complex-селектора.
//  Позволяет не проверять ВСЕ правила для каждого узла, а лишь те,
//  чей последний compound содержит #id, .class, tag или *.
//
//  Это ключевая оптимизация: O(N * M) → O(N * k), где k << M.
// ============================================================
class StyleRuleIndex {
public:
    // Индексы вместо указателей — невосприимчиво к реаллокации rules/selectors.
    struct Entry {
        uint32_t    rule_index = 0;   // в sheet_.rules
        uint32_t    selector_index = 0;   // в rule.selectors
        Specificity spec;
        uint32_t    order = 0;            // глобальный порядок объявления
    };

    explicit StyleRuleIndex(const StyleSheet& sheet) : sheet_(sheet) {}

    void build() {
        id_buckets_.clear();
        class_buckets_.clear();
        tag_buckets_.clear();
        universal_bucket_.clear();
        entries_.clear();

        // Точная оценка сверху — гарантирует отсутствие реаллокаций entries_.
        size_t total = 0;
        for (const auto& r : sheet_.rules) total += r.selectors.size();
        entries_.reserve(total);

        uint32_t order = 0;
        for (uint32_t ri = 0; ri < sheet_.rules.size(); ++ri) {
            const CSSRule& rule = sheet_.rules[ri];
            for (uint32_t si = 0; si < rule.selectors.size(); ++si) {
                const ComplexSelector& sel = rule.selectors[si];
                if (sel.compounds.empty()) continue;

                Entry e;
                e.rule_index = ri;
                e.selector_index = si;
                e.spec = SelectorMatcher::compute_specificity(sel);
                e.order = order++;

                const uint32_t eidx = static_cast<uint32_t>(entries_.size());
                entries_.push_back(e);
                index_entry(eidx, sel);
            }
        }
    }

    // out НЕ очищается — caller сам решает (scratch-паттерн).
    void collect(const DOMNode* node, std::vector<uint32_t>& out) const {
        auto it_id = node->attributes.find("id");
        if (it_id != node->attributes.end()) {
            auto b = id_buckets_.find(it_id->second);
            if (b != id_buckets_.end())
                out.insert(out.end(), b->second.begin(), b->second.end());
        }

        auto it_cls = node->attributes.find("class");
        if (it_cls != node->attributes.end()) {
            const std::string& s = it_cls->second;
            size_t i = 0, n = s.size();
            while (i < n) {
                while (i < n && std::isspace((unsigned char)s[i])) ++i;
                size_t st = i;
                while (i < n && !std::isspace((unsigned char)s[i])) ++i;
                if (i > st) {
                    auto b = class_buckets_.find(s.substr(st, i - st));
                    if (b != class_buckets_.end())
                        out.insert(out.end(), b->second.begin(), b->second.end());
                }
            }
        }

        auto bt = tag_buckets_.find(node->tag_name);
        if (bt != tag_buckets_.end())
            out.insert(out.end(), bt->second.begin(), bt->second.end());

        out.insert(out.end(), universal_bucket_.begin(), universal_bucket_.end());
    }

    // --- Резолверы по индексам ---
    const Entry& entry(uint32_t eidx) const { return entries_[eidx]; }
    const CSSRule& rule(uint32_t ri)   const { return sheet_.rules[ri]; }
    const ComplexSelector& selector(uint32_t ri, uint32_t si) const {
        return sheet_.rules[ri].selectors[si];
    }
    const Declaration& declaration(const Entry& e, uint32_t di) const {
        return sheet_.rules[e.rule_index].declarations[di];
    }
    const StyleSheet& sheet() const { return sheet_; }

private:
    void index_entry(uint32_t eidx, const ComplexSelector& sel) {
        const CompoundSelector& right = sel.compounds.back();
        bool indexed = false;

        for (const auto& s : right.parts) {
            if (s.kind == SimpleSelector::Kind::Id) {
                id_buckets_[s.name].push_back(eidx);
                indexed = true; break;
            }
        }
        if (!indexed) for (const auto& s : right.parts) {
            if (s.kind == SimpleSelector::Kind::Class) {
                class_buckets_[s.name].push_back(eidx);
                indexed = true; break;
            }
        }
        if (!indexed) for (const auto& s : right.parts) {
            if (s.kind == SimpleSelector::Kind::Tag) {
                tag_buckets_[s.name].push_back(eidx);
                indexed = true; break;
            }
        }
        if (!indexed) universal_bucket_.push_back(eidx);
    }

    const StyleSheet& sheet_;
    std::unordered_map<std::string, std::vector<uint32_t>> id_buckets_;
    std::unordered_map<std::string, std::vector<uint32_t>> class_buckets_;
    std::unordered_map<std::string, std::vector<uint32_t>> tag_buckets_;
    std::vector<uint32_t> universal_bucket_;
    std::vector<Entry>    entries_;
};

// ============================================================
//  Builder
// ============================================================
class StyleTreeBuilder {
public:
    static StyleStorageSoA build(DOMNode* dom_root, const StyleSheet& sheet) {
        StyleStorageSoA storage;
        storage.dom_nodes.reserve(256);
        if (!dom_root) return storage;

        StyleRuleIndex index(sheet);
        index.build();

        std::vector<uint32_t> scratch;
        scratch.reserve(64);

        dfs_build(dom_root, INVALID_INDEX, index, scratch, storage);
        return storage;
    }

private:
    // -------------------------------------------------------------
    //  DFS
    // -------------------------------------------------------------
    static uint32_t dfs_build(DOMNode* node, uint32_t parent_idx,
        const StyleRuleIndex& index,
        std::vector<uint32_t>& scratch,
        StyleStorageSoA& storage)
    {
        if (!node || node->type != NodeType::Element) return INVALID_INDEX;

        const uint32_t idx = storage.allocate_node(node, parent_idx);

        compute_style(node, idx, parent_idx, index, scratch, storage);

        uint32_t prev_child = INVALID_INDEX;
        for (DOMNode* child : node->children) {
            uint32_t cidx = dfs_build(child, idx, index, scratch, storage);
            if (cidx != INVALID_INDEX) {
                if (storage.first_child_indices[idx] == INVALID_INDEX) {
                    storage.first_child_indices[idx] = cidx;
                }
                if (prev_child != INVALID_INDEX) {
                    storage.next_sibling_indices[prev_child] = cidx;
                }
                storage.last_child_indices[idx] = cidx;
                prev_child = cidx;
            }
        }
        return idx;
    }

    // -------------------------------------------------------------
    //  Каскад: собрать matched declarations, разрешить конфликты
    // -------------------------------------------------------------
    struct Winner {
        uint32_t    rule_index = UINT32_MAX;   // UINT32_MAX = «нет победителя»
        uint32_t    decl_index = 0;
        Specificity spec;
        uint32_t    order = 0;
        bool        important = false;

        bool empty() const { return rule_index == UINT32_MAX; }

        bool dominates(const Winner& o) const {
            if (important != o.important) return important;
            if (spec.a != o.spec.a) return spec.a > o.spec.a;
            if (spec.b != o.spec.b) return spec.b > o.spec.b;
            if (spec.c != o.spec.c) return spec.c > o.spec.c;
            return order > o.order;
        }
    };

    static void compute_style(DOMNode* node, uint32_t idx, uint32_t parent_idx,
        const StyleRuleIndex& index,
        std::vector<uint32_t>& scratch,
        StyleStorageSoA& storage)
    {
        // 1. Сначала наследуем значения от родителя (чтобы parent_fs был актуален!)
        if (parent_idx != INVALID_INDEX)
            inherit_from_parent(idx, parent_idx, storage);

        scratch.clear();
        index.collect(node, scratch);

        Winner winners[style::PROP_COUNT]{};

        for (uint32_t eidx : scratch) {
            const auto& entry = index.entry(eidx);
            const ComplexSelector& sel = index.selector(entry.rule_index, entry.selector_index);

            if (!SelectorMatcher::match_complex(node, sel)) continue;

            const CSSRule& rule = index.rule(entry.rule_index);
            for (uint32_t di = 0; di < rule.declarations.size(); ++di) {
                const Declaration& d = rule.declarations[di];

                Prop p;
                if (!lookup_prop(d.property, p)) continue;

                Winner w;
                w.rule_index = entry.rule_index;
                w.decl_index = di;
                w.spec = entry.spec;
                w.order = entry.order;
                w.important = d.important;

                const size_t pi = style::prop_index(p);
                if (winners[pi].empty() || w.dominates(winners[pi]))
                    winners[pi] = w;
            }
        }

        // 2. Теперь применяем победившие декларации (они перепишут унаследованные значения)
        for (size_t pi = 0; pi < style::PROP_COUNT; ++pi) {
            if (winners[pi].empty()) continue;
            const Declaration& d = index.rule(winners[pi].rule_index)
                .declarations[winners[pi].decl_index];
            apply_declaration(static_cast<Prop>(pi), d, idx, parent_idx, storage);
            storage.mark_explicit(idx, static_cast<Prop>(pi));
        }

        finalize(idx, storage);
    }

    // -------------------------------------------------------------
    //  Маппинг property-name → Prop
    // -------------------------------------------------------------
    static bool lookup_prop(const std::string& name, Prop& out) {
        using P = Prop;
        // Ключи в CSSParser уже в нижнем регистре.
        static const std::unordered_map<std::string, P> table = {
            {"display",        P::Display},
            {"position",       P::Position},
            {"float",          P::Float},
            {"overflow",       P::Overflow},
            {"visibility",     P::Visibility},
            {"box-sizing",     P::BoxSizing},
            {"z-index",        P::ZIndex},
            {"opacity",        P::Opacity},
            {"color",          P::Color},
            {"background-color", P::BackgroundColor},
            {"font-size",      P::FontSize},
            {"font-weight",    P::FontWeight},
            {"font-style",     P::FontStyle},
            {"line-height",    P::LineHeight},
            {"text-align",     P::TextAlign},
            {"letter-spacing", P::LetterSpacing},
            {"white-space",    P::WhiteSpace},
            {"width",          P::Width},
            {"height",         P::Height},
            {"min-width",      P::MinWidth},
            {"max-width",      P::MaxWidth},
            {"min-height",     P::MinHeight},
            {"max-height",     P::MaxHeight},
            {"margin-top",     P::MarginTop},
            {"margin-right",   P::MarginRight},
            {"margin-bottom",  P::MarginBottom},
            {"margin-left",    P::MarginLeft},
            {"padding-top",    P::PaddingTop},
            {"padding-right",  P::PaddingRight},
            {"padding-bottom", P::PaddingBottom},
            {"padding-left",   P::PaddingLeft},
            {"border-top-width",    P::BorderTopWidth},
            {"border-right-width",  P::BorderRightWidth},
            {"border-bottom-width", P::BorderBottomWidth},
            {"border-left-width",   P::BorderLeftWidth},
            {"flex-direction",  P::FlexDirection},
            {"justify-content", P::JustifyContent},
            {"align-items",     P::AlignItems},
            {"flex-grow",       P::FlexGrow},
            {"flex-shrink",     P::FlexShrink},
            {"flex-basis",      P::FlexBasis},
        };
        auto it = table.find(name);
        if (it == table.end()) return false;
        out = it->second;
        return true;
    }

    // -------------------------------------------------------------
    //  Применение одной декларации (уже победившей в каскаде).
    //  Разворачиваем шорткаты, разрешаем em/rem для font-size.
    // -------------------------------------------------------------
    static void apply_declaration(Prop p, const Declaration& d,
        uint32_t idx, uint32_t parent_idx,
        StyleStorageSoA& storage)
    {
        const std::string& v = d.value;

        switch (p) {
        case Prop::Display:  storage.displays[idx] = parse_display(v);  break;
        case Prop::Position: storage.positions[idx] = parse_position(v); break;
        case Prop::Float:    storage.floats[idx] = parse_float(v);    break;
        case Prop::Overflow: storage.overflows[idx] = parse_overflow(v); break;
        case Prop::Visibility: storage.visibilities[idx] = parse_visibility(v); break;
        case Prop::BoxSizing: storage.box_sizings[idx] = parse_box_sizing(v); break;
        case Prop::TextAlign: storage.text_aligns[idx] = parse_text_align(v); break;
        case Prop::FontStyle: storage.font_styles[idx] = parse_font_style(v); break;
        case Prop::WhiteSpace: storage.white_spaces[idx] = parse_white_space(v); break;
        case Prop::FlexDirection: storage.flex_directions[idx] = parse_flex_dir(v); break;
        case Prop::JustifyContent: storage.justify_contents[idx] = parse_justify(v); break;
        case Prop::AlignItems: storage.align_items[idx] = parse_align_items(v); break;

        case Prop::ZIndex: {
            char* end = nullptr;
            long n = std::strtol(v.c_str(), &end, 10);
            storage.z_indices[idx] = (end != v.c_str()) ? (int32_t)n : 0;
            break;
        }
        case Prop::Opacity: {
            float f = std::strtof(v.c_str(), nullptr);
            if (f < 0) f = 0; if (f > 1) f = 1;
            storage.opacity_q[idx] = (uint8_t)std::lround(f * 255.0f);
            break;
        }

        case Prop::Color:
            storage.text_colors[idx] = parse_color(v).pack();
            break;
        case Prop::BackgroundColor:
            storage.background_colors[idx] = parse_color(v).pack();
            break;

        case Prop::FontSize: {
            // parent_idx == INVALID_INDEX  ⇔  это корень (idx == 0).
            // inherit_from_parent уже скопировал font_sizes[parent_idx] в font_sizes[idx],
            // но резолвим em/% именно против родителя — так требует спецификация.
            const float parent_fs = (parent_idx != INVALID_INDEX)
                ? storage.font_sizes[parent_idx]
                : 16.0f;
            storage.font_sizes[idx] = parse_font_size(v, parent_fs, idx, storage);
            break;
        }

        case Prop::FontWeight: {
            if (v == "normal") storage.font_weights[idx] = 400;
            else if (v == "bold") storage.font_weights[idx] = 700;
            else {
                char* end = nullptr;
                long n = std::strtol(v.c_str(), &end, 10);
                if (end != v.c_str()) storage.font_weights[idx] = (uint16_t)n;
            }
            break;
        }
        case Prop::LineHeight: {
            if (v == "normal") { storage.line_heights[idx] = 0.0f; break; }
            float fs = storage.font_sizes[idx];
            float result = 0.0f;
            if (v.size() > 1 && v[0] != '+' && v[0] != '-' &&
                v.find_first_not_of("0123456789.") != std::string::npos) {
                Length l = parse_length(v);
                result = resolve_length(l, fs, idx, storage);            // + idx
            }
            else {
                float mul = std::strtof(v.c_str(), nullptr);
                result = mul * fs;
            }
            storage.line_heights[idx] = result;
            break;
        }

        case Prop::LetterSpacing:
            storage.letter_spacings[idx] = resolve_length(
                parse_length(v), storage.font_sizes[idx], idx, storage);
            break;

            // ----- размеры / отступы -----
        case Prop::Width:      storage.widths[idx] = parse_length(v); break;
        case Prop::Height:     storage.heights[idx] = parse_length(v); break;
        case Prop::MinWidth:   storage.min_widths[idx] = parse_length(v); break;
        case Prop::MaxWidth:   storage.max_widths[idx] = parse_length(v); break;
        case Prop::MinHeight:  storage.min_heights[idx] = parse_length(v); break;
        case Prop::MaxHeight:  storage.max_heights[idx] = parse_length(v); break;
        case Prop::MarginTop:    storage.margin_top[idx] = parse_length(v); break;
        case Prop::MarginRight:  storage.margin_right[idx] = parse_length(v); break;
        case Prop::MarginBottom: storage.margin_bottom[idx] = parse_length(v); break;
        case Prop::MarginLeft:   storage.margin_left[idx] = parse_length(v); break;
        case Prop::PaddingTop:    storage.padding_top[idx] = parse_length(v); break;
        case Prop::PaddingRight:  storage.padding_right[idx] = parse_length(v); break;
        case Prop::PaddingBottom: storage.padding_bottom[idx] = parse_length(v); break;
        case Prop::PaddingLeft:   storage.padding_left[idx] = parse_length(v); break;
        case Prop::BorderTopWidth:    storage.border_top_width[idx] = parse_length(v); break;
        case Prop::BorderRightWidth:  storage.border_right_width[idx] = parse_length(v); break;
        case Prop::BorderBottomWidth: storage.border_bottom_width[idx] = parse_length(v); break;
        case Prop::BorderLeftWidth:   storage.border_left_width[idx] = parse_length(v); break;

        case Prop::FlexGrow:   storage.flex_grows[idx] = std::strtof(v.c_str(), nullptr); break;
        case Prop::FlexShrink: storage.flex_shrinks[idx] = std::strtof(v.c_str(), nullptr); break;
        case Prop::FlexBasis:  storage.flex_bases[idx] = parse_length(v); break;

        default: break;
        }
    }

    // -------------------------------------------------------------
    //  Наследование
    // -------------------------------------------------------------
    static void inherit_from_parent(uint32_t idx, uint32_t parent_idx,
        StyleStorageSoA& storage)
    {
        // Таблица наследуемых свойств — фиксирована спекой CSS.
        // Инвариант: вызывается ДО применения explicit-деклараций,
        // поэтому проверка is_explicit избыточна: победители каскада
        // перезапишут унаследованные значения на шаге 2.
        static constexpr Prop kInherited[] = {
            Prop::Color,
            Prop::FontSize,
            Prop::FontWeight,
            Prop::FontStyle,
            Prop::LineHeight,
            Prop::TextAlign,
            Prop::LetterSpacing,
            Prop::WhiteSpace,
            Prop::Visibility,
        };
        for (Prop p : kInherited) {
            copy_prop(p, parent_idx, idx, storage);
        }
    }

    static void copy_prop(Prop p, uint32_t from, uint32_t to, StyleStorageSoA& s) {
        switch (p) {
        case Prop::Color:         s.text_colors[to] = s.text_colors[from]; break;
        case Prop::FontSize:      s.font_sizes[to] = s.font_sizes[from]; break;
        case Prop::FontWeight:    s.font_weights[to] = s.font_weights[from]; break;
        case Prop::FontStyle:     s.font_styles[to] = s.font_styles[from]; break;
        case Prop::LineHeight:    s.line_heights[to] = s.line_heights[from]; break;
        case Prop::TextAlign:     s.text_aligns[to] = s.text_aligns[from]; break;
        case Prop::LetterSpacing: s.letter_spacings[to] = s.letter_spacings[from]; break;
        case Prop::WhiteSpace:    s.white_spaces[to] = s.white_spaces[from]; break;
        case Prop::Visibility:    s.visibilities[to] = s.visibilities[from]; break;
        default: break;
        }
    }

    // -------------------------------------------------------------
    //  Финализация: дефолты, зависящие от других полей
    // -------------------------------------------------------------
    static void finalize(uint32_t idx, StyleStorageSoA& s) {
        // line-height: normal → 1.2 * font-size
        if (s.line_heights[idx] == 0.0f) {
            s.line_heights[idx] = s.font_sizes[idx] * 1.2f;
        }
    }

    // =============================================================
    //  Парсеры значений
    // =============================================================
    static Display parse_display(const std::string& v) {
        if (v == "none")         return Display::None;
        if (v == "block")        return Display::Block;
        if (v == "inline")       return Display::Inline;
        if (v == "inline-block") return Display::InlineBlock;
        if (v == "flex")         return Display::Flex;
        if (v == "inline-flex")  return Display::InlineFlex;
        if (v == "grid")         return Display::Grid;
        if (v == "inline-grid")  return Display::InlineGrid;
        if (v == "table")        return Display::Table;
        if (v == "table-row")    return Display::TableRow;
        if (v == "table-cell")   return Display::TableCell;
        if (v == "contents")     return Display::Contents;
        return Display::Inline;
    }
    static Position parse_position(const std::string& v) {
        if (v == "relative") return Position::Relative;
        if (v == "absolute") return Position::Absolute;
        if (v == "fixed")    return Position::Fixed;
        if (v == "sticky")   return Position::Sticky;
        return Position::Static;
    }
    static Float parse_float(const std::string& v) {
        if (v == "left")  return Float::Left;
        if (v == "right") return Float::Right;
        return Float::None;
    }
    static Overflow parse_overflow(const std::string& v) {
        if (v == "hidden") return Overflow::Hidden;
        if (v == "scroll") return Overflow::Scroll;
        if (v == "auto")   return Overflow::Auto;
        return Overflow::Visible;
    }
    static Visibility parse_visibility(const std::string& v) {
        if (v == "hidden")   return Visibility::Hidden;
        if (v == "collapse") return Visibility::Collapse;
        return Visibility::Visible;
    }
    static BoxSizing parse_box_sizing(const std::string& v) {
        return v == "border-box" ? BoxSizing::BorderBox : BoxSizing::ContentBox;
    }
    static TextAlign parse_text_align(const std::string& v) {
        if (v == "left")    return TextAlign::Left;
        if (v == "right")   return TextAlign::Right;
        if (v == "center")  return TextAlign::Center;
        if (v == "justify") return TextAlign::Justify;
        if (v == "end")     return TextAlign::End;
        return TextAlign::Start;
    }
    static FontStyle parse_font_style(const std::string& v) {
        if (v == "italic")  return FontStyle::Italic;
        if (v == "oblique") return FontStyle::Oblique;
        return FontStyle::Normal;
    }
    static WhiteSpace parse_white_space(const std::string& v) {
        if (v == "nowrap")   return WhiteSpace::NoWrap;
        if (v == "pre")      return WhiteSpace::Pre;
        if (v == "pre-wrap") return WhiteSpace::PreWrap;
        if (v == "pre-line") return WhiteSpace::PreLine;
        return WhiteSpace::Normal;
    }
    static FlexDir parse_flex_dir(const std::string& v) {
        if (v == "row-reverse")    return FlexDir::RowReverse;
        if (v == "column")         return FlexDir::Column;
        if (v == "column-reverse") return FlexDir::ColumnReverse;
        return FlexDir::Row;
    }
    static Justify parse_justify(const std::string& v) {
        if (v == "end" || v == "flex-end")   return Justify::End;
        if (v == "center")                    return Justify::Center;
        if (v == "space-between")             return Justify::SpaceBetween;
        if (v == "space-around")              return Justify::SpaceAround;
        if (v == "space-evenly")              return Justify::SpaceEvenly;
        return Justify::Start;
    }
    static AlignItems parse_align_items(const std::string& v) {
        if (v == "flex-start" || v == "start") return AlignItems::FlexStart;
        if (v == "flex-end" || v == "end")     return AlignItems::FlexEnd;
        if (v == "center")                     return AlignItems::Center;
        if (v == "baseline")                   return AlignItems::Baseline;
        return AlignItems::Stretch;
    }

    // ---- Length ----
    static Length parse_length(const std::string& v) {
        // trim
        size_t b = 0, e = v.size();
        while (b < e && std::isspace((unsigned char)v[b])) ++b;
        while (e > b && std::isspace((unsigned char)v[e - 1])) --e;
        if (b == e) return { 0.0f, Unit::Px };

        std::string s = v.substr(b, e - b);
        if (s == "auto")   return { -1.0f, Unit::Auto };
        if (s == "none")   return { 0.0f, Unit::None };
        if (s == "0")      return { 0.0f, Unit::Px };

        // split числовая часть / единица
        size_t i = 0;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
        if (i < s.size() && s[i] == '.') {
            ++i;
            while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            size_t j = i + 1;
            if (j < s.size() && (s[j] == '+' || s[j] == '-')) ++j;
            const size_t digits_start = j;
            while (j < s.size() && std::isdigit((unsigned char)s[j])) ++j;
            if (j > digits_start) i = j;   // принимаем 'e' только при наличии цифр после
        }

        if (i == 0) return { 0.0f, Unit::Px };   // не число вовсе

        float val = 0.0f;
        try { val = std::stof(s.substr(0, i)); }
        catch (...) { return { 0.0f, Unit::Px }; }
        std::string u = s.substr(i);


        for (char& c : u) c = (char)std::tolower((unsigned char)c);

        Unit unit = Unit::Px;
        if (u.empty() || u == "px") unit = Unit::Px;
        else if (u == "em")    unit = Unit::Em;
        else if (u == "rem")   unit = Unit::Rem;
        else if (u == "%")     unit = Unit::Percent;
        else if (u == "vw")    unit = Unit::Vw;
        else if (u == "vh")    unit = Unit::Vh;
        else if (u == "vmin")  unit = Unit::Vmin;
        else if (u == "vmax")  unit = Unit::Vmax;
        else if (u == "pt") { unit = Unit::Px; val *= 96.0f / 72.0f; }
        else if (u == "pc") { unit = Unit::Px; val *= 16.0f; }
        else if (u == "in") { unit = Unit::Px; val *= 96.0f; }
        else if (u == "cm") { unit = Unit::Px; val *= 96.0f / 2.54f; }
        else if (u == "mm") { unit = Unit::Px; val *= 96.0f / 25.4f; }
        return { val, unit };
    }

    static float resolve_rem(const StyleStorageSoA& s, uint32_t idx) {
        // idx == 0 — это сам корень; по спеке rem для корня = initial (16px)
        if (idx == 0 || s.font_sizes.empty()) return 16.0f;
        return s.font_sizes[0];
    }

    static float parse_font_size(const std::string& v, float parent_fs,
        uint32_t idx, const StyleStorageSoA& s) {
        if (v == "xx-small") return 9.0f;
        if (v == "x-small")  return 10.0f;
        if (v == "small")    return 13.0f;
        if (v == "medium")   return 16.0f;
        if (v == "large")    return 18.0f;
        if (v == "x-large")  return 24.0f;
        if (v == "xx-large") return 32.0f;
        if (v == "smaller")  return parent_fs * 0.833f;
        if (v == "larger")   return parent_fs * 1.2f;
        if (v == "inherit")  return parent_fs;

        Length l = parse_length(v);
        switch (l.unit) {
        case Unit::Px:      return l.value;
        case Unit::Em:      return l.value * parent_fs;
        case Unit::Rem:     return l.value * resolve_rem(s, idx);   // ← было 16.0f
        case Unit::Percent: return l.value * 0.01f * parent_fs;
        case Unit::Pt:      return l.value * 96.0f / 72.0f;
        default:            return parent_fs;
        }
    }

    static float resolve_length(const Length& l, float font_size,
        uint32_t idx, const StyleStorageSoA& s) {
        switch (l.unit) {
        case Unit::Px:      return l.value;
        case Unit::Em:      return l.value * font_size;
        case Unit::Rem:     return l.value * resolve_rem(s, idx);   // ← было 16.0f
        case Unit::Percent: return 0.0f;    // % от контейнера → layout
        case Unit::Pt:      return l.value * 96.0f / 72.0f;
        default:            return l.value;
        }
    }

    // ---- Цвет ----
    static style::Color parse_color(const std::string& v) {
        // #rgb #rgba #rrggbb #rrggbbaa
        if (!v.empty() && v[0] == '#') {
            std::string h = v.substr(1);
            auto hex1 = [](char c) -> uint8_t {
                if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
                if (c >= 'a' && c <= 'f') return (uint8_t)(10 + c - 'a');
                if (c >= 'A' && c <= 'F') return (uint8_t)(10 + c - 'A');
                return 0;
                };
            if (h.size() == 3) {
                return { (uint8_t)(hex1(h[0]) * 17), (uint8_t)(hex1(h[1]) * 17),
                         (uint8_t)(hex1(h[2]) * 17), 255 };
            }
            if (h.size() == 4) {
                return { (uint8_t)(hex1(h[0]) * 17), (uint8_t)(hex1(h[1]) * 17),
                         (uint8_t)(hex1(h[2]) * 17), (uint8_t)(hex1(h[3]) * 17) };
            }
            if (h.size() == 6) {
                return { (uint8_t)((hex1(h[0]) << 4) | hex1(h[1])),
                         (uint8_t)((hex1(h[2]) << 4) | hex1(h[3])),
                         (uint8_t)((hex1(h[4]) << 4) | hex1(h[5])), 255 };
            }
            if (h.size() == 8) {
                return { (uint8_t)((hex1(h[0]) << 4) | hex1(h[1])),
                         (uint8_t)((hex1(h[2]) << 4) | hex1(h[3])),
                         (uint8_t)((hex1(h[4]) << 4) | hex1(h[5])),
                         (uint8_t)((hex1(h[6]) << 4) | hex1(h[7])) };
            }
        }
        // rgb()/rgba() — базовый парсинг
        if (v.rfind("rgb", 0) == 0) {
            size_t lp = v.find('('), rp = v.find(')');
            if (lp != std::string::npos && rp != std::string::npos) {
                std::string inner = v.substr(lp + 1, rp - lp - 1);
                int vals[4] = { 0,0,0,255 };
                int k = 0;
                size_t i = 0;
                while (i < inner.size() && k < 4) {
                    while (i < inner.size() && (std::isspace((unsigned char)inner[i]) ||
                        inner[i] == ',')) ++i;
                    size_t s = i;
                    while (i < inner.size() && inner[i] != ',' &&
                        !std::isspace((unsigned char)inner[i])) ++i;
                    if (i == s) break;
                    if (inner[s] == '%') { ++s; }
                    vals[k++] = std::atoi(inner.substr(s, i - s).c_str());
                }
                // упрощение: если % — уже не важно, клампим
                for (int i2 = 0; i2 < 3; ++i2) {
                    if (vals[i2] < 0) vals[i2] = 0;
                    if (vals[i2] > 255) vals[i2] = 255;
                }
                return { (uint8_t)vals[0], (uint8_t)vals[1], (uint8_t)vals[2], (uint8_t)vals[3] };
            }
        }
        // named (минимум)
        static const std::unordered_map<std::string, uint32_t> named = {
            {"black", 0x000000FF}, {"white", 0xFFFFFFFF}, {"red", 0xFF0000FF},
            {"green", 0x008000FF}, {"blue", 0x0000FFFF}, {"yellow", 0xFFFF00FF},
            {"cyan", 0x00FFFFFF}, {"magenta", 0xFF00FFFF}, {"gray", 0x808080FF},
            {"grey", 0x808080FF}, {"silver", 0xC0C0C0FF}, {"maroon", 0x800000FF},
            {"olive", 0x808000FF}, {"lime", 0x00FF00FF}, {"aqua", 0x00FFFFFF},
            {"teal", 0x008080FF}, {"navy", 0x000080FF}, {"fuchsia", 0xFF00FFFF},
            {"purple", 0x800080FF}, {"orange", 0xFFA500FF}, {"transparent", 0x00000000},
        };
        auto it = named.find(v);
        if (it != named.end()) return style::Color::unpack(it->second);
        return { 0,0,0,255 };
    }
};