#pragma once
#include <string>
#include <vector>
#include "style_types.h"

namespace style_props {

    // 1. Маппинг имени CSS-свойства в enum (например, "color" -> Prop::Color)
    bool lookup_css_prop(const std::string& name, style::Prop& out);

    // 2. Маппинг HTML-атрибута в CSS-свойство (например, "bgcolor" -> "background-color")
    // Возвращает пустую строку, если атрибут не презентационный.
    std::string map_html_attr_to_css(const std::string& attr_name);

    // 3. Список наследуемых свойств (для каскада)
    bool is_inherited(style::Prop p);

    // 4. Разворот шортхендов (margin -> margin-top, margin-right...)
    // Возвращает true, если свойство было развернуто.
    bool expand_shorthand(const std::string& prop_name, const std::string& value,
        std::vector<std::pair<std::string, std::string>>& out_longhands);

} // namespace style_props