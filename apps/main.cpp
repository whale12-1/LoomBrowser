#include <iostream>
#include <cassert>
#include <cmath>              // ← добавили
#include <string_view>
#include <unordered_map>
#include <vector>

#include "parsers/html_parser/headers/dom.h"
#include "parsers/css_parser/headers/css_dom.h"
#include "parsers/selector_matcher/style_tree_builder.h"

#include "layout/headers/layout_node.h"
#include "layout/headers/layout_tree_builder.h"

// ============================================================
//  5. Layout: базовая блочная раскладка
// ============================================================
void test_layout_basic_block() {
    std::cout << "[RUN] Testing LayoutTreeBuilder basic block layout...\n";

    // <html><body><div/></body></html>
    DOMNode* html = new DOMNode();
    html->type = NodeType::Element;
    html->tag_name = "html";

    DOMNode* body = new DOMNode();
    body->type = NodeType::Element;
    body->tag_name = "body";
    body->parent = html;
    html->children.push_back(body);

    DOMNode* div = new DOMNode();
    div->type = NodeType::Element;
    div->tag_name = "div";
    div->parent = body;
    body->children.push_back(div);

    StyleSheet sheet;
    {
        CSSRule r; ComplexSelector s; CompoundSelector c;
        c.parts.push_back({ SimpleSelector::Kind::Tag, "html" });
        s.compounds = { c }; r.selectors = { s };
        r.declarations.push_back({ "display", "block", false });
        sheet.rules.push_back(std::move(r));
    }
    {
        CSSRule r; ComplexSelector s; CompoundSelector c;
        c.parts.push_back({ SimpleSelector::Kind::Tag, "body" });
        s.compounds = { c }; r.selectors = { s };
        r.declarations.push_back({ "display", "block", false });
        sheet.rules.push_back(std::move(r));
    }
    {
        CSSRule r; ComplexSelector s; CompoundSelector c;
        c.parts.push_back({ SimpleSelector::Kind::Tag, "div" });
        s.compounds = { c }; r.selectors = { s };
        r.declarations.push_back({ "display", "block", false });
        r.declarations.push_back({ "width",   "200px", false });
        r.declarations.push_back({ "height",  "100px", false });
        sheet.rules.push_back(std::move(r));
    }

    StyleStorageSoA storage = StyleTreeBuilder::build(html, sheet);
    auto layout = LayoutTreeBuilder::build(storage);
    LayoutTreeBuilder::compute_layout(layout.get(), 1024.0f, storage);

    assert(layout);
    assert(layout->type == BoxType::Block);
    assert(layout->children.size() == 1);

    LayoutNode* Lbody = layout->children[0].get();
    assert(Lbody->type == BoxType::Block);
    assert(Lbody->children.size() == 1);

    LayoutNode* Ldiv = Lbody->children[0].get();
    assert(Ldiv->type == BoxType::Block);

    assert(std::abs(layout->geometry.width - 1024.0f) < 0.01f);
    assert(std::abs(layout->geometry.height - 100.0f) < 0.01f);
    assert(std::abs(Lbody->geometry.width - 1024.0f) < 0.01f);
    assert(std::abs(Lbody->geometry.height - 100.0f) < 0.01f);

    assert(std::abs(Ldiv->geometry.width - 200.0f) < 0.01f);
    assert(std::abs(Ldiv->geometry.height - 100.0f) < 0.01f);
    assert(std::abs(Ldiv->geometry.x) < 0.01f);
    assert(std::abs(Ldiv->geometry.y) < 0.01f);

    delete html; delete body; delete div;
    std::cout << "  [OK] Layout basic block passed!\n";
}

// ============================================================
//  6. Layout: вертикальный стек с margin'ами
// ============================================================
void test_layout_vertical_stack() {
    std::cout << "[RUN] Testing LayoutTreeBuilder vertical stack...\n";

    DOMNode* body = new DOMNode();
    body->type = NodeType::Element;
    body->tag_name = "body";

    DOMNode* d1 = new DOMNode();
    d1->type = NodeType::Element;
    d1->tag_name = "div";
    d1->attributes["id"] = "d1";
    d1->parent = body;
    body->children.push_back(d1);

    DOMNode* d2 = new DOMNode();
    d2->type = NodeType::Element;
    d2->tag_name = "div";
    d2->attributes["id"] = "d2";
    d2->parent = body;
    body->children.push_back(d2);

    StyleSheet sheet;
    {
        CSSRule r; ComplexSelector s; CompoundSelector c;
        c.parts.push_back({ SimpleSelector::Kind::Tag, "body" });
        s.compounds = { c }; r.selectors = { s };
        r.declarations.push_back({ "display", "block", false });
        sheet.rules.push_back(std::move(r));
    }
    {
        CSSRule r; ComplexSelector s; CompoundSelector c;
        c.parts.push_back({ SimpleSelector::Kind::Tag, "div" });
        s.compounds = { c }; r.selectors = { s };
        r.declarations.push_back({ "display", "block", false });
        sheet.rules.push_back(std::move(r));
    }
    {
        CSSRule r; ComplexSelector s; CompoundSelector c;
        c.parts.push_back({ SimpleSelector::Kind::Id, "d1" });
        s.compounds = { c }; r.selectors = { s };
        r.declarations.push_back({ "height",        "50px", false });
        r.declarations.push_back({ "margin-bottom", "10px", false });
        sheet.rules.push_back(std::move(r));
    }
    {
        CSSRule r; ComplexSelector s; CompoundSelector c;
        c.parts.push_back({ SimpleSelector::Kind::Id, "d2" });
        s.compounds = { c }; r.selectors = { s };
        r.declarations.push_back({ "height",     "30px", false });
        r.declarations.push_back({ "margin-top", "20px", false });
        sheet.rules.push_back(std::move(r));
    }

    StyleStorageSoA storage = StyleTreeBuilder::build(body, sheet);
    auto layout = LayoutTreeBuilder::build(storage);
    LayoutTreeBuilder::compute_layout(layout.get(), 800.0f, storage);

    assert(layout);
    assert(layout->children.size() == 2);

    LayoutNode* Ld1 = layout->children[0].get();
    LayoutNode* Ld2 = layout->children[1].get();

    assert(std::abs(Ld1->geometry.y) < 0.01f);
    assert(std::abs(Ld1->geometry.height - 50.0f) < 0.01f);
    assert(std::abs(Ld1->geometry.margin_bottom - 10.0f) < 0.01f);

    assert(std::abs(Ld2->geometry.y - 80.0f) < 0.01f);
    assert(std::abs(Ld2->geometry.height - 30.0f) < 0.01f);

    // body.height = 50 + 10 + 20 + 30 = 110 (margin collapsing ещё нет)
    assert(std::abs(layout->geometry.height - 110.0f) < 0.01f);

    delete body; delete d1; delete d2;
    std::cout << "  [OK] Layout vertical stack passed!\n";
}

// ============================================================
//  7. Layout: анонимные блоки (mixed inline / block)
// ============================================================
void test_layout_anonymous_blocks() {
    std::cout << "[RUN] Testing LayoutTreeBuilder anonymous blocks...\n";

    DOMNode* html = new DOMNode();
    html->type = NodeType::Element;
    html->tag_name = "html";

    DOMNode* body = new DOMNode();
    body->type = NodeType::Element;
    body->tag_name = "body";
    body->parent = html;
    html->children.push_back(body);

    DOMNode* div = new DOMNode();
    div->type = NodeType::Element;
    div->tag_name = "div";
    div->parent = body;
    body->children.push_back(div);

    // div.children = [Text("hello"), span, inner_div, Text("tail")]
    DOMNode* t1 = new DOMNode();
    t1->type = NodeType::Text;
    t1->text_content = "hello";
    t1->parent = div;
    div->children.push_back(t1);

    DOMNode* span = new DOMNode();
    span->type = NodeType::Element;
    span->tag_name = "span";
    span->parent = div;
    div->children.push_back(span);

    DOMNode* t2 = new DOMNode();
    t2->type = NodeType::Text;
    t2->text_content = "world";
    t2->parent = span;
    span->children.push_back(t2);

    DOMNode* inner = new DOMNode();
    inner->type = NodeType::Element;
    inner->tag_name = "div";
    inner->parent = div;
    div->children.push_back(inner);

    DOMNode* t3 = new DOMNode();
    t3->type = NodeType::Text;
    t3->text_content = "tail";
    t3->parent = div;
    div->children.push_back(t3);

    StyleSheet sheet;
    {
        CSSRule r; ComplexSelector s; CompoundSelector c;
        c.parts.push_back({ SimpleSelector::Kind::Tag, "html" });
        s.compounds = { c }; r.selectors = { s };
        r.declarations.push_back({ "display", "block", false });
        sheet.rules.push_back(std::move(r));
    }
    {
        CSSRule r; ComplexSelector s; CompoundSelector c;
        c.parts.push_back({ SimpleSelector::Kind::Tag, "body" });
        s.compounds = { c }; r.selectors = { s };
        r.declarations.push_back({ "display", "block", false });
        sheet.rules.push_back(std::move(r));
    }
    {
        CSSRule r; ComplexSelector s; CompoundSelector c;
        c.parts.push_back({ SimpleSelector::Kind::Tag, "div" });
        s.compounds = { c }; r.selectors = { s };
        r.declarations.push_back({ "display", "block", false });
        sheet.rules.push_back(std::move(r));
    }
    // span не задаём — остаётся inline по умолчанию

    StyleStorageSoA storage = StyleTreeBuilder::build(html, sheet);
    auto layout = LayoutTreeBuilder::build(storage);

    assert(layout);
    assert(layout->children.size() == 1);
    LayoutNode* Lbody = layout->children[0].get();
    assert(Lbody->children.size() == 1);
    LayoutNode* Ldiv = Lbody->children[0].get();
    assert(Ldiv->type == BoxType::Block);

    // Ожидаем: [ Anon[Text, Inline[Text]], Block, Anon[Text] ]
    assert(Ldiv->children.size() == 3);

    LayoutNode* c0 = Ldiv->children[0].get();
    assert(c0->type == BoxType::AnonymousBlock);
    assert(c0->children.size() == 2);
    assert(c0->children[0]->type == BoxType::Text);
    assert(c0->children[0]->text_content == "hello");
    assert(c0->children[1]->type == BoxType::Inline);
    assert(c0->children[1]->children.size() == 1);
    assert(c0->children[1]->children[0]->type == BoxType::Text);
    assert(c0->children[1]->children[0]->text_content == "world");

    LayoutNode* c1 = Ldiv->children[1].get();
    assert(c1->type == BoxType::Block);

    LayoutNode* c2 = Ldiv->children[2].get();
    assert(c2->type == BoxType::AnonymousBlock);
    assert(c2->children.size() == 1);
    assert(c2->children[0]->type == BoxType::Text);
    assert(c2->children[0]->text_content == "tail");

    assert(c0->style_soa_idx == UINT32_MAX);
    assert(c2->style_soa_idx == UINT32_MAX);

    delete t1; delete t2; delete t3;
    delete span; delete inner;
    delete div; delete body; delete html;
    std::cout << "  [OK] Layout anonymous blocks passed!\n";
}

// ============================================================
//  8. Layout: display:none выкидывает поддерево целиком
// ============================================================
void test_layout_display_none() {
    std::cout << "[RUN] Testing LayoutTreeBuilder display:none skip...\n";

    DOMNode* body = new DOMNode();
    body->type = NodeType::Element;
    body->tag_name = "body";

    DOMNode* hidden = new DOMNode();
    hidden->type = NodeType::Element;
    hidden->tag_name = "div";
    hidden->attributes["id"] = "hidden";
    hidden->parent = body;
    body->children.push_back(hidden);

    DOMNode* deep = new DOMNode();
    deep->type = NodeType::Element;
    deep->tag_name = "span";
    deep->parent = hidden;
    hidden->children.push_back(deep);

    DOMNode* t = new DOMNode();
    t->type = NodeType::Text;
    t->text_content = "invisible";
    t->parent = deep;
    deep->children.push_back(t);

    DOMNode* p = new DOMNode();
    p->type = NodeType::Element;
    p->tag_name = "p";
    p->parent = body;
    body->children.push_back(p);

    StyleSheet sheet;
    {
        CSSRule r; ComplexSelector s; CompoundSelector c;
        c.parts.push_back({ SimpleSelector::Kind::Tag, "body" });
        s.compounds = { c }; r.selectors = { s };
        r.declarations.push_back({ "display", "block", false });
        sheet.rules.push_back(std::move(r));
    }
    {
        CSSRule r; ComplexSelector s; CompoundSelector c;
        c.parts.push_back({ SimpleSelector::Kind::Tag, "p" });
        s.compounds = { c }; r.selectors = { s };
        r.declarations.push_back({ "display", "block", false });
        sheet.rules.push_back(std::move(r));
    }
    {
        CSSRule r; ComplexSelector s; CompoundSelector c;
        c.parts.push_back({ SimpleSelector::Kind::Tag, "div" });
        s.compounds = { c }; r.selectors = { s };
        r.declarations.push_back({ "display", "none", false });
        sheet.rules.push_back(std::move(r));
    }

    StyleStorageSoA storage = StyleTreeBuilder::build(body, sheet);
    auto layout = LayoutTreeBuilder::build(storage);

    assert(layout);
    // Остался только <p>, ветка <div id="hidden"> выкинута
    assert(layout->children.size() == 1);
    assert(layout->children[0]->type == BoxType::Block);
    assert(layout->children[0]->children.empty());

    // Полный обход: должно быть ровно 2 узла (body + p).
    // Ловит случай, когда display:none фильтрует только сам узел,
    // но всё равно создаёт его потомков.
    std::vector<const LayoutNode*> todo;
    todo.push_back(layout.get());
    size_t total = 0;
    while (!todo.empty()) {
        const LayoutNode* n = todo.back(); todo.pop_back();
        ++total;
        for (auto& ch : n->children) todo.push_back(ch.get());
    }
    assert(total == 2);

    delete t; delete deep; delete hidden; delete p; delete body;
    std::cout << "  [OK] Layout display:none passed!\n";
}
int main() {
    std::cout << "========================================\n";
    std::cout << " Running Full Engine Subsystem Tests   \n";
    std::cout << "========================================\n";


    // --- Layout tree ---
    test_layout_basic_block();
    test_layout_vertical_stack();
    test_layout_anonymous_blocks();
    test_layout_display_none();

    std::cout << "========================================\n";
    std::cout << " ALL SUITE TESTS PASSED SUCCESSFULLY! \n";
    std::cout << "========================================\n";
    return 0;
}