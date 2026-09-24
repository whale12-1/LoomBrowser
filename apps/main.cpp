#include <iostream>
#include <cassert>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "headers/dom.h"
#include "headers/css_dom.h"
#include "layout/style_tree_builder.h"

static DOMNode* create_element(const std::string& tag, DOMNode* parent = nullptr) {
    DOMNode* node = new DOMNode();
    node->type = NodeType::Element;
    node->tag_name = tag;
    node->parent = parent;
    if (parent) {
        parent->children.push_back(node);
    }
    return node;
}

static void set_id(DOMNode* node, const std::string& id) {
    node->attributes["id"] = id;
}

static void set_class(DOMNode* node, const std::string& cls) {
    node->attributes["class"] = cls;
}

static void set_attr(DOMNode* node, const std::string& key, const std::string& val) {
    node->attributes[key] = val;
}

static void destroy_tree(DOMNode* node) {
    if (!node) return;
    for (DOMNode* child : node->children) {
        destroy_tree(child);
    }
    delete node;
}

// ============================================================
//  1. Тесты Combinators & Pseudo-classes (SelectorMatcher)
// ============================================================
void test_selector_matching() {
    std::cout << "[RUN] Testing SelectorMatcher & Combinators...\n";

    // Иерархия:
    // <html id="root">
    //   <body class="main-body">
    //     <div id="wrapper">
    //       <p class="text active" id="p1"></p>
    //       <p class="text" id="p2"></p>
    //       <span class="text" id="s1"></span>
    //       <a href="https://example.com" id="a1">Link</a>
    //     </div>
    //   </body>
    // </html>
    DOMNode* html = create_element("html");
    set_id(html, "root");

    DOMNode* body = create_element("body", html);
    set_class(body, "main-body");

    DOMNode* wrapper = create_element("div", body);
    set_id(wrapper, "wrapper");

    DOMNode* p1 = create_element("p", wrapper);
    set_class(p1, "text active");
    set_id(p1, "p1");

    DOMNode* p2 = create_element("p", wrapper);
    set_class(p2, "text");
    set_id(p2, "p2");

    DOMNode* s1 = create_element("span", wrapper);
    set_class(s1, "text");
    set_id(s1, "s1");

    DOMNode* a1 = create_element("a", wrapper);
    set_attr(a1, "href", "https://example.com");
    set_id(a1, "a1");

    // 1. Descendant ( ) & Child (>)
    {
        ComplexSelector cs;
        // html #wrapper p.active
        CompoundSelector c1, c2, c3;
        c1.parts.push_back({ SimpleSelector::Kind::Tag, "html" });

        c2.combinator = Combinator::Descendant;
        c2.parts.push_back({ SimpleSelector::Kind::Id, "wrapper" });

        c3.combinator = Combinator::Child;
        c3.parts.push_back({ SimpleSelector::Kind::Tag, "p" });
        c3.parts.push_back({ SimpleSelector::Kind::Class, "active" });

        cs.compounds = { c1, c2, c3 };

        assert(SelectorMatcher::match_complex(p1, cs) == true);
        assert(SelectorMatcher::match_complex(p2, cs) == false);
    }

    // 2. Adjacent Sibling (+) & General Sibling (~)
    {
        // p + p
        ComplexSelector cs_adj;
        CompoundSelector c1, c2;
        c1.parts.push_back({ SimpleSelector::Kind::Tag, "p" });
        c2.combinator = Combinator::AdjacentSibling;
        c2.parts.push_back({ SimpleSelector::Kind::Tag, "p" });
        cs_adj.compounds = { c1, c2 };

        assert(SelectorMatcher::match_complex(p2, cs_adj) == true);
        assert(SelectorMatcher::match_complex(p1, cs_adj) == false);

        // p ~ span
        ComplexSelector cs_gen;
        CompoundSelector g1, g2;
        g1.parts.push_back({ SimpleSelector::Kind::Tag, "p" });
        g2.combinator = Combinator::GeneralSibling;
        g2.parts.push_back({ SimpleSelector::Kind::Tag, "span" });
        cs_gen.compounds = { g1, g2 };

        assert(SelectorMatcher::match_complex(s1, cs_gen) == true);
    }

    // 3. Pseudo-classes (:nth-child, :first-of-type, :not, :any-link)
    {
        // :nth-child(2n+1) -> odd -> 1st child (p1) и 3rd child (s1)
        ComplexSelector cs_nth;
        CompoundSelector c;
        SimpleSelector ps;
        ps.kind = SimpleSelector::Kind::PseudoClass;
        ps.name = "nth-child";
        ps.arg = "2n+1";
        c.parts.push_back(ps);
        cs_nth.compounds = { c };

        assert(SelectorMatcher::match_complex(p1, cs_nth) == true);  // 1st child
        assert(SelectorMatcher::match_complex(p2, cs_nth) == false); // 2nd child
        assert(SelectorMatcher::match_complex(s1, cs_nth) == true);  // 3rd child

        // :first-of-type для span (s1 — первый span, хоть и 3-й ребёнок)
        ComplexSelector cs_fot;
        CompoundSelector c_fot;
        SimpleSelector ps_fot;
        ps_fot.kind = SimpleSelector::Kind::PseudoClass;
        ps_fot.name = "first-of-type";
        c_fot.parts.push_back(ps_fot);
        cs_fot.compounds = { c_fot };

        assert(SelectorMatcher::match_complex(s1, cs_fot) == true);

        // :not(.active)
        ComplexSelector cs_not;
        CompoundSelector c_not;
        SimpleSelector ps_not;
        ps_not.kind = SimpleSelector::Kind::PseudoClass;
        ps_not.name = "not";
        ps_not.arg = ".active";
        c_not.parts.push_back(ps_not);
        cs_not.compounds = { c_not };

        assert(SelectorMatcher::match_complex(p2, cs_not) == true);
        assert(SelectorMatcher::match_complex(p1, cs_not) == false);

        // :any-link
        ComplexSelector cs_link;
        CompoundSelector c_link;
        SimpleSelector ps_link;
        ps_link.kind = SimpleSelector::Kind::PseudoClass;
        ps_link.name = "any-link";
        c_link.parts.push_back(ps_link);
        cs_link.compounds = { c_link };

        assert(SelectorMatcher::match_complex(a1, cs_link) == true);
        assert(SelectorMatcher::match_complex(p1, cs_link) == false);
    }

    destroy_tree(html);
    std::cout << "  [OK] SelectorMatcher & Combinators tests passed!\n";
}

// ============================================================
//  2. Тесты Specificity, Cascade & !important
// ============================================================
void test_cascade_and_specificity() {
    std::cout << "[RUN] Testing Cascade, Specificity & !important...\n";

    DOMNode* root = create_element("div");
    set_id(root, "main");
    set_class(root, "box primary");

    StyleSheet sheet;

    // Rule 1: .box { color: red; } -> (0, 1, 0)
    CSSRule r1;
    ComplexSelector s1;
    CompoundSelector c1;
    c1.parts.push_back({ SimpleSelector::Kind::Class, "box" });
    s1.compounds = { c1 };
    r1.selectors = { s1 };
    r1.declarations.push_back({ "color", "red", false });
    sheet.rules.push_back(r1);

    // Rule 2: #main { color: blue; } -> (1, 0, 0)
    CSSRule r2;
    ComplexSelector s2;
    CompoundSelector c2;
    c2.parts.push_back({ SimpleSelector::Kind::Id, "main" });
    s2.compounds = { c2 };
    r2.selectors = { s2 };
    r2.declarations.push_back({ "color", "blue", false });
    sheet.rules.push_back(r2);

    // Rule 3: .primary { color: yellow !important; } -> перебивает #main из-за !important
    CSSRule r3;
    ComplexSelector s3;
    CompoundSelector c3;
    c3.parts.push_back({ SimpleSelector::Kind::Class, "primary" });
    s3.compounds = { c3 };
    r3.selectors = { s3 };
    r3.declarations.push_back({ "color", "yellow", true });
    sheet.rules.push_back(r3);

    StyleStorageSoA storage = StyleTreeBuilder::build(root, sheet);

    // Ожидаем yellow (0xFFFF00FF)
    uint32_t expected_yellow = style::Color{ 255, 255, 0, 255 }.pack();
    assert(storage.text_colors[0] == expected_yellow);

    destroy_tree(root);
    std::cout << "  [OK] Cascade & Specificity tests passed!\n";
}

// ============================================================
//  3. Тесты Inheritance & Unit Resolution (rem / em)
// ============================================================
void test_inheritance_and_units() {
    std::cout << "[RUN] Testing Inheritance & Unit Resolution (rem/em)...\n";

    DOMNode* html = create_element("html");
    DOMNode* body = create_element("body", html);
    DOMNode* div = create_element("div", body);
    DOMNode* p = create_element("p", div);

    StyleSheet sheet;

    // html { font-size: 20px; color: green; }
    CSSRule r_html;
    ComplexSelector s_html; CompoundSelector c_html;
    c_html.parts.push_back({ SimpleSelector::Kind::Tag, "html" });
    s_html.compounds = { c_html };
    r_html.selectors = { s_html };
    r_html.declarations.push_back({ "font-size", "20px", false });
    r_html.declarations.push_back({ "color", "green", false });
    sheet.rules.push_back(r_html);

    // body { font-size: 1.5rem; } -> 1.5 * 20px (root font-size) = 30px
    CSSRule r_body;
    ComplexSelector s_body; CompoundSelector c_body;
    c_body.parts.push_back({ SimpleSelector::Kind::Tag, "body" });
    s_body.compounds = { c_body };
    r_body.selectors = { s_body };
    r_body.declarations.push_back({ "font-size", "1.5rem", false });
    sheet.rules.push_back(r_body);

    // div { font-size: 2em; } -> 2 * 30px (parent body font-size) = 60px
    CSSRule r_div;
    ComplexSelector s_div; CompoundSelector c_div;
    c_div.parts.push_back({ SimpleSelector::Kind::Tag, "div" });
    s_div.compounds = { c_div };
    r_div.selectors = { s_div };
    r_div.declarations.push_back({ "font-size", "2em", false });
    sheet.rules.push_back(r_div);

    StyleStorageSoA storage = StyleTreeBuilder::build(html, sheet);

    uint32_t html_idx = 0;
    uint32_t body_idx = storage.first_child_indices[html_idx];
    uint32_t div_idx = storage.first_child_indices[body_idx];
    uint32_t p_idx = storage.first_child_indices[div_idx];

    // Проверка font-size
    assert(storage.font_sizes[html_idx] == 20.0f);
    assert(storage.font_sizes[body_idx] == 30.0f);
    assert(storage.font_sizes[div_idx] == 60.0f);
    assert(storage.font_sizes[p_idx] == 60.0f); // Унаследовано от div

    // Проверка наследования цвета (green)
    uint32_t expected_green = style::Color{ 0, 128, 0, 255 }.pack();
    assert(storage.text_colors[p_idx] == expected_green);

    destroy_tree(html);
    std::cout << "  [OK] Inheritance & Units tests passed!\n";
}

// ============================================================
//  4. Тесты StyleRuleIndex (Buckets & Collect по индексам)
// ============================================================
void test_rule_index_buckets() {
    std::cout << "[RUN] Testing StyleRuleIndex Buckets & Re-build...\n";

    DOMNode* node = create_element("button");
    set_id(node, "btn-id");
    set_class(node, "btn primary");

    // Sheet 1
    StyleSheet sheet1;
    CSSRule r1;
    ComplexSelector s1; CompoundSelector c1;
    c1.parts.push_back({ SimpleSelector::Kind::Class, "primary" });
    s1.compounds = { c1 };
    r1.selectors = { s1 };
    r1.declarations.push_back({ "display", "flex", false });
    sheet1.rules.push_back(r1);

    // Первичный билд индекса
    StyleRuleIndex index1(sheet1);
    index1.build();

    std::vector<uint32_t> matched_entry_indices;
    index1.collect(node, matched_entry_indices);

    // Должна найтись ровно 1 запись по классу .primary
    assert(matched_entry_indices.size() == 1);

    // Проверяем обратную связь с правилом через индекс
    uint32_t entry_idx = matched_entry_indices[0];
    const auto& entry = index1.entry(entry_idx);
    const CSSRule& matched_rule = index1.rule(entry.rule_index);
    assert(matched_rule.declarations[0].value == "flex");

    // Sheet 2: Проверка независимости нового экземпляра/пересборки
    StyleSheet sheet2;
    CSSRule r2;
    ComplexSelector s2; CompoundSelector c2;
    c2.parts.push_back({ SimpleSelector::Kind::Id, "btn-id" });
    s2.compounds = { c2 };
    r2.selectors = { s2 };
    r2.declarations.push_back({ "display", "block", false });
    sheet2.rules.push_back(r2);

    StyleRuleIndex index2(sheet2);
    index2.build();

    matched_entry_indices.clear();
    index2.collect(node, matched_entry_indices);

    // Должна найтись ровно 1 запись из sheet2 по id
    assert(matched_entry_indices.size() == 1);
    const auto& entry2 = index2.entry(matched_entry_indices[0]);
    assert(index2.rule(entry2.rule_index).declarations[0].value == "block");

    destroy_tree(node);
    std::cout << "  [OK] StyleRuleIndex tests passed!\n";
}

// ============================================================
//  Main Entry Point
// ============================================================
int main() {
    std::cout << "========================================\n";
    std::cout << " Running Full Engine Subsystem Tests   \n";
    std::cout << "========================================\n";

    test_selector_matching();
    test_cascade_and_specificity();
    test_inheritance_and_units();
    test_rule_index_buckets();

    std::cout << "========================================\n";
    std::cout << " ALL SUITE TESTS PASSED SUCCESSFULLY! \n";
    std::cout << "========================================\n";
    return 0;
}