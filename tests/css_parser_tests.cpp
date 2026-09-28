#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <string>
#include <vector>

#include "../parsers/css_parser/headers/css_parser.h"
#include "../parsers/css_parser/headers/css_dom.h"
#include "../parsers/arena_memory_allocator/headers/arena.h"

// ============================================================
//  Утилиты для краткости тестов
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

    inline std::string simple_tag(const ComplexSelector& cs) {
        if (cs.compounds.size() != 1 || cs.compounds[0].parts.size() != 1) return "";
        const auto& p = cs.compounds[0].parts[0];
        return (p.kind == SimpleSelector::Kind::Tag) ? p.name : std::string{};
    }

} // namespace

TEST_CASE("CSSParser: empty input produces empty sheet", "[css_parser][basic]") {
    ArenaAllocator arena;
    auto r = parse(arena, "");
    REQUIRE(r.sheet != nullptr);
    REQUIRE(r.sheet->rules.empty());
    REQUIRE(r.sheet->at_rules.empty());
    REQUIRE(r.ok());
}

TEST_CASE("CSSParser: whitespace-only input", "[css_parser][basic]") {
    ArenaAllocator arena;
    auto r = parse(arena, "   \t\n\r\f  ");
    REQUIRE(r.sheet->rules.empty());
    REQUIRE(r.ok());
}

TEST_CASE("CSSParser: single tag rule", "[css_parser][basic]") {
    ArenaAllocator arena;
    auto r = parse(arena, "div { color: red; }");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules.size() == 1);

    const auto& rule = r.sheet->rules[0];
    REQUIRE(rule.selectors.size() == 1);
    REQUIRE(rule.selectors[0].compounds.size() == 1);
    REQUIRE(rule.selectors[0].compounds[0].parts.size() == 1);
    REQUIRE(rule.selectors[0].compounds[0].parts[0].kind == SimpleSelector::Kind::Tag);
    REQUIRE(rule.selectors[0].compounds[0].parts[0].name == "div");

    REQUIRE(rule.declarations.size() == 1);
    REQUIRE(rule.declarations[0].property == "color");
    REQUIRE(rule.declarations[0].value == "red");
    REQUIRE_FALSE(rule.declarations[0].important);
}

TEST_CASE("CSSParser: empty rule body", "[css_parser][basic]") {
    ArenaAllocator arena;
    auto r = parse(arena, "div {}");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules.size() == 1);
    REQUIRE(r.sheet->rules[0].selectors.size() == 1);
    REQUIRE(r.sheet->rules[0].declarations.empty());
}

TEST_CASE("CSSParser: multiple declarations", "[css_parser][basic]") {
    ArenaAllocator arena;
    auto r = parse(arena, "p { color: red; font-size: 16px; font-weight: bold; }");
    REQUIRE(r.ok());
    const auto& d = r.sheet->rules[0].declarations;
    REQUIRE(d.size() == 3);
    REQUIRE(d[0].property == "color");      REQUIRE(d[0].value == "red");
    REQUIRE(d[1].property == "font-size");  REQUIRE(d[1].value == "16px");
    REQUIRE(d[2].property == "font-weight");REQUIRE(d[2].value == "bold");
}

TEST_CASE("CSSParser: missing final semicolon", "[css_parser][basic]") {
    ArenaAllocator arena;
    auto r = parse(arena, "p { color: red }");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules[0].declarations.size() == 1);
    REQUIRE(r.sheet->rules[0].declarations[0].value == "red");
}

TEST_CASE("CSSParser: multiple rules in sequence", "[css_parser][basic]") {
    ArenaAllocator arena;
    auto r = parse(arena,
        "html { display: block; }"
        "body { margin: 0; }"
        ".foo { color: blue; }");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules.size() == 3);
    REQUIRE(simple_tag(r.sheet->rules[0].selectors[0]) == "html");
    REQUIRE(simple_tag(r.sheet->rules[1].selectors[0]) == "body");
    REQUIRE(r.sheet->rules[2].selectors[0].compounds[0].parts[0].kind ==
        SimpleSelector::Kind::Class);
}


TEST_CASE("CSSParser: universal selector", "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse(arena, "* { box-sizing: border-box; }");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules[0].selectors[0].compounds[0].parts[0].kind ==
        SimpleSelector::Kind::Universal);
}

TEST_CASE("CSSParser: class selector", "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse(arena, ".button { color: blue; }");
    const auto& p = r.sheet->rules[0].selectors[0].compounds[0].parts[0];
    REQUIRE(p.kind == SimpleSelector::Kind::Class);
    REQUIRE(p.name == "button");
}

TEST_CASE("CSSParser: id selector", "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse(arena, "#header { height: 60px; }");
    const auto& p = r.sheet->rules[0].selectors[0].compounds[0].parts[0];
    REQUIRE(p.kind == SimpleSelector::Kind::Id);
    REQUIRE(p.name == "header");
}

TEST_CASE("CSSParser: tag with dash and underscore", "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse(arena, "my-element { color: red; } _foo { color: blue; }");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules[0].selectors[0].compounds[0].parts[0].name == "my-element");
    REQUIRE(r.sheet->rules[1].selectors[0].compounds[0].parts[0].name == "_foo");
}

TEST_CASE("CSSParser: compound selector div.cls#id", "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse(arena, "div.btn#primary { color: red; }");
    const auto& parts = r.sheet->rules[0].selectors[0].compounds[0].parts;
    REQUIRE(parts.size() == 3);
    REQUIRE(parts[0].kind == SimpleSelector::Kind::Tag);   REQUIRE(parts[0].name == "div");
    REQUIRE(parts[1].kind == SimpleSelector::Kind::Class); REQUIRE(parts[1].name == "btn");
    REQUIRE(parts[2].kind == SimpleSelector::Kind::Id);    REQUIRE(parts[2].name == "primary");
}

TEST_CASE("CSSParser: multiple classes on one compound", "[css_parser][selector]") {
    ArenaAllocator arena;
    auto r = parse(arena, ".btn.primary.big { color: red; }");
    const auto& parts = r.sheet->rules[0].selectors[0].compounds[0].parts;
    REQUIRE(parts.size() == 3);
    REQUIRE(parts[0].name == "btn");
    REQUIRE(parts[1].name == "primary");
    REQUIRE(parts[2].name == "big");
}



TEST_CASE("CSSParser: descendant combinator", "[css_parser][combinator]") {
    ArenaAllocator arena;
    auto r = parse(arena, "div p { color: red; }");
    REQUIRE(r.ok());
    const auto& cs = r.sheet->rules[0].selectors[0];
    REQUIRE(cs.compounds.size() == 2);
    REQUIRE(cs.compounds[1].combinator == Combinator::Descendant);
}

TEST_CASE("CSSParser: child combinator >", "[css_parser][combinator]") {
    ArenaAllocator arena;
    auto r = parse(arena, "div > p { color: red; }");
    const auto& cs = r.sheet->rules[0].selectors[0];
    REQUIRE(cs.compounds.size() == 2);
    REQUIRE(cs.compounds[1].combinator == Combinator::Child);
}

TEST_CASE("CSSParser: adjacent sibling +", "[css_parser][combinator]") {
    ArenaAllocator arena;
    auto r = parse(arena, "h1 + p { margin: 0; }");
    const auto& cs = r.sheet->rules[0].selectors[0];
    REQUIRE(cs.compounds.size() == 2);
    REQUIRE(cs.compounds[1].combinator == Combinator::AdjacentSibling);
}

TEST_CASE("CSSParser: general sibling ~", "[css_parser][combinator]") {
    ArenaAllocator arena;
    auto r = parse(arena, "h1 ~ p { margin: 0; }");
    const auto& cs = r.sheet->rules[0].selectors[0];
    REQUIRE(cs.compounds.size() == 2);
    REQUIRE(cs.compounds[1].combinator == Combinator::GeneralSibling);
}

TEST_CASE("CSSParser: mixed combinators chain", "[css_parser][combinator]") {
    ArenaAllocator arena;
    auto r = parse(arena, "main > div .item + span ~ a { color: red; }");
    const auto& cs = r.sheet->rules[0].selectors[0];
    REQUIRE(cs.compounds.size() == 5);
    REQUIRE(cs.compounds[1].combinator == Combinator::Child);
    REQUIRE(cs.compounds[2].combinator == Combinator::Descendant);
    REQUIRE(cs.compounds[3].combinator == Combinator::AdjacentSibling);
    REQUIRE(cs.compounds[4].combinator == Combinator::GeneralSibling);
}

TEST_CASE("CSSParser: whitespace around child combinator", "[css_parser][combinator]") {
    ArenaAllocator arena;
    auto r = parse(arena, "div   >   p { color: red; }");
    const auto& cs = r.sheet->rules[0].selectors[0];
    REQUIRE(cs.compounds.size() == 2);
    REQUIRE(cs.compounds[1].combinator == Combinator::Child);
}


TEST_CASE("CSSParser: comma-separated selectors", "[css_parser][selector_list]") {
    ArenaAllocator arena;
    auto r = parse(arena, "h1, h2, h3 { font-weight: bold; }");
    REQUIRE(r.ok());
    const auto& sel = r.sheet->rules[0].selectors;
    REQUIRE(sel.size() == 3);
    REQUIRE(simple_tag(sel[0]) == "h1");
    REQUIRE(simple_tag(sel[1]) == "h2");
    REQUIRE(simple_tag(sel[2]) == "h3");
}

TEST_CASE("CSSParser: comma-separated complex selectors", "[css_parser][selector_list]") {
    ArenaAllocator arena;
    auto r = parse(arena, "div > p, .box, #main { color: red; }");
    const auto& sel = r.sheet->rules[0].selectors;
    REQUIRE(sel.size() == 3);
    REQUIRE(sel[0].compounds.size() == 2);
    REQUIRE(sel[1].compounds[0].parts[0].kind == SimpleSelector::Kind::Class);
    REQUIRE(sel[2].compounds[0].parts[0].kind == SimpleSelector::Kind::Id);
}

TEST_CASE("CSSParser: trailing comma before brace", "[css_parser][selector_list]") {
    ArenaAllocator arena;
    auto r = parse(arena, "h1, h2, { font-weight: bold; }");
    // парсер должен съесть пустой третий selector без падения
    REQUIRE(r.sheet->rules.size() == 1);
    REQUIRE(r.sheet->rules[0].selectors.size() == 2);
}


TEST_CASE("CSSParser: attribute presence", "[css_parser][attr]") {
    ArenaAllocator arena;
    auto r = parse(arena, "[disabled] { opacity: 0.5; }");
    const auto& p = r.sheet->rules[0].selectors[0].compounds[0].parts[0];
    REQUIRE(p.kind == SimpleSelector::Kind::Attribute);
    REQUIRE(p.name == "disabled");
    REQUIRE(p.op.empty());
    REQUIRE(p.arg.empty());
}

TEST_CASE("CSSParser: attribute equals", "[css_parser][attr]") {
    ArenaAllocator arena;
    auto r = parse(arena, "[type=\"text\"] { color: red; }");
    const auto& p = r.sheet->rules[0].selectors[0].compounds[0].parts[0];
    REQUIRE(p.kind == SimpleSelector::Kind::Attribute);
    REQUIRE(p.name == "type");
    REQUIRE(p.op == "=");
    REQUIRE(p.arg == "text");
}

TEST_CASE("CSSParser: attribute equals with single quotes", "[css_parser][attr]") {
    ArenaAllocator arena;
    auto r = parse(arena, "[type='text'] { color: red; }");
    const auto& p = r.sheet->rules[0].selectors[0].compounds[0].parts[0];
    REQUIRE(p.op == "=");
    REQUIRE(p.arg == "text");
}

TEST_CASE("CSSParser: attribute equals unquoted", "[css_parser][attr]") {
    ArenaAllocator arena;
    auto r = parse(arena, "[type=text] { color: red; }");
    const auto& p = r.sheet->rules[0].selectors[0].compounds[0].parts[0];
    REQUIRE(p.op == "=");
    REQUIRE(p.arg == "text");
}

TEST_CASE("CSSParser: attribute ~= operator", "[css_parser][attr]") {
    ArenaAllocator arena;
    auto r = parse(arena, "[class~=foo] { color: red; }");
    const auto& p = r.sheet->rules[0].selectors[0].compounds[0].parts[0];
    REQUIRE(p.op == "~=");
    REQUIRE(p.arg == "foo");
}

TEST_CASE("CSSParser: attribute |= operator", "[css_parser][attr]") {
    ArenaAllocator arena;
    auto r = parse(arena, "[lang|=en] { color: red; }");
    const auto& p = r.sheet->rules[0].selectors[0].compounds[0].parts[0];
    REQUIRE(p.op == "|=");
    REQUIRE(p.arg == "en");
}

TEST_CASE("CSSParser: attribute ^= operator", "[css_parser][attr]") {
    ArenaAllocator arena;
    auto r = parse(arena, "[href^=\"https://\"] { color: red; }");
    const auto& p = r.sheet->rules[0].selectors[0].compounds[0].parts[0];
    REQUIRE(p.op == "^=");
    REQUIRE(p.arg == "https://");
}

TEST_CASE("CSSParser: attribute $= operator", "[css_parser][attr]") {
    ArenaAllocator arena;
    auto r = parse(arena, "[href$=\".png\"] { color: red; }");
    const auto& p = r.sheet->rules[0].selectors[0].compounds[0].parts[0];
    REQUIRE(p.op == "$=");
    REQUIRE(p.arg == ".png");
}

TEST_CASE("CSSParser: attribute *= operator", "[css_parser][attr]") {
    ArenaAllocator arena;
    auto r = parse(arena, "[href*=\"example\"] { color: red; }");
    const auto& p = r.sheet->rules[0].selectors[0].compounds[0].parts[0];
    REQUIRE(p.op == "*=");
    REQUIRE(p.arg == "example");
}

TEST_CASE("CSSParser: attribute with whitespace inside brackets", "[css_parser][attr]") {
    ArenaAllocator arena;
    auto r = parse(arena, "[ type = \"text\" ] { color: red; }");
    REQUIRE(r.ok());
    const auto& p = r.sheet->rules[0].selectors[0].compounds[0].parts[0];
    REQUIRE(p.name == "type");
    REQUIRE(p.op == "=");
    REQUIRE(p.arg == "text");
}

TEST_CASE("CSSParser: attribute as part of compound", "[css_parser][attr]") {
    ArenaAllocator arena;
    auto r = parse(arena, "input[type=\"text\"].form-input { color: red; }");
    const auto& parts = r.sheet->rules[0].selectors[0].compounds[0].parts;
    REQUIRE(parts.size() == 3);
    REQUIRE(parts[0].kind == SimpleSelector::Kind::Tag);
    REQUIRE(parts[1].kind == SimpleSelector::Kind::Attribute);
    REQUIRE(parts[2].kind == SimpleSelector::Kind::Class);
}


TEST_CASE("CSSParser: simple pseudo-class", "[css_parser][pseudo]") {
    ArenaAllocator arena;
    auto r = parse(arena, "a:hover { color: red; }");
    const auto& parts = r.sheet->rules[0].selectors[0].compounds[0].parts;
    REQUIRE(parts.size() == 2);
    REQUIRE(parts[1].kind == SimpleSelector::Kind::PseudoClass);
    REQUIRE(parts[1].name == "hover");
    REQUIRE_FALSE(parts[1].has_arg);
}

TEST_CASE("CSSParser: pseudo-class with arg", "[css_parser][pseudo]") {
    ArenaAllocator arena;
    auto r = parse(arena, "li:nth-child(2n+1) { color: red; }");
    const auto& p = r.sheet->rules[0].selectors[0].compounds[0].parts[1];
    REQUIRE(p.kind == SimpleSelector::Kind::PseudoClass);
    REQUIRE(p.name == "nth-child");
    REQUIRE(p.has_arg);
    REQUIRE(p.arg == "2n+1");
}

TEST_CASE("CSSParser: pseudo-class arg with nested parens", "[css_parser][pseudo]") {
    ArenaAllocator arena;
    auto r = parse(arena, "p:not(:not(.foo)) { color: red; }");
    const auto& p = r.sheet->rules[0].selectors[0].compounds[0].parts[1];
    REQUIRE(p.name == "not");
    REQUIRE(p.has_arg);
    REQUIRE(p.arg == ":not(.foo)");
}

TEST_CASE("CSSParser: pseudo-element ::before", "[css_parser][pseudo]") {
    ArenaAllocator arena;
    auto r = parse(arena, "p::before { content: \"→\"; }");
    const auto& p = r.sheet->rules[0].selectors[0].compounds[0].parts[1];
    REQUIRE(p.kind == SimpleSelector::Kind::PseudoElement);
    REQUIRE(p.name == "before");
}

TEST_CASE("CSSParser: single-colon pseudo-element legacy :before", "[css_parser][pseudo]") {
    ArenaAllocator arena;
    auto r = parse(arena, "p:before { content: \"→\"; }");
    const auto& p = r.sheet->rules[0].selectors[0].compounds[0].parts[1];
    // парсер трактует одиночное ':' как PseudoClass — это ожидаемое поведение
    REQUIRE(p.kind == SimpleSelector::Kind::PseudoClass);
    REQUIRE(p.name == "before");
}

TEST_CASE("CSSParser: multiple pseudo-classes in compound", "[css_parser][pseudo]") {
    ArenaAllocator arena;
    auto r = parse(arena, "a:link:visited { color: red; }");
    const auto& parts = r.sheet->rules[0].selectors[0].compounds[0].parts;
    REQUIRE(parts.size() == 3);
    REQUIRE(parts[1].name == "link");
    REQUIRE(parts[2].name == "visited");
}


TEST_CASE("CSSParser: !important flag", "[css_parser][declaration]") {
    ArenaAllocator arena;
    auto r = parse(arena, "div { color: red !important; }");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules[0].declarations[0].important);
    REQUIRE(r.sheet->rules[0].declarations[0].value == "red");
}

TEST_CASE("CSSParser: !important with whitespace between ! and important",
    "[css_parser][declaration]") {
    ArenaAllocator arena;
    auto r = parse(arena, "div { color: red !  important ; }");
    REQUIRE(r.sheet->rules[0].declarations[0].important);
}

TEST_CASE("CSSParser: !IMPORTANT case-insensitive", "[css_parser][declaration]") {
    ArenaAllocator arena;
    auto r = parse(arena, "div { color: red !IMPORTANT; }");
    REQUIRE(r.sheet->rules[0].declarations[0].important);
}

TEST_CASE("CSSParser: rgb() value with commas and parens", "[css_parser][declaration]") {
    ArenaAllocator arena;
    auto r = parse(arena, "div { color: rgb(255, 128, 0); }");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules[0].declarations.size() == 1);
    REQUIRE(r.sheet->rules[0].declarations[0].value == "rgb(255, 128, 0)");
}

TEST_CASE("CSSParser: rgba() with 4 args", "[css_parser][declaration]") {
    ArenaAllocator arena;
    auto r = parse(arena, "div { color: rgba(255,128,0,0.5); }");
    REQUIRE(r.sheet->rules[0].declarations[0].value == "rgba(255,128,0,0.5)");
}

TEST_CASE("CSSParser: url() value", "[css_parser][declaration]") {
    ArenaAllocator arena;
    auto r = parse(arena, "body { background: url(\"img/bg.png\") no-repeat; }");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules[0].declarations.size() == 1);
    REQUIRE(r.sheet->rules[0].declarations[0].value ==
        "url(\"img/bg.png\") no-repeat");
}

TEST_CASE("CSSParser: value with string containing semicolon", "[css_parser][declaration]") {
    ArenaAllocator arena;
    auto r = parse(arena, "p::after { content: \"hello; world\"; }");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules[0].declarations[0].value == "\"hello; world\"");
}

TEST_CASE("CSSParser: value with escaped quote", "[css_parser][declaration]") {
    ArenaAllocator arena;
    auto r = parse(arena, "p::after { content: \"a\\\"b\"; }");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules[0].declarations[0].value == "\"a\\\"b\"");
}

TEST_CASE("CSSParser: negative and floating values", "[css_parser][declaration]") {
    ArenaAllocator arena;
    auto r = parse(arena, "div { margin-top: -12.5px; z-index: -1; }");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules[0].declarations[0].value == "-12.5px");
    REQUIRE(r.sheet->rules[0].declarations[1].value == "-1");
}

TEST_CASE("CSSParser: percentage values", "[css_parser][declaration]") {
    ArenaAllocator arena;
    auto r = parse(arena, "div { width: 50%; padding: 5.5%; }");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules[0].declarations[0].value == "50%");
    REQUIRE(r.sheet->rules[0].declarations[1].value == "5.5%");
}

TEST_CASE("CSSParser: multi-token value preserved for non-shorthand",
    "[css_parser][declaration]") {
    ArenaAllocator arena;
    auto r = parse(arena, "a { text-decoration: underline dotted red; }");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules[0].declarations.size() == 1);
    REQUIRE(r.sheet->rules[0].declarations[0].value ==
        "underline dotted red");
}

TEST_CASE("CSSParser: margin 4 values expand to 4 longhands",
    "[css_parser][shorthand]") {
    ArenaAllocator arena;
    auto r = parse(arena, "div { margin: 10px 20px 30px 40px; }");
    REQUIRE(r.ok());
    const auto& d = r.sheet->rules[0].declarations;
    REQUIRE(d.size() == 4);
    REQUIRE(d[0].property == "margin-top");    REQUIRE(d[0].value == "10px");
    REQUIRE(d[1].property == "margin-right");  REQUIRE(d[1].value == "20px");
    REQUIRE(d[2].property == "margin-bottom"); REQUIRE(d[2].value == "30px");
    REQUIRE(d[3].property == "margin-left");   REQUIRE(d[3].value == "40px");
}

TEST_CASE("CSSParser: value with embedded comment", "[css_parser][declaration]") {
    ArenaAllocator arena;
    auto r = parse(arena, "div { color: red /* comment */ ; }");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules[0].declarations[0].value == "red");
}

TEST_CASE("CSSParser: custom property --foo", "[css_parser][declaration]") {
    ArenaAllocator arena;
    auto r = parse(arena, ":root { --brand-color: #3498db; }");
    REQUIRE(r.ok());
    const auto& d = r.sheet->rules[0].declarations[0];
    REQUIRE(d.property == "--brand-color");
    REQUIRE(d.value == "#3498db");
    REQUIRE(d.is_custom_property);
}


TEST_CASE("CSSParser: comments stripped between rules", "[css_parser][comment]") {
    ArenaAllocator arena;
    auto r = parse(arena,
        "/* header */\n"
        "div { color: red; }\n"
        "/* footer */");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules.size() == 1);
}

TEST_CASE("CSSParser: comment inside a rule", "[css_parser][comment]") {
    ArenaAllocator arena;
    auto r = parse(arena, "div { /* a */ color: /* b */ red; }");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules[0].declarations.size() == 1);
    REQUIRE(r.sheet->rules[0].declarations[0].value == "red");
}

TEST_CASE("CSSParser: multiline comment", "[css_parser][comment]") {
    ArenaAllocator arena;
    auto r = parse(arena, "/*\n multi\n line\n*/ div { color: red; }");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules.size() == 1);
}

TEST_CASE("CSSParser: unterminated comment reports error", "[css_parser][comment][error]") {
    ArenaAllocator arena;
    auto r = parse(arena, "div { color: red; } /* never closed");
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.sheet->rules.size() == 1);  // правило успело распарситься
}


TEST_CASE("CSSParser: @import with url string", "[css_parser][at_rule]") {
    ArenaAllocator arena;
    auto r = parse(arena, "@import url(\"reset.css\");");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->at_rules.size() == 1);
    const auto& at = r.sheet->at_rules[0];
    REQUIRE(at.name == "import");
    REQUIRE(at.prelude == "url(\"reset.css\")");
    REQUIRE(at.rules.empty());
    REQUIRE(at.declarations.empty());
}

TEST_CASE("CSSParser: @charset with quotes", "[css_parser][at_rule]") {
    ArenaAllocator arena;
    auto r = parse(arena, "@charset \"utf-8\";");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->at_rules[0].name == "charset");
    REQUIRE(r.sheet->at_rules[0].prelude == "\"utf-8\"");
}

TEST_CASE("CSSParser: @media with nested rules", "[css_parser][at_rule]") {
    ArenaAllocator arena;
    auto r = parse(arena,
        "@media (max-width: 600px) {\n"
        "  .card { padding: 12px; }\n"
        "  h1 { font-size: 24px; }\n"
        "}");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->at_rules.size() == 1);
    const auto& at = r.sheet->at_rules[0];
    REQUIRE(at.name == "media");
    REQUIRE(at.prelude == "(max-width: 600px)");
    REQUIRE(at.rules.size() == 2);
    REQUIRE(at.rules[0].selectors.size() == 1);
    REQUIRE(at.rules[0].declarations.size() == 4);
    REQUIRE(at.rules[0].declarations[0].property == "padding-top");
    REQUIRE(at.rules[0].declarations[0].value == "12px");
}

TEST_CASE("CSSParser: @supports with nested rules", "[css_parser][at_rule]") {
    ArenaAllocator arena;
    auto r = parse(arena,
        "@supports (display: grid) {\n"
        "  .grid { display: grid; }\n"
        "}");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->at_rules[0].name == "supports");
    REQUIRE(r.sheet->at_rules[0].rules.size() == 1);
}

TEST_CASE("CSSParser: @font-face with declarations only", "[css_parser][at_rule]") {
    ArenaAllocator arena;
    auto r = parse(arena,
        "@font-face {\n"
        "  font-family: 'MyFont';\n"
        "  src: url(\"my.woff2\");\n"
        "  font-weight: 400;\n"
        "}");
    REQUIRE(r.ok());
    const auto& at = r.sheet->at_rules[0];
    REQUIRE(at.name == "font-face");
    REQUIRE(at.declarations.size() == 3);
    REQUIRE(at.declarations[0].property == "font-family");
    REQUIRE(at.declarations[0].value == "'MyFont'");
    REQUIRE(at.rules.empty());
}

TEST_CASE("CSSParser: @page with declarations", "[css_parser][at_rule]") {
    ArenaAllocator arena;
    auto r = parse(arena, "@page { margin: 2cm; }");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->at_rules[0].name == "page");
    REQUIRE(r.sheet->at_rules[0].declarations.size() == 4);
    REQUIRE(r.sheet->at_rules[0].declarations[0].property == "margin-top");
    REQUIRE(r.sheet->at_rules[0].declarations[0].value == "2cm");
    REQUIRE(r.sheet->at_rules[0].declarations[3].property == "margin-left");
}

TEST_CASE("CSSParser: @media with nested @supports", "[css_parser][at_rule]") {
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
    REQUIRE(media.name == "media");
    REQUIRE(media.rules.empty());
    REQUIRE(media.nested_at_rules.size() == 1);
    REQUIRE(media.nested_at_rules[0].name == "supports");
}

TEST_CASE("CSSParser: @import plus rules", "[css_parser][at_rule]") {
    ArenaAllocator arena;
    auto r = parse(arena,
        "@import \"base.css\";\n"
        "body { color: #000; }");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->at_rules.size() == 1);
    REQUIRE(r.sheet->rules.size() == 1);
}

TEST_CASE("CSSParser: @at-rule name is lowercased", "[css_parser][at_rule]") {
    ArenaAllocator arena;
    auto r = parse(arena, "@MEDIA screen { div { color: red; } }");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->at_rules[0].name == "media");
}

TEST_CASE("CSSParser: hex escape in identifier", "[css_parser][escape]") {
    ArenaAllocator arena;
    // \41 = 'A' (латинская)
    auto r = parse(arena, ".\\41 bc { color: red; }");
    const auto& p = r.sheet->rules[0].selectors[0].compounds[0].parts[0];
    REQUIRE(p.kind == SimpleSelector::Kind::Class);
    REQUIRE(p.name == "Abc");
}

TEST_CASE("CSSParser: escaped non-hex char in identifier",
    "[css_parser][escape]") {
    ArenaAllocator arena;
    auto r = parse(arena, ".\\!important { color: red; }");
    const auto& p = r.sheet->rules[0].selectors[0].compounds[0].parts[0];
    REQUIRE(p.kind == SimpleSelector::Kind::Class);
    REQUIRE(p.name == "!important");
}

TEST_CASE("CSSParser: hex escape with trailing space consumed",
    "[css_parser][escape]") {
    ArenaAllocator arena;
    // "\41 bc" → 'A' + "bc" = "Abc" (пробел после escape съедается)
    auto r = parse(arena, ".\\41 bc { color: red; }");
    const auto& p = r.sheet->rules[0].selectors[0].compounds[0].parts[0];
    REQUIRE(p.name == "Abc");
}


TEST_CASE("CSSParser: unterminated string reports error", "[css_parser][error]") {
    ArenaAllocator arena;
    auto r = parse(arena, "div { content: \"unterminated }");
    REQUIRE_FALSE(r.ok());
}

TEST_CASE("CSSParser: stray } is reported and skipped", "[css_parser][error]") {
    ArenaAllocator arena;
    auto r = parse(arena, "div { color: red; } } body { margin: 0; }");
    REQUIRE_FALSE(r.ok());
    // второе правило всё равно распарсилось
    REQUIRE(r.sheet->rules.size() == 2);
    REQUIRE(simple_tag(r.sheet->rules[1].selectors[0]) == "body");
}

TEST_CASE("CSSParser: missing { after selector", "[css_parser][error]") {
    ArenaAllocator arena;
    auto r = parse(arena, "div color: red; }");
    REQUIRE_FALSE(r.ok());
}

TEST_CASE("CSSParser: invalid attribute operator still advances",
    "[css_parser][error]") {
    ArenaAllocator arena;
    auto r = parse(arena, "[a!=b] { color: red; }");
    // парсер должен зафиксировать ошибку и не зациклиться
    REQUIRE_FALSE(r.ok());
}

TEST_CASE("CSSParser: empty at-rule prelude tolerated", "[css_parser][error]") {
    ArenaAllocator arena;
    auto r = parse(arena, "@media { div { color: red; } }");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->at_rules[0].name == "media");
    REQUIRE(r.sheet->at_rules[0].prelude.empty());
}


TEST_CASE("CSSParser: realistic stylesheet", "[css_parser][integration]") {
    ArenaAllocator arena;
    const std::string css = R"CSS(
        /* reset */
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

        h1 { display: block; font-size: 32px; color: #1a1a1a; margin-bottom: 12px; }
        p  { display: block; font-size: 16px; color: #444444; margin-bottom: 16px; }
        button { display: inline-block; padding: 8px 16px;
                 background-color: #3498db; color: #ffffff; font-size: 14px; }

        @media (max-width: 600px) {
            .card { width: 100%; margin: 8px; }
        }
    )CSS";

    auto r = parse(arena, css);
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules.size() == 6);
    REQUIRE(r.sheet->at_rules.size() == 1);
    REQUIRE(r.sheet->at_rules[0].name == "media");

    // Правило ".card": margin, padding, bg, border-top, border-left, width
    const auto& card_rule = r.sheet->rules[2];
    REQUIRE(card_rule.selectors[0].compounds[0].parts[0].kind ==
        SimpleSelector::Kind::Class);
    REQUIRE(card_rule.declarations.size() == 13);
    REQUIRE(card_rule.declarations[0].property == "display");
    // margin развернулся в 1..4
    REQUIRE(card_rule.declarations[1].property == "margin-top");
    REQUIRE(card_rule.declarations[1].value == "24px");
    REQUIRE(card_rule.declarations[2].property == "margin-right");
    REQUIRE(card_rule.declarations[3].property == "margin-bottom");
    REQUIRE(card_rule.declarations[4].property == "margin-left");
    // padding развернулся в 5..8
    REQUIRE(card_rule.declarations[5].property == "padding-top");
    REQUIRE(card_rule.declarations[8].property == "padding-left");
    // далее исходные свойства
    REQUIRE(card_rule.declarations[9].property == "background-color");
    REQUIRE(card_rule.declarations[10].property == "border-top-width");
    REQUIRE(card_rule.declarations[10].value == "4px");
    REQUIRE(card_rule.declarations[11].property == "border-left-width");
    REQUIRE(card_rule.declarations[12].property == "width");

}

TEST_CASE("CSSParser: two rules with same selector do not overwrite",
    "[css_parser][integration]") {
    ArenaAllocator arena;
    auto r = parse(arena,
        ".btn { color: red; }"
        ".btn { color: blue; }");
    REQUIRE(r.ok());
    REQUIRE(r.sheet->rules.size() == 2);
    REQUIRE(r.sheet->rules[0].declarations[0].value == "red");
    REQUIRE(r.sheet->rules[1].declarations[0].value == "blue");
}

TEST_CASE("CSSParser: parser is reusable across calls",
    "[css_parser][integration]") {
    ArenaAllocator arena;
    CSSParser p(arena);

    StyleSheet* a = p.parse("div { color: red; }");
    REQUIRE(p.errors().empty());
    REQUIRE(a->rules.size() == 1);

    StyleSheet* b = p.parse("span { color: blue; }");
    REQUIRE(p.errors().empty());
    REQUIRE(b->rules.size() == 1);
    REQUIRE(b->rules[0].selectors[0].compounds[0].parts[0].name == "span");

    // Первый sheet не должен измениться
    REQUIRE(a->rules.size() == 1);
    REQUIRE(a->rules[0].selectors[0].compounds[0].parts[0].name == "div");
}


TEST_CASE("CSSParser: margin shorthand expands or is preserved",
    "[css_parser][shorthand]") {
    ArenaAllocator arena;
    auto r = parse(arena, ".card { margin: 24px; padding: 8px 16px; }");
    REQUIRE(r.ok());
    // если парсер не разворачивает — оставляем как один Declaration с property="margin"
    // если разворачивает — четыре Declaration с "margin-top/right/bottom/left"
}