#include "style_properties.h"
#include <unordered_map>
#include <algorithm>
#include <cctype>

namespace style_props {

    // =========================================================
    //  Таблица CSS-свойств (перенесено из style_tree_builder)
    // =========================================================
    bool lookup_css_prop(const std::string& name, style::Prop& out) {
        using P = style::Prop;
        static const std::unordered_map<std::string, P> table = {
            {"display",        P::Display},
            {"position",       P::Position},
            {"float",          P::Float},
            {"clear",          P::Clear},
            {"overflow",       P::Overflow},
            {"visibility",     P::Visibility},
            {"box-sizing",     P::BoxSizing},
            {"z-index",        P::ZIndex},
            {"opacity",        P::Opacity},
            {"color",          P::Color},
            {"background-color", P::BackgroundColor},
            {"background-image", P::BackgroundImage},
            {"font-size",      P::FontSize},
            {"font-weight",    P::FontWeight},
            {"font-style",     P::FontStyle},
            {"font-family",    P::FontFamily},
            {"line-height",    P::LineHeight},
            {"text-align",     P::TextAlign},
            {"text-decoration", P::TextDecoration},
            {"letter-spacing", P::LetterSpacing},
            {"white-space",    P::WhiteSpace},
            {"width",          P::Width},
            {"height",         P::Height},
            {"min-width",      P::MinWidth},
            {"max-width",      P::MaxWidth},
            {"min-height",     P::MinHeight},
            {"max-height",     P::MaxHeight},
            {"top",            P::Top},
            {"right",          P::Right},
            {"bottom",         P::Bottom},
            {"left",           P::Left},
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
            {"border-top-color",    P::BorderTopColor},
            {"border-right-color",  P::BorderRightColor},
            {"border-bottom-color", P::BorderBottomColor},
            {"border-left-color",   P::BorderLeftColor},
            {"border-radius",              P::BorderRadius},
            {"border-top-left-radius",     P::BorderTopLeftRadius},
            {"border-top-right-radius",    P::BorderTopRightRadius},
            {"border-bottom-right-radius", P::BorderBottomRightRadius},
            {"border-bottom-left-radius",  P::BorderBottomLeftRadius},
            {"background-size",            P::BackgroundSize},
            {"background-position",        P::BackgroundPosition},
            {"background-repeat",          P::BackgroundRepeat},
            {"flex-direction",  P::FlexDirection},
            {"justify-content", P::JustifyContent},
            {"align-items",     P::AlignItems},
            {"vertical-align",  P::VerticalAlign},
            {"flex-grow",       P::FlexGrow},
            {"flex-shrink",     P::FlexShrink},
            {"flex-basis",      P::FlexBasis},
            {"flex-wrap",       P::FlexWrap},
            {"align-content",   P::AlignContent},
            {"align-self",      P::AlignSelf},
            {"order",           P::Order},
            {"row-gap",         P::RowGap},
            {"column-gap",      P::ColumnGap},
            {"gap",             P::RowGap}, // Обработаем отдельно как шортхенд
            {"grid-template-columns", P::GridTemplateColumns},
            {"grid-template-rows",    P::GridTemplateRows},
            {"grid-auto-rows",        P::GridAutoRows},
            {"grid-column",           P::GridColumn},
            {"grid-row",              P::GridRow},

        };
        auto it = table.find(name);
        if (it == table.end()) return false;
        out = it->second;
        return true;
    }

    // =========================================================
    //  Таблица HTML-атрибутов (презентационные свойства)
    // =========================================================
    std::string map_html_attr_to_css(const std::string& attr) {
        static const std::unordered_map<std::string, std::string> table = {
            {"bgcolor", "background-color"},
            {"background", "background-image"},
            {"color", "color"},
            {"width", "width"},
            {"height", "height"},
            {"align", "text-align"},
            {"valign", "vertical-align"},
            {"border", "border-width"},
            {"cellpadding", "padding"},
            {"cellspacing", "border-spacing"},
            {"face", "font-family"},
            {"size", "font-size"}
        };
        auto it = table.find(attr);
        return (it != table.end()) ? it->second : "";
    }

    // =========================================================
    //  Наследование
    // =========================================================
    bool is_inherited(style::Prop p) {
        using P = style::Prop;
        switch (p) {
        case P::Color: case P::FontSize: case P::FontWeight:
        case P::FontStyle: case P::FontFamily: case P::LineHeight:
        case P::TextAlign: case P::LetterSpacing: case P::WhiteSpace:
        case P::Visibility:
            return true;
        default: return false;
        }
    }

    // =========================================================
    //  Шортхенды (Margin, Padding, Border)
    // =========================================================

    bool expand_shorthand(const std::string& prop_name, const std::string& value,
        std::vector<std::pair<std::string, std::string>>& out)
    {
        auto split = [](const std::string& val) {
            std::vector<std::string> tokens;
            std::string cur;
            for (char c : val) {
                if (std::isspace(static_cast<unsigned char>(c))) {
                    if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
                }
                else {
                    cur += c;
                }
            }
            if (!cur.empty()) tokens.push_back(cur);
            return tokens;
            };

        auto apply_1to4 = [&](const std::string& prefix, const std::string& suffix, const std::vector<std::string>& tokens) {
            if (tokens.empty()) return false;
            std::string top, right, bottom, left;
            if (tokens.size() == 1) { top = right = bottom = left = tokens[0]; }
            else if (tokens.size() == 2) { top = bottom = tokens[0]; right = left = tokens[1]; }
            else if (tokens.size() == 3) { top = tokens[0]; right = left = tokens[1]; bottom = tokens[2]; }
            else if (tokens.size() >= 4) { top = tokens[0]; right = tokens[1]; bottom = tokens[2]; left = tokens[3]; }
            else return false;

            out.push_back({ prefix + "top" + suffix, top });
            out.push_back({ prefix + "right" + suffix, right });
            out.push_back({ prefix + "bottom" + suffix, bottom });
            out.push_back({ prefix + "left" + suffix, left });
            return true;
            };

        if (prop_name == "margin")       return apply_1to4("margin-", "", split(value));
        if (prop_name == "padding")      return apply_1to4("padding-", "", split(value));
        if (prop_name == "border-width") return apply_1to4("border-", "-width", split(value));
        if (prop_name == "border-color") return apply_1to4("border-", "-color", split(value));
        if (prop_name == "gap") {
            auto tokens = split(value);
            if (tokens.size() == 1) {
                out.push_back({ "row-gap", tokens[0] });
                out.push_back({ "column-gap", tokens[0] });
            }
            else if (tokens.size() == 2) {
                out.push_back({ "row-gap", tokens[0] });
                out.push_back({ "column-gap", tokens[1] });
            }
            return true;
        }
        if (prop_name == "border") {
            auto tokens = split(value);
            std::string width = "medium";
            std::string color = "currentcolor";

            for (const auto& t : tokens) {
                if (std::isdigit(static_cast<unsigned char>(t[0])) || t == "thin" || t == "medium" || t == "thick") {
                    width = t;
                }
                else if (t[0] == '#' || t.rfind("rgb", 0) == 0 || std::isalpha(static_cast<unsigned char>(t[0]))) {
                    // Игнорируем стили линий, нам нужен только цвет
                    if (t != "solid" && t != "dashed" && t != "dotted" && t != "none") {
                        color = t;
                    }
                }
            }

            for (const std::string& side : { "top", "right", "bottom", "left" }) {
                out.push_back({ "border-" + side + "-width", width });
                out.push_back({ "border-" + side + "-color", color });
            }
            return true;
        }

        return false;
    }

} // namespace style_props