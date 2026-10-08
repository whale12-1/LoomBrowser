#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

#include "../parsers/css_parser/headers/css_parser.h"
#include "../parsers/css_parser/headers/css_dom.h"
#include "../parsers/arena_memory_allocator/headers/arena.h"

namespace {

    // Парсит CSS и возвращает StyleSheet.
    struct ParsedCSS { StyleSheet* sheet = nullptr; };

    inline ParsedCSS parse_css(ArenaAllocator& arena, const std::string& css) {
        CSSParser p(arena);
        return { p.parse(css) };
    }

    // Достаёт первый ComplexSelector первого правила.
    inline const ComplexSelector* first_selector(const StyleSheet* s) {
        if (!s || s->rules.empty()) return nullptr;
        if (s->rules[0].selectors.empty()) return nullptr;
        return &s->rules[0].selectors[0];
    }

    // Проверяет, что первый compound первого селектора состоит из одного
    // SimpleSelector с заданным kind и name.
    inline bool has_simple(const ComplexSelector* cs,
        SimpleSelector::Kind kind,
        std::string_view name)
    {
        if (!cs || cs->compounds.empty()) return false;
        for (const auto& part : cs->compounds[0].parts) {
            if (part.kind == kind && std::string_view(part.name.data(), part.name.size()) == name)
                return true;
        }
        return false;
    }

    inline const Declaration* first_decl(const StyleSheet* s) {
        if (!s || s->rules.empty()) return nullptr;
        if (s->rules[0].declarations.empty()) return nullptr;
        return &s->rules[0].declarations[0];
    }

} // namespace


// ============================================================
//  Базовая структура
// ============================================================

TEST_CASE("CSSParser: empty input yields empty sheet",
    "[css_parser][basic]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, "");
    REQUIRE(r.sheet != nullptr);
    REQUIRE(r.sheet->rules.empty());
    REQUIRE(r.sheet->at_rules.empty());
}

TEST_CASE("CSSParser: whitespace-only input",
    "[css_parser][basic]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, "   \n\t  ");
    REQUIRE(r.sheet->rules.empty());
}

TEST_CASE("CSSParser: simple rule with one declaration",
    "[css_parser][basic]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, "body { color: red; }");
    REQUIRE(r.sheet->rules.size() == 1);

    auto* sel = first_selector(r.sheet);
    REQUIRE(sel != nullptr);
    REQUIRE(has_simple(sel, SimpleSelector::Kind::Tag, "body"));

    REQUIRE(r.sheet->rules[0].declarations.size() == 1);
    const auto& d = r.sheet->rules[0].declarations[0];
    REQUIRE(std::string_view(d.property.data(), d.property.size()) == "color");
    REQUIRE(std::string_view(d.value.data(), d.value.size()) == "red");
    REQUIRE(d.important == false);
}

TEST_CASE("CSSParser: multiple rules",
    "[css_parser][basic]") {
    ArenaAllocator arena;
    auto r = parse_css(arena,
        "body { color: red; }"
        "p { margin: 0; }"
        "div { padding: 1px; }");
    REQUIRE(r.sheet->rules.size() == 3);
}

TEST_CASE("CSSParser: multiple declarations in one rule",
    "[css_parser][basic]") {
    // Без shorthand — считаем ровно то, что написали.
    ArenaAllocator arena;
    auto r = parse_css(arena,
        "p { color: red; font-size: 16px; text-align: center; }");
    REQUIRE(r.sheet->rules[0].declarations.size() == 3);
}

TEST_CASE("CSSParser: shorthand margin expands to 4 longhands",
    "[css_parser][decl][shorthand]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, "p { margin: 0 auto; }");

    const auto& decls = r.sheet->rules[0].declarations;
    REQUIRE(decls.size() == 4);   // margin-top/right/bottom/left

    auto val = [&](std::string_view prop) -> std::string_view {
        for (const auto& d : decls) {
            if (std::string_view(d.property.data(), d.property.size()) == prop)
                return std::string_view(d.value.data(), d.value.size());
        }
        return {};
        };

    // "0 auto" → top=bottom=0, right=left=auto
    REQUIRE(val("margin-top") == "0");
    REQUIRE(val("margin-right") == "auto");
    REQUIRE(val("margin-bottom") == "0");
    REQUIRE(val("margin-left") == "auto");
}

TEST_CASE("CSSParser: shorthand padding expands to 4 longhands",
    "[css_parser][decl][shorthand]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, "p { padding: 1px 2px 3px 4px; }");

    const auto& decls = r.sheet->rules[0].declarations;
    REQUIRE(decls.size() == 4);

    auto val = [&](std::string_view prop) -> std::string_view {
        for (const auto& d : decls) {
            if (std::string_view(d.property.data(), d.property.size()) == prop)
                return std::string_view(d.value.data(), d.value.size());
        }
        return {};
        };

    REQUIRE(val("padding-top") == "1px");
    REQUIRE(val("padding-right") == "2px");
    REQUIRE(val("padding-bottom") == "3px");
    REQUIRE(val("padding-left") == "4px");
}

TEST_CASE("CSSParser: single-value shorthand fills all sides",
    "[css_parser][decl][shorthand]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, "p { margin: 10px; }");

    const auto& decls = r.sheet->rules[0].declarations;
    REQUIRE(decls.size() == 4);
    for (const auto& d : decls)
        REQUIRE(std::string_view(d.value.data(), d.value.size()) == "10px");
}

TEST_CASE("CSSParser: parser is reusable",
    "[css_parser][basic]") {
    ArenaAllocator arena;
    CSSParser p(arena);
    StyleSheet* a = p.parse("p { color: red; }");
    StyleSheet* b = p.parse("div { color: blue; }");
    REQUIRE(a != b);
    REQUIRE(a->rules.size() == 1);
    REQUIRE(b->rules.size() == 1);
}


// ============================================================
//  Селекторы
// ============================================================

TEST_CASE("CSSParser: universal selector",
    "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, "* { margin: 0; }");
    auto* sel = first_selector(r.sheet);
    REQUIRE(has_simple(sel, SimpleSelector::Kind::Universal, ""));
}

TEST_CASE("CSSParser: class selector",
    "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, ".foo { color: red; }");
    auto* sel = first_selector(r.sheet);
    REQUIRE(has_simple(sel, SimpleSelector::Kind::Class, "foo"));
}

TEST_CASE("CSSParser: id selector",
    "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, "#main { color: red; }");
    auto* sel = first_selector(r.sheet);
    REQUIRE(has_simple(sel, SimpleSelector::Kind::Id, "main"));
}

TEST_CASE("CSSParser: compound selector",
    "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, "div.foo#bar { color: red; }");
    auto* sel = first_selector(r.sheet);
    REQUIRE(sel->compounds.size() == 1);
    REQUIRE(sel->compounds[0].parts.size() == 3);
    REQUIRE(has_simple(sel, SimpleSelector::Kind::Tag, "div"));
    REQUIRE(has_simple(sel, SimpleSelector::Kind::Class, "foo"));
    REQUIRE(has_simple(sel, SimpleSelector::Kind::Id, "bar"));
}

TEST_CASE("CSSParser: descendant combinator",
    "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, "div p { color: red; }");
    auto* sel = first_selector(r.sheet);
    REQUIRE(sel->compounds.size() == 2);
    REQUIRE(sel->compounds[0].combinator == Combinator::None);       // первый — без комбинатора
    REQUIRE(sel->compounds[1].combinator == Combinator::Descendant); // второй — через пробел
}

TEST_CASE("CSSParser: child combinator",
    "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, "div > p { color: red; }");
    auto* sel = first_selector(r.sheet);
    REQUIRE(sel->compounds.size() == 2);
    REQUIRE(sel->compounds[1].combinator == Combinator::Child);
}

TEST_CASE("CSSParser: adjacent sibling combinator",
    "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, "h1 + p { margin: 0; }");
    auto* sel = first_selector(r.sheet);
    REQUIRE(sel->compounds.size() == 2);
    REQUIRE(sel->compounds[1].combinator == Combinator::AdjacentSibling);
}

TEST_CASE("CSSParser: general sibling combinator",
    "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, "h1 ~ p { margin: 0; }");
    auto* sel = first_selector(r.sheet);
    REQUIRE(sel->compounds[1].combinator == Combinator::GeneralSibling);
}

TEST_CASE("CSSParser: selector list",
    "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, "h1, h2, h3 { color: red; }");
    REQUIRE(r.sheet->rules[0].selectors.size() == 3);
}

TEST_CASE("CSSParser: attribute selector with equals",
    "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, R"(a[href="x"] { color: red; })");
    auto* sel = first_selector(r.sheet);
    REQUIRE(has_simple(sel, SimpleSelector::Kind::Attribute, "href"));
    const auto& p = sel->compounds[0].parts[1];
    REQUIRE(std::string_view(p.op.data(), p.op.size()) == "=");
    REQUIRE(std::string_view(p.arg.data(), p.arg.size()) == "x");
}

TEST_CASE("CSSParser: pseudo-class",
    "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, "a:hover { color: red; }");
    auto* sel = first_selector(r.sheet);
    REQUIRE(has_simple(sel, SimpleSelector::Kind::PseudoClass, "hover"));
}

TEST_CASE("CSSParser: pseudo-class with argument",
    "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, "li:nth-child(2n+1) { color: red; }");
    auto* sel = first_selector(r.sheet);
    bool found = false;
    for (const auto& p : sel->compounds[0].parts) {
        if (p.kind == SimpleSelector::Kind::PseudoClass &&
            std::string_view(p.name.data(), p.name.size()) == "nth-child") {
            REQUIRE(p.has_arg);
            REQUIRE(std::string_view(p.arg.data(), p.arg.size()) == "2n+1");
            found = true;
        }
    }
    REQUIRE(found);
}

TEST_CASE("CSSParser: pseudo-element",
    "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, "p::before { content: 'x'; }");
    auto* sel = first_selector(r.sheet);
    REQUIRE(has_simple(sel, SimpleSelector::Kind::PseudoElement, "before"));
}


// ============================================================
//  Декларации
// ============================================================

TEST_CASE("CSSParser: !important flag",
    "[css_parser][decl]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, "p { color: red !important; }");
    REQUIRE(r.sheet->rules[0].declarations[0].important == true);
}

TEST_CASE("CSSParser: value with spaces trimmed",
    "[css_parser][decl]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, "p { color:    red   ; }");
    REQUIRE(std::string_view(r.sheet->rules[0].declarations[0].value.data(),
        r.sheet->rules[0].declarations[0].value.size()) == "red");
}

TEST_CASE("CSSParser: quoted string in value",
    "[css_parser][decl]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, R"(p::before { content: "hello"; })");
    auto& v = r.sheet->rules[0].declarations[0].value;
    REQUIRE(std::string_view(v.data(), v.size()).find("hello") != std::string_view::npos);
}

TEST_CASE("CSSParser: calc() keeps parens",
    "[css_parser][decl]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, "div { width: calc(100% - 20px); }");
    auto& v = r.sheet->rules[0].declarations[0].value;
    REQUIRE(std::string_view(v.data(), v.size()).find("calc") != std::string_view::npos);
}

TEST_CASE("CSSParser: custom property",
    "[css_parser][decl]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, ":root { --main-color: red; }");
    auto& d = r.sheet->rules[0].declarations[0];
    REQUIRE(std::string_view(d.property.data(), d.property.size()) == "--main-color");
    REQUIRE(d.is_custom_property == true);
}


// ============================================================
//  Комментарии
// ============================================================

TEST_CASE("CSSParser: comment between rules",
    "[css_parser][comment]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, "/* hello */ body { color: red; }");
    REQUIRE(r.sheet->rules.size() == 1);
}

TEST_CASE("CSSParser: comment inside rule",
    "[css_parser][comment]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, "p { /* mid */ color: red; }");
    REQUIRE(r.sheet->rules[0].declarations.size() == 1);
}

TEST_CASE("CSSParser: unterminated comment doesn't crash",
    "[css_parser][comment]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, "/* oops");
    REQUIRE(r.sheet != nullptr);
}


// ============================================================
//  At-rules
// ============================================================

TEST_CASE("CSSParser: @media with rules",
    "[css_parser][at]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, "@media (min-width: 100px) { p { color: red; } }");
    REQUIRE(r.sheet->at_rules.size() == 1);
    REQUIRE(std::string_view(r.sheet->at_rules[0].name.data(),
        r.sheet->at_rules[0].name.size()) == "media");
    REQUIRE(r.sheet->at_rules[0].rules.size() == 1);
}

TEST_CASE("CSSParser: @import ends with semicolon",
    "[css_parser][at]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, R"(@import url("x.css");)");
    REQUIRE(r.sheet->at_rules.size() == 1);
    REQUIRE(std::string_view(r.sheet->at_rules[0].name.data(),
        r.sheet->at_rules[0].name.size()) == "import");
}


// ============================================================
//  Устойчивость
// ============================================================

TEST_CASE("CSSParser: unbalanced brace doesn't crash",
    "[css_parser][robust]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, "p { color: red;");
    REQUIRE(r.sheet != nullptr);
}

TEST_CASE("CSSParser: stray closing brace",
    "[css_parser][robust]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, "} p { color: red; }");
    REQUIRE(r.sheet != nullptr);
}

TEST_CASE("CSSParser: at-rule inside rule block",
    "[css_parser][robust]") {
    ArenaAllocator arena;
    auto r = parse_css(arena, "p { @media (min-width: 100px) { color: red; } }");
    REQUIRE(r.sheet != nullptr);
}