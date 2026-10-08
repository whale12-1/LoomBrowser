#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

#include "../parsers/css_parser/headers/css_parser.h"
#include "../parsers/css_parser/headers/css_dom.h"
#include "../parsers/arena_memory_allocator/headers/arena.h"

// ============================================================
//  Утилиты для краткости тестов.
// ============================================================
namespace {

    struct ParsedCSS {
        StyleSheet* sheet = nullptr;
        std::vector<std::string> errors;

        bool ok() const { return errors.empty(); }
    };

    inline ParsedCSS parse(ArenaAllocator& arena, const std::string& css) {
        CSSParser p(arena);
        StyleSheet* s = p.parse(css);
        return ParsedCSS{ s, p.errors() };
    }

    inline const ComplexSelector* first_selector(const StyleSheet* s) {
        if (!s || s->rules.empty()) return nullptr;
        if (s->rules[0].selectors.empty()) return nullptr;
        return &s->rules[0].selectors[0];
    }

    inline bool has_simple(const ComplexSelector* cs,
        SimpleSelector::Kind kind,
        std::string_view name)
    {
        if (!cs || cs->compounds.empty()) return false;
        for (const auto& part : cs->compounds[0].parts) {
            if (part.kind == kind &&
                std::string_view(part.name.data(), part.name.size()) == name)
                return true;
        }
        return false;
    }

    inline std::string_view sv(const std::string& s) {
        return { s.data(), s.size() };
    }

    inline std::string_view prop(const Declaration& d) { return sv(d.property); }
    inline std::string_view val(const Declaration& d) { return sv(d.value); }

} // namespace


// ============================================================
//  Базовая структура
// ============================================================

TEST_CASE("CSSParser: empty input yields empty sheet",
    "[css_parser][basic]") {
    ArenaAllocator arena;
    auto r = parse(arena, "");
    REQUIRE(r.ok());
    REQUIRE(r.sheet != nullptr);
    REQUIRE(r.sheet->rules.empty());
    REQUIRE(r.sheet->at_rules.empty());
}

TEST_CASE("CSSParser: whitespace-only input",
    "[css_parser][basic]") {
    ArenaAllocator arena;
    auto r = parse(arena, "   \n\t\r\f  ");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules.empty());
}

TEST_CASE("CSSParser: simple rule with one declaration",
    "[css_parser][basic]") {
    ArenaAllocator arena;
    auto r = parse(arena, "body { color: red; }");
    REQUIRE(r.ok());

    auto* sel = first_selector(r.sheet);
    REQUIRE(sel != nullptr);
    REQUIRE(has_simple(sel, SimpleSelector::Kind::Tag, "body"));

    REQUIRE(r.sheet->rules[0].declarations.size() == 1);
    const auto& d = r.sheet->rules[0].declarations[0];
    REQUIRE(prop(d) == "color");
    REQUIRE(val(d) == "red");
    REQUIRE_FALSE(d.important);
}

TEST_CASE("CSSParser: multiple rules",
    "[css_parser][basic]") {
    ArenaAllocator arena;
    auto r = parse(arena,
        "body { color: red; }"
        "p { margin: 0; }"
        "div { padding: 1px; }");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules.size() == 3);
}

TEST_CASE("CSSParser: multiple declarations in one rule",
    "[css_parser][basic]") {
    ArenaAllocator arena;
    auto r = parse(arena,
        "p { color: red; font-size: 16px; text-align: center; }");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules[0].declarations.size() == 3);
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
    auto r = parse(arena, "* { margin: 0; }");
    REQUIRE(has_simple(first_selector(r.sheet),
        SimpleSelector::Kind::Universal, ""));
}

TEST_CASE("CSSParser: class selector",
    "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse(arena, ".foo { color: red; }");
    REQUIRE(has_simple(first_selector(r.sheet),
        SimpleSelector::Kind::Class, "foo"));
}

TEST_CASE("CSSParser: id selector",
    "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse(arena, "#main { color: red; }");
    REQUIRE(has_simple(first_selector(r.sheet),
        SimpleSelector::Kind::Id, "main"));
}

TEST_CASE("CSSParser: compound selector",
    "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse(arena, "div.foo#bar { color: red; }");
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
    auto r = parse(arena, "div p { color: red; }");
    auto* sel = first_selector(r.sheet);
    REQUIRE(sel->compounds.size() == 2);
    REQUIRE(sel->compounds[0].combinator == Combinator::None);
    REQUIRE(sel->compounds[1].combinator == Combinator::Descendant);
}

TEST_CASE("CSSParser: child combinator",
    "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse(arena, "div > p { color: red; }");
    auto* sel = first_selector(r.sheet);
    REQUIRE(sel->compounds.size() == 2);
    REQUIRE(sel->compounds[1].combinator == Combinator::Child);
}

TEST_CASE("CSSParser: adjacent sibling combinator",
    "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse(arena, "h1 + p { margin: 0; }");
    REQUIRE(first_selector(r.sheet)->compounds[1].combinator
        == Combinator::AdjacentSibling);
}

TEST_CASE("CSSParser: general sibling combinator",
    "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse(arena, "h1 ~ p { margin: 0; }");
    REQUIRE(first_selector(r.sheet)->compounds[1].combinator
        == Combinator::GeneralSibling);
}

TEST_CASE("CSSParser: selector list",
    "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse(arena, "h1, h2, h3 { color: red; }");
    REQUIRE(r.sheet->rules[0].selectors.size() == 3);
}

TEST_CASE("CSSParser: attribute selector with equals",
    "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse(arena, R"(a[href="x"] { color: red; })");
    auto* sel = first_selector(r.sheet);
    REQUIRE(has_simple(sel, SimpleSelector::Kind::Attribute, "href"));
    const auto& p = sel->compounds[0].parts[1];
    REQUIRE(sv(p.op) == "=");
    REQUIRE(sv(p.arg) == "x");
}

TEST_CASE("CSSParser: pseudo-class",
    "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse(arena, "a:hover { color: red; }");
    REQUIRE(has_simple(first_selector(r.sheet),
        SimpleSelector::Kind::PseudoClass, "hover"));
}

TEST_CASE("CSSParser: pseudo-class with argument",
    "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse(arena, "li:nth-child(2n+1) { color: red; }");
    bool found = false;
    for (const auto& p : first_selector(r.sheet)->compounds[0].parts) {
        if (p.kind == SimpleSelector::Kind::PseudoClass &&
            sv(p.name) == "nth-child") {
            REQUIRE(p.has_arg);
            REQUIRE(sv(p.arg) == "2n+1");
            found = true;
        }
    }
    REQUIRE(found);
}

TEST_CASE("CSSParser: pseudo-element",
    "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse(arena, "p::before { content: 'x'; }");
    REQUIRE(has_simple(first_selector(r.sheet),
        SimpleSelector::Kind::PseudoElement, "before"));
}


// ============================================================
//  Декларации
// ============================================================

TEST_CASE("CSSParser: !important flag",
    "[css_parser][decl]") {
    ArenaAllocator arena;
    auto r = parse(arena, "p { color: red !important; }");
    REQUIRE(r.sheet->rules[0].declarations[0].important);
}

TEST_CASE("CSSParser: value with spaces trimmed",
    "[css_parser][decl]") {
    ArenaAllocator arena;
    auto r = parse(arena, "p { color:    red   ; }");
    REQUIRE(val(r.sheet->rules[0].declarations[0]) == "red");
}

TEST_CASE("CSSParser: quoted string in value",
    "[css_parser][decl]") {
    ArenaAllocator arena;
    auto r = parse(arena, R"(p::before { content: "hello"; })");
    REQUIRE(val(r.sheet->rules[0].declarations[0]).find("hello")
        != std::string_view::npos);
}

TEST_CASE("CSSParser: calc() keeps parens",
    "[css_parser][decl]") {
    ArenaAllocator arena;
    auto r = parse(arena, "div { width: calc(100% - 20px); }");
    REQUIRE(val(r.sheet->rules[0].declarations[0]).find("calc")
        != std::string_view::npos);
}

TEST_CASE("CSSParser: custom property",
    "[css_parser][decl]") {
    ArenaAllocator arena;
    auto r = parse(arena, ":root { --main-color: red; }");
    const auto& d = r.sheet->rules[0].declarations[0];
    REQUIRE(prop(d) == "--main-color");
    REQUIRE(d.is_custom_property);
}


// ============================================================
//  Shorthand
// ============================================================

TEST_CASE("CSSParser: margin shorthand expands to 4 longhands",
    "[css_parser][shorthand]") {
    ArenaAllocator arena;
    auto r = parse(arena, "p { margin: 0 auto; }");

    const auto& decls = r.sheet->rules[0].declarations;
    REQUIRE(decls.size() == 4);

    auto lookup = [&](std::string_view p) -> std::string_view {
        for (const auto& d : decls) if (prop(d) == p) return val(d);
        return {};
        };
    REQUIRE(lookup("margin-top") == "0");
    REQUIRE(lookup("margin-right") == "auto");
    REQUIRE(lookup("margin-bottom") == "0");
    REQUIRE(lookup("margin-left") == "auto");
}

TEST_CASE("CSSParser: padding shorthand 4 values",
    "[css_parser][shorthand]") {
    ArenaAllocator arena;
    auto r = parse(arena, "p { padding: 1px 2px 3px 4px; }");
    const auto& decls = r.sheet->rules[0].declarations;
    REQUIRE(decls.size() == 4);

    auto lookup = [&](std::string_view p) -> std::string_view {
        for (const auto& d : decls) if (prop(d) == p) return val(d);
        return {};
        };
    REQUIRE(lookup("padding-top") == "1px");
    REQUIRE(lookup("padding-right") == "2px");
    REQUIRE(lookup("padding-bottom") == "3px");
    REQUIRE(lookup("padding-left") == "4px");
}

TEST_CASE("CSSParser: single-value shorthand fills all sides",
    "[css_parser][shorthand]") {
    ArenaAllocator arena;
    auto r = parse(arena, "p { margin: 10px; }");
    const auto& decls = r.sheet->rules[0].declarations;
    REQUIRE(decls.size() == 4);
    for (const auto& d : decls) REQUIRE(val(d) == "10px");
}

TEST_CASE("CSSParser: margin two values",
    "[css_parser][shorthand]") {
    ArenaAllocator arena;
    auto r = parse(arena, "div { margin: 10px 20px; }");
    const auto& d = r.sheet->rules[0].declarations;
    REQUIRE(d.size() == 4);
    REQUIRE(val(d[0]) == "10px");   // top
    REQUIRE(val(d[1]) == "20px");   // right
    REQUIRE(val(d[2]) == "10px");   // bottom
    REQUIRE(val(d[3]) == "20px");   // left
}

TEST_CASE("CSSParser: margin three values",
    "[css_parser][shorthand]") {
    ArenaAllocator arena;
    auto r = parse(arena, "div { margin: 1px 2px 3px; }");
    const auto& d = r.sheet->rules[0].declarations;
    REQUIRE(d.size() == 4);
    REQUIRE(val(d[0]) == "1px");
    REQUIRE(val(d[1]) == "2px");
    REQUIRE(val(d[2]) == "3px");
    REQUIRE(val(d[3]) == "2px");   // left = right
}

TEST_CASE("CSSParser: margin five values — not expanded",
    "[css_parser][shorthand]") {
    ArenaAllocator arena;
    auto r = parse(arena, "div { margin: 1 2 3 4 5; }");
    const auto& d = r.sheet->rules[0].declarations;
    REQUIRE(d.size() == 1);
    REQUIRE(prop(d[0]) == "margin");
}


// ============================================================
//  Комментарии
// ============================================================

TEST_CASE("CSSParser: comment between rules",
    "[css_parser][comment]") {
    ArenaAllocator arena;
    auto r = parse(arena, "/* hello */ body { color: red; }");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules.size() == 1);
}

TEST_CASE("CSSParser: comment inside rule",
    "[css_parser][comment]") {
    ArenaAllocator arena;
    auto r = parse(arena, "p { /* mid */ color: red; }");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules[0].declarations.size() == 1);
}

TEST_CASE("CSSParser: unterminated comment reports error",
    "[css_parser][comment][error]") {
    ArenaAllocator arena;
    auto r = parse(arena, "/* oops");
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.sheet != nullptr);
}

TEST_CASE("CSSParser: multiline comment",
    "[css_parser][comment]") {
    ArenaAllocator arena;
    auto r = parse(arena, "/*\n multi\n line\n*/ div { color: red; }");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules.size() == 1);
}


// ============================================================
//  At-rules
// ============================================================

TEST_CASE("CSSParser: @media with rules",
    "[css_parser][at_rule]") {
    ArenaAllocator arena;
    auto r = parse(arena, "@media (min-width: 100px) { p { color: red; } }");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->at_rules.size() == 1);
    REQUIRE(sv(r.sheet->at_rules[0].name) == "media");
    REQUIRE(r.sheet->at_rules[0].rules.size() == 1);
}

TEST_CASE("CSSParser: @import ends with semicolon",
    "[css_parser][at_rule]") {
    ArenaAllocator arena;
    auto r = parse(arena, R"(@import url("x.css");)");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->at_rules.size() == 1);
    REQUIRE(sv(r.sheet->at_rules[0].name) == "import");
}

TEST_CASE("CSSParser: @font-face with declarations",
    "[css_parser][at_rule]") {
    ArenaAllocator arena;
    auto r = parse(arena,
        "@font-face {\n"
        "  font-family: 'MyFont';\n"
        "  src: url(\"my.woff2\");\n"
        "  font-weight: 400;\n"
        "}");
    REQUIRE(r.ok());
    const auto& at = r.sheet->at_rules[0];
    REQUIRE(sv(at.name) == "font-face");
    REQUIRE(at.declarations.size() == 3);
    REQUIRE(at.rules.empty());
}

TEST_CASE("CSSParser: @keyframes parsed with rules",
    "[css_parser][at_rule][keyframes]") {
    ArenaAllocator arena;
    auto r = parse(arena,
        "@keyframes spin {\n"
        "  from { transform: rotate(0deg); }\n"
        "  to   { transform: rotate(360deg); }\n"
        "}");
    REQUIRE(r.ok());
    const auto& at = r.sheet->at_rules[0];
    REQUIRE(sv(at.name) == "keyframes");
    REQUIRE(sv(at.prelude) == "spin");
    REQUIRE(at.rules.size() == 2);
}

TEST_CASE("CSSParser: @supports nested inside @media",
    "[css_parser][at_rule][nested]") {
    ArenaAllocator arena;
    auto r = parse(arena,
        "@media screen {\n"
        "  @supports (display: flex) {\n"
        "    .row { display: flex; }\n"
        "  }\n"
        "}");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->at_rules.size() == 1);
    const auto& media = r.sheet->at_rules[0];
    REQUIRE(sv(media.name) == "media");
    REQUIRE(media.rules.empty());
    REQUIRE(media.nested_at_rules.size() == 1);
    REQUIRE(sv(media.nested_at_rules[0].name) == "supports");
}


// ============================================================
//  Escape-последовательности
// ============================================================

TEST_CASE("CSSParser: hex escape in identifier",
    "[css_parser][escape]") {
    ArenaAllocator arena;
    auto r = parse(arena, ".\\41 bc { color: red; }");  // \41 = 'A'
    REQUIRE(r.ok());
    REQUIRE(has_simple(first_selector(r.sheet),
        SimpleSelector::Kind::Class, "Abc"));
}

TEST_CASE("CSSParser: escape at start of class name",
    "[css_parser][escape]") {
    ArenaAllocator arena;
    auto r = parse(arena, ".\\31 23 { color: red; }");  // \31 = '1'
    REQUIRE(r.ok());
    REQUIRE(has_simple(first_selector(r.sheet),
        SimpleSelector::Kind::Class, "123"));
}

TEST_CASE("CSSParser: escaped non-hex char in identifier",
    "[css_parser][escape]") {
    ArenaAllocator arena;
    auto r = parse(arena, ".\\!important { color: red; }");
    REQUIRE(r.ok());
    REQUIRE(has_simple(first_selector(r.sheet),
        SimpleSelector::Kind::Class, "!important"));
}


// ============================================================
//  Устойчивость
// ============================================================

TEST_CASE("CSSParser: unbalanced brace doesn't crash",
    "[css_parser][robust]") {
    ArenaAllocator arena;
    auto r = parse(arena, "p { color: red;");
    REQUIRE(r.sheet != nullptr);
}

TEST_CASE("CSSParser: stray closing brace",
    "[css_parser][robust]") {
    ArenaAllocator arena;
    auto r = parse(arena, "} p { color: red; }");
    REQUIRE(r.sheet != nullptr);
}

TEST_CASE("CSSParser: at-rule inside rule block",
    "[css_parser][robust]") {
    ArenaAllocator arena;
    auto r = parse(arena, "p { @media (min-width: 100px) { color: red; } }");
    REQUIRE(r.sheet != nullptr);
}

TEST_CASE("CSSParser: 1000 rules in one sheet",
    "[css_parser][robust]") {
    ArenaAllocator arena;
    std::string css;
    for (int i = 0; i < 1000; ++i)
        css += ".c" + std::to_string(i) + " { color: red; }\n";
    auto r = parse(arena, css);
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules.size() == 1000);
}

TEST_CASE("CSSParser: very long class name",
    "[css_parser][robust]") {
    ArenaAllocator arena;
    std::string long_name(10000, 'a');
    auto r = parse(arena, "." + long_name + " { color: red; }");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules[0].selectors[0].compounds[0].parts[0].name.size()
        == 10000);
}


// ============================================================
//  Интеграция
// ============================================================

TEST_CASE("CSSParser: realistic stylesheet",
    "[css_parser][integration]") {
    ArenaAllocator arena;
    const std::string css = R"CSS(
        html, body { display: block; margin: 0; padding: 0; }
        body {
            background-color: #f0f0f0;
            font-size: 16px;
            color: #222222;
        }
        .card {
            display: block;
            margin: 24px;
            padding: 24px;
            background-color: #ffffff;
            border-top-width: 4px;
            border-left-width: 4px;
            width: 640px;
        }
        h1 { display: block; font-size: 32px; color: #1a1a1a; }
        @media (max-width: 600px) {
            .card { width: 100%; margin: 8px; }
        }
    )CSS";

    auto r = parse(arena, css);
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules.size() == 4);
    REQUIRE(r.sheet->at_rules.size() == 1);
    REQUIRE(sv(r.sheet->at_rules[0].name) == "media");
}