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
        // ... Твой код из expand_shorthand_1to4 ...
        if (prop_name == "margin") {
            // парсим 1-4 значения
            out.push_back({ "margin-top", "..." });
            // ...
            return true;
        }
        return false;
    }

} // namespace style_props