#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <string>
#include <vector>

#include "../parsers/selector_matcher/style_tree_builder.h"
#include "../parsers/selector_matcher/style_storage_soa.h"
#include "../parsers/selector_matcher/selector_matcher.h"
#include "../parsers/html_parser/headers/dom.h"
#include "../parsers/css_parser/headers/css_dom.h"
#include "../parsers/html_parser/headers/html_parser.h"
#include "../parsers/css_parser/headers/css_parser.h"

namespace {

    using K = SimpleSelector::Kind;
    using SS = SimpleSelector;
    using Cmp = CompoundSelector;
    using CS = ComplexSelector;
    using SM = SelectorMatcher;

    // ---------- Simple selector builders ----------
    inline SS tg(const std::string& n) { SS s; s.kind = K::Tag;    s.name = n; return s; }
    inline SS cl(const std::string& n) { SS s; s.kind = K::Class;  s.name = n; return s; }
    inline SS id_(const std::string& n) { SS s; s.kind = K::Id;     s.name = n; return s; }
    inline SS uni() { SS s; s.kind = K::Universal;           return s; }

    // ---------- Compound / Complex builders ----------
    inline Cmp cmp(SS a) { Cmp c; c.parts.push_back(std::move(a)); return c; }
    inline Cmp cmp(SS a, SS b) {
        Cmp c; c.parts.push_back(std::move(a));
        c.parts.push_back(std::move(b)); return c;
    }
    inline Cmp cmp(SS a, SS b, SS c3) {
        Cmp c; c.parts.push_back(std::move(a));
        c.parts.push_back(std::move(b));
        c.parts.push_back(std::move(c3)); return c;
    }
    inline CS  one(SS s) { CS cs; cs.compounds.push_back(cmp(std::move(s))); return cs; }
    inline CS  one(Cmp c) { CS cs; cs.compounds.push_back(std::move(c)); return cs; }

    // ---------- Declaration / Rule / Sheet builders ----------
    inline Declaration decl_(const std::string& p, const std::string& v, bool imp = false) {
        Declaration d; d.property = p; d.value = v; d.important = imp; return d;
    }
    inline CSSRule rule_(std::vector<CS> sels, std::vector<Declaration> decls) {
        CSSRule r; r.selectors = std::move(sels); r.declarations = std::move(decls); return r;
    }
    inline CSSRule rtag(const std::string& tag, std::vector<Declaration> decls) {
        return rule_({ one(tg(tag)) }, std::move(decls));
    }

    // ---------- DOM builder (RAII) ----------
    struct Tree {
        std::vector<DOMNode*> owned;

        DOMNode* E(const std::string& tag, DOMNode* parent = nullptr) {
            auto* n = new DOMNode();
            n->type = NodeType::Element;
            n->tag_name = tag;
            if (parent) { n->parent = parent; parent->children.push_back(n); }
            owned.push_back(n);
            return n;
        }
        DOMNode* T(const std::string& text, DOMNode* parent = nullptr) {
            auto* n = new DOMNode();
            n->type = NodeType::Text;
            n->text_content = text;
            if (parent) { n->parent = parent; parent->children.push_back(n); }
            owned.push_back(n);
            return n;
        }
        ~Tree() { for (auto* n : owned) delete n; }
    };

    inline void set_class(DOMNode* n, const std::string& c) { n->attributes["class"] = c; }
    inline void set_id(DOMNode* n, const std::string& i) { n->attributes["id"] = i; }

    // ---------- Common tree shape ----------
    // <html> (0) → <body> (1) → <div> (2)
    struct HtmlBodyDiv {
        Tree t;
        DOMNode* html; DOMNode* body; DOMNode* div;
        HtmlBodyDiv() {
            html = t.E("html");
            body = t.E("body", html);
            div = t.E("div", body);
        }
    };

} // namespace



TEST_CASE("STB: display block applied", "[stb][value]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("display", "block") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.displays[2] == style::Display::Block);
}

TEST_CASE("STB: display none applied", "[stb][value]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("display", "none") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.displays[2] == style::Display::None);
}

TEST_CASE("STB: display flex", "[stb][value]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("display", "flex") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.displays[2] == style::Display::Flex);
}

TEST_CASE("STB: display invalid falls back to Inline",
    "[stb][value][malformed]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("display", "garbage") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.displays[2] == style::Display::Inline);
}

TEST_CASE("STB: position absolute", "[stb][value]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("position", "absolute") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.positions[2] == Position::Absolute);
}

TEST_CASE("STB: visibility hidden", "[stb][value]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("visibility", "hidden") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.visibilities[2] == Visibility::Hidden);
}

TEST_CASE("STB: box-sizing border-box", "[stb][value]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("box-sizing", "border-box") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.box_sizings[2] == BoxSizing::BorderBox);
}

TEST_CASE("STB: text-align center", "[stb][value]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("text-align", "center") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.text_aligns[2] == TextAlign::Center);
}

TEST_CASE("STB: z-index numeric", "[stb][value]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("z-index", "100") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.z_indices[2] == 100);
}

TEST_CASE("STB: z-index negative", "[stb][value]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("z-index", "-5") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.z_indices[2] == -5);
}

TEST_CASE("STB: opacity quantized to 0..255", "[stb][value]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("opacity", "0.5") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.opacity_q[2] == 128);  // 0.5 * 255 rounded
}


TEST_CASE("STB: named color red", "[stb][color]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("color", "red") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.text_colors[2] == 0xFF0000FFu);
}

TEST_CASE("STB: named color transparent", "[stb][color]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("background-color", "transparent") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.background_colors[2] == 0x00000000u);
}

TEST_CASE("STB: hex 3-digit #f00", "[stb][color]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("color", "#f00") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.text_colors[2] == 0xFF0000FFu);
}

TEST_CASE("STB: hex 6-digit #3498db", "[stb][color]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("color", "#3498db") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.text_colors[2] == 0x3498DBFFu);
}

TEST_CASE("STB: hex 8-digit with alpha", "[stb][color]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("color", "#3498db80") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.text_colors[2] == 0x3498DB80u);
}

TEST_CASE("STB: rgb() value", "[stb][color]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("color", "rgb(255, 128, 0)") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.text_colors[2] == 0xFF8000FFu);
}


TEST_CASE("STB: font-size px", "[stb][font]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("font-size", "24px") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.font_sizes[2] == 24.0f);
}

TEST_CASE("STB: font-size keyword medium = 16", "[stb][font]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("font-size", "medium") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.font_sizes[2] == 16.0f);
}

TEST_CASE("STB: font-size keyword large = 18", "[stb][font]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("font-size", "large") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.font_sizes[2] == 18.0f);
}

TEST_CASE("STB: font-size em relative to parent", "[stb][font][em]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("html", { decl_("font-size", "20px") }));
    s.rules.push_back(rtag("body", { decl_("font-size", "1.5em") }));  // 1.5 * 20 = 30
    s.rules.push_back(rtag("div", { decl_("font-size", "2em") }));  // 2 * 30 = 60
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.font_sizes[0] == 20.0f);
    REQUIRE(soa.font_sizes[1] == 30.0f);
    REQUIRE(soa.font_sizes[2] == 60.0f);
}

TEST_CASE("STB: font-size rem relative to root", "[stb][font][rem]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("html", { decl_("font-size", "20px") }));
    s.rules.push_back(rtag("body", { decl_("font-size", "1.5rem") }));  // 1.5 * 20 = 30
    s.rules.push_back(rtag("div", { decl_("font-size", "2rem") }));  // 2 * 20 = 40
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.font_sizes[1] == 30.0f);
    REQUIRE(soa.font_sizes[2] == 40.0f);
}

TEST_CASE("STB: font-size percent of parent", "[stb][font][percent]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("html", { decl_("font-size", "20px") }));
    s.rules.push_back(rtag("body", { decl_("font-size", "150%") }));   // 30
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.font_sizes[1] == 30.0f);
}

TEST_CASE("STB: font-weight bold = 700", "[stb][font]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("font-weight", "bold") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.font_weights[2] == 700);
}

TEST_CASE("STB: font-weight normal = 400", "[stb][font]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("html", { decl_("font-weight", "bold") }));
    s.rules.push_back(rtag("div", { decl_("font-weight", "normal") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.font_weights[2] == 400);
}

TEST_CASE("STB: font-weight numeric 600", "[stb][font]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("font-weight", "600") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.font_weights[2] == 600);
}



TEST_CASE("STB: line-height normal хранится как 0.0 sentinel",
    "[stb][line-height]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("font-size",   "20px") }));
    s.rules.push_back(rtag("div", { decl_("line-height", "normal") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.line_heights[2] == 0.0f);
    REQUIRE(style::resolve_line_height(soa, 2) == 24.0f);   // 20 * 1.2
}

TEST_CASE("STB: line-height unitless хранится как отрицательное",
    "[stb][line-height]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("font-size",   "20px") }));
    s.rules.push_back(rtag("div", { decl_("line-height", "1.5") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.line_heights[2] == -1.5f);
    REQUIRE(style::resolve_line_height(soa, 2) == 30.0f);
}

TEST_CASE("STB: line-height px хранится как px и наследуется как px",
    "[stb][line-height][inherit]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("html", { decl_("line-height", "32px") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    // Все три узла получили 32px (абсолютная величина наследуется как есть)
    REQUIRE(soa.line_heights[0] == 32.0f);
    REQUIRE(soa.line_heights[1] == 32.0f);
    REQUIRE(soa.line_heights[2] == 32.0f);
    REQUIRE(style::resolve_line_height(soa, 2) == 32.0f);
}

TEST_CASE("STB: line-height normal НЕ наследуется как px",
    "[stb][line-height][inherit]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("html", { decl_("font-size",   "16px") }));
    s.rules.push_back(rtag("div", { decl_("font-size",   "32px") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    // Оба имеют raw = 0.0 → resolve даёт разные значения
    REQUIRE(soa.line_heights[0] == 0.0f);
    REQUIRE(soa.line_heights[2] == 0.0f);
    REQUIRE(style::resolve_line_height(soa, 0) == 19.2f);  // 16 * 1.2
    REQUIRE(style::resolve_line_height(soa, 2) == 38.4f);  // 32 * 1.2
}

TEST_CASE("STB: width 200px stored", "[stb][length]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("width", "200px") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.widths[2].value == 200.0f);
    REQUIRE(soa.widths[2].unit == Unit::Px);
}

TEST_CASE("STB: width auto stored as Auto", "[stb][length]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("width", "auto") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.widths[2].is_auto());
}

TEST_CASE("STB: width 50% stored as Percent", "[stb][length]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("width", "50%") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.widths[2].value == 50.0f);
    REQUIRE(soa.widths[2].unit == Unit::Percent);
}

TEST_CASE("STB: height 100px", "[stb][length]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("height", "100px") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.heights[2].value == 100.0f);
}

TEST_CASE("STB: margin-top/bottom set individually", "[stb][length]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", {
        decl_("margin-top",    "5px"),
        decl_("margin-bottom", "12px")
        }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.margin_top[2].value == 5.0f);
    REQUIRE(soa.margin_bottom[2].value == 12.0f);
    REQUIRE(soa.margin_left[2].value == 0.0f);  // default
}

TEST_CASE("STB: negative margin allowed", "[stb][length]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("margin-top", "-8px") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.margin_top[2].value == -8.0f);
}

TEST_CASE("STB: padding em stored as em (deferred)", "[stb][length]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("padding-top", "2em") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.padding_top[2].value == 2.0f);
    REQUIRE(soa.padding_top[2].unit == Unit::Em);
}

TEST_CASE("STB: border-width px", "[stb][length]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("border-top-width", "3px") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.border_top_width[2].value == 3.0f);
}



TEST_CASE("STB cascade: id beats class", "[stb][cascade][spec]") {
    HtmlBodyDiv h;
    set_id(h.div, "main");
    set_class(h.div, "box");
    StyleSheet s;
    // Порядок намеренно плохой — .box раньше #main
    s.rules.push_back(rule_({ one(cl("box")) }, { decl_("color", "red") }));
    s.rules.push_back(rule_({ one(id_("main")) }, { decl_("color", "blue") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    // Победит #main (специфичность 1,0,0 > 0,1,0)
    REQUIRE(soa.text_colors[2] == 0x0000FFFFu);
}

TEST_CASE("STB cascade: class beats tag", "[stb][cascade][spec]") {
    HtmlBodyDiv h;
    set_class(h.div, "box");
    StyleSheet s;
    s.rules.push_back(rtag("div", { decl_("color", "red") }));
    s.rules.push_back(rule_({ one(cl("box")) }, { decl_("color", "blue") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.text_colors[2] == 0x0000FFFFu);
}

TEST_CASE("STB cascade: same specificity later wins",
    "[stb][cascade][spec]") {
    HtmlBodyDiv h;
    set_class(h.div, "box");
    StyleSheet s;
    s.rules.push_back(rule_({ one(cl("box")) }, { decl_("color", "red") }));
    s.rules.push_back(rule_({ one(cl("box")) }, { decl_("color", "blue") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.text_colors[2] == 0x0000FFFFu);
}

TEST_CASE("STB cascade: more specific compound beats simple",
    "[stb][cascade][spec]") {
    HtmlBodyDiv h;
    set_class(h.div, "box");
    StyleSheet s;
    s.rules.push_back(rule_({ one(cl("box")) }, { decl_("color", "red") }));
    s.rules.push_back(rule_({ one(cmp(tg("div"), cl("box"))) },
        { decl_("color", "blue") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.text_colors[2] == 0x0000FFFFu);
}

TEST_CASE("STB cascade: non-matching rule ignored",
    "[stb][cascade]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("span", { decl_("color", "red") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    // <div> не matcher'ится — цвет остаётся дефолтный
    REQUIRE(soa.text_colors[2] == 0x000000FFu);
}


TEST_CASE("STB important: !important beats higher specificity",
    "[stb][important]") {
    HtmlBodyDiv h;
    set_id(h.div, "main");
    set_class(h.div, "box");
    StyleSheet s;
    s.rules.push_back(rule_({ one(id_("main")) }, { decl_("color", "blue") }));
    s.rules.push_back(rule_({ one(cl("box")) },
        { decl_("color", "red", true) }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.text_colors[2] == 0xFF0000FFu);  // red побеждает из-за !important
}

TEST_CASE("STB important: !important vs !important — specificity решает",
    "[stb][important]") {
    HtmlBodyDiv h;
    set_id(h.div, "main");
    set_class(h.div, "box");
    StyleSheet s;
    s.rules.push_back(rule_({ one(cl("box")) },
        { decl_("color", "red", true) }));
    s.rules.push_back(rule_({ one(id_("main")) },
        { decl_("color", "blue", true) }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.text_colors[2] == 0x0000FFFFu);  // id важнее
}

TEST_CASE("STB important: !important vs !important same specificity — "
    "later wins", "[stb][important]") {
    HtmlBodyDiv h;
    set_class(h.div, "box");
    StyleSheet s;
    s.rules.push_back(rule_({ one(cl("box")) },
        { decl_("color", "red", true) }));
    s.rules.push_back(rule_({ one(cl("box")) },
        { decl_("color", "blue", true) }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.text_colors[2] == 0x0000FFFFu);
}


TEST_CASE("STB inherit: color inherits to children", "[stb][inherit]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("html", { decl_("color", "red") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.text_colors[0] == 0xFF0000FFu);
    REQUIRE(soa.text_colors[1] == 0xFF0000FFu);
    REQUIRE(soa.text_colors[2] == 0xFF0000FFu);
}

TEST_CASE("STB inherit: child override wins over inherited",
    "[stb][inherit]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("html", { decl_("color", "red") }));
    s.rules.push_back(rtag("div", { decl_("color", "blue") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.text_colors[0] == 0xFF0000FFu);
    REQUIRE(soa.text_colors[2] == 0x0000FFFFu);
}

TEST_CASE("STB inherit: font-size inherits", "[stb][inherit]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("html", { decl_("font-size", "24px") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.font_sizes[1] == 24.0f);
    REQUIRE(soa.font_sizes[2] == 24.0f);
}

TEST_CASE("STB inherit: font-weight inherits", "[stb][inherit]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("html", { decl_("font-weight", "bold") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.font_weights[2] == 700);
}

TEST_CASE("STB inherit: background-color does NOT inherit",
    "[stb][inherit]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("html", { decl_("background-color", "red") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.background_colors[0] == 0xFF0000FFu);
    // Дети не наследуют background-color — остаётся прозрачным
    REQUIRE(soa.background_colors[1] == 0x00000000u);
    REQUIRE(soa.background_colors[2] == 0x00000000u);
}

TEST_CASE("STB inherit: width does NOT inherit", "[stb][inherit]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("html", { decl_("width", "500px") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.widths[0].value == 500.0f);
    REQUIRE(soa.widths[1].is_auto());  // body не унаследовал
}

TEST_CASE("STB inherit: visibility inherits", "[stb][inherit]") {
    HtmlBodyDiv h;
    StyleSheet s;
    s.rules.push_back(rtag("html", { decl_("visibility", "hidden") }));
    auto soa = StyleTreeBuilder::build(h.html, s);
    REQUIRE(soa.visibilities[2] == Visibility::Hidden);
}



TEST_CASE("STB integration: margin shorthand через полный пайплайн",
    "[stb][shorthand][integration]") {
    ArenaAllocator arena;

    HTMLParser hp(arena);
    DOMNode* dom = hp.parse("<div class='card'></div>");
    DOMNode* root = nullptr;
    for (DOMNode* c : dom->children)
        if (c->type == NodeType::Element) { root = c; break; }
    REQUIRE(root != nullptr);

    CSSParser cp(arena);
    StyleSheet* sheet = cp.parse(".card { margin: 24px; }");
    REQUIRE(sheet != nullptr);
    REQUIRE(sheet->rules.size() == 1);
    // Проверяем, что CSSParser развернул
    REQUIRE(sheet->rules[0].declarations.size() == 4);

    auto soa = StyleTreeBuilder::build(root, *sheet);
    REQUIRE(soa.size() == 1);
    REQUIRE(soa.margin_top[0].value == 24.0f);
    REQUIRE(soa.margin_right[0].value == 24.0f);
    REQUIRE(soa.margin_bottom[0].value == 24.0f);
    REQUIRE(soa.margin_left[0].value == 24.0f);
}



TEST_CASE("STB rule with multiple selectors applies to all matches",
    "[stb][integration]") {
    Tree t;
    auto* root = t.E("root");
    auto* a = t.E("div", root);
    auto* b = t.E("span", root);
    auto* c = t.E("p", root);

    StyleSheet s;
    s.rules.push_back(rule_(
        { one(tg("div")), one(tg("span")) },
        { decl_("color", "red") }));

    auto soa = StyleTreeBuilder::build(root, s);
    REQUIRE(soa.text_colors[1] == 0xFF0000FFu);  // div
    REQUIRE(soa.text_colors[2] == 0xFF0000FFu);  // span
    REQUIRE(soa.text_colors[3] == 0x000000FFu);  // p — не match
}



TEST_CASE("STB integration: card example", "[stb][integration]") {
    Tree t;
    auto* html = t.E("html");
    auto* body = t.E("body", html);
    auto* card = t.E("div", body); set_class(card, "card");
    auto* h1 = t.E("h1", card);

    StyleSheet s;
    s.rules.push_back(rtag("html", { decl_("display", "block") }));
    s.rules.push_back(rtag("body", { decl_("display", "block"),
                                       decl_("font-size", "16px"),
                                       decl_("color", "#222222") }));
    s.rules.push_back(rule_({ one(cl("card")) }, {
        decl_("display", "block"),
        decl_("width",   "640px"),
        decl_("background-color", "#ffffff"),
        decl_("margin-top",  "24px"),
        decl_("padding-top", "24px")
        }));
    s.rules.push_back(rtag("h1", {
        decl_("display",   "block"),
        decl_("font-size", "32px"),
        decl_("color",     "#1a1a1a")
        }));

    auto soa = StyleTreeBuilder::build(html, s);

    // html = 0, body = 1, card = 2, h1 = 3
    REQUIRE(soa.displays[2] == style::Display::Block);
    REQUIRE(soa.widths[2].value == 640.0f);
    REQUIRE(soa.background_colors[2] == 0xFFFFFFFFu);
    REQUIRE(soa.margin_top[2].value == 24.0f);
    REQUIRE(soa.padding_top[2].value == 24.0f);

    // h1: font-size = 32px, color = #1a1a1a
    REQUIRE(soa.font_sizes[3] == 32.0f);
    REQUIRE(soa.text_colors[3] == 0x1A1A1AFFu);

    // h1.font-weight унаследован от body (400 default)
    REQUIRE(soa.font_weights[3] == 400);

    // line-height хранится в сыром виде (0.0 = normal).
// Разрешённое значение = font-size * 1.2.
    REQUIRE(soa.line_heights[3] == 0.0f);
    REQUIRE(style::resolve_line_height(soa, 3) == 38.4f);
}

TEST_CASE("STB integration: cascade with inherit and override",
    "[stb][integration]") {
    Tree t;
    auto* html = t.E("html");
    auto* body = t.E("body", html);
    auto* p1 = t.E("p", body);
    auto* p2 = t.E("p", body); set_class(p2, "note");

    StyleSheet s;
    s.rules.push_back(rtag("html", { decl_("font-size", "18px"),
                                     decl_("color", "blue") }));
    s.rules.push_back(rtag("p", { decl_("font-size", "0.8em") }));  // 0.8 * 18 = 14.4
    s.rules.push_back(rule_({ one(cl("note")) },
        { decl_("color", "red") }));

    auto soa = StyleTreeBuilder::build(html, s);
    // p1 — унаследовал blue и получил font-size 14.4
    REQUIRE(soa.font_sizes[2] == Catch::Approx(14.4f).epsilon(0.001));
    REQUIRE(soa.text_colors[2] == 0x0000FFFFu);
    // p2 — красный (class specificity выше tag + inherit)
    REQUIRE(soa.text_colors[3] == 0xFF0000FFu);
    REQUIRE(soa.font_sizes[3] == Catch::Approx(14.4f).epsilon(0.001));
}