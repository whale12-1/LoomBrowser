#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <memory>
#include <string>
#include <vector>

#include "../layout/headers/layout_node.h"
#include "../layout/headers/layout_tree_builder.h"
#include "../parsers/selector_matcher/style_tree_builder.h"
#include "../parsers/selector_matcher/display_list_builder.h"
#include "../parsers/html_parser/headers/html_parser.h"
#include "../parsers/css_parser/headers/css_parser.h"
#include "../parsers/selector_matcher/user_agent_stylesheet.h"

namespace {

    using Catch::Approx;

    // ============================================================
    //  Полный пайплайн: HTML + CSS → SoA + LayoutNode-дерево
    // ============================================================
    struct Pipeline {
        StyleStorageSoA storage;
        std::unique_ptr<LayoutNode> root;
    };

    // Найти первый Element в детях Document
    inline DOMNode* first_element(DOMNode* n) {
        if (!n) return nullptr;
        if (n->type == NodeType::Element) return n;
        for (DOMNode* c : n->children)
            if (auto* e = first_element(c)) return e;
        return nullptr;
    }

    inline Pipeline run_pipeline(ArenaAllocator& arena,
        const std::string& inner_html,
        const std::string& css,
        float viewport = 1024.0f,
        bool use_full_ua = false) {
        std::string html = "<html><body>" + inner_html + "</body></html>";

        std::string full_css;
        if (use_full_ua) {
            full_css = std::string(ua_css::kSource) + " " + css;
        }
        else {
            full_css = "html, body { display: block; } " + css;
        }

        HTMLParser hp(arena);
        DOMNode* dom = hp.parse(html);
        DOMNode* root_elem = first_element(dom);
        REQUIRE(root_elem != nullptr);

        CSSParser cp(arena);
        StyleSheet* sheet = cp.parse(full_css);
        REQUIRE(sheet != nullptr);

        Pipeline p;
        p.storage = StyleTreeBuilder::build(root_elem, *sheet);
        p.root = LayoutTreeBuilder::build(p.storage);
        if (p.root) {
            LayoutTreeBuilder::Viewport vp{ viewport, 768.0f };
            LayoutTreeBuilder::compute_layout(p.root.get(), vp, p.storage);
        }

        return p;
    }

    // Рекурсивный счётчик узлов дерева
    inline size_t count_nodes(const LayoutNode* n) {
        if (!n) return 0;
        size_t c = 1;
        for (const auto& ch : n->children) c += count_nodes(ch.get());
        return c;
    }

    // Первый ребёнок указанного типа (глубина = 1)
    inline LayoutNode* child_of_type(const LayoutNode* n, BoxType t) {
        if (!n) return nullptr;
        for (const auto& c : n->children) if (c->type == t) return c.get();
        return nullptr;
    }

    inline LayoutNode* find_by_soa_idx(LayoutNode* node, uint32_t soa_idx) {
        if (!node) return nullptr;
        if (node->style_soa_idx == soa_idx) return node;
        for (auto& child : node->children) {
            if (auto* found = find_by_soa_idx(child.get(), soa_idx)) return found;
        }
        return nullptr;
    }

    // Первый НЕпробельный Text-потомок — рекурсивно через AnonymousBlock.
    // Нужно потому, что теперь текст обёрнут в AnonymousBlock и лежит
    // на один уровень глубже, чем раньше.
    inline LayoutNode* first_text_deep(const LayoutNode* node) {
        if (!node) return nullptr;
        for (const auto& c : node->children) {
            if (c->type == BoxType::Text && c->text_content != " ")
                return c.get();
            if (c->type == BoxType::AnonymousBlock) {
                if (auto* t = first_text_deep(c.get())) return t;
            }
        }
        return nullptr;
    }

} // namespace


TEST_CASE("LTB build: empty storage → nullptr", "[ltb][build]") {
    StyleStorageSoA storage;
    auto root = LayoutTreeBuilder::build(storage);
    REQUIRE(root == nullptr);
}

TEST_CASE("LTB build: single block element",
    "[ltb][build][classify]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div></div>",
        "div { display: block; }");
    REQUIRE(p.root != nullptr);
    REQUIRE(p.root->type == BoxType::Block);
    REQUIRE(p.root->style_soa_idx == 0);
}

TEST_CASE("LTB build: display:inline → BoxType::Inline",
    "[ltb][build][classify]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<span></span>",
        "span { display: inline; }");

    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    REQUIRE(body != nullptr);                                   // <body>

    // <span> обёрнут в AnonymousBlock (inline formatting context).
    LayoutNode* anon = child_of_type(body, BoxType::AnonymousBlock);
    REQUIRE(anon != nullptr);

    LayoutNode* sp = child_of_type(anon, BoxType::Inline);
    REQUIRE(sp != nullptr);                                     // <span>
    REQUIRE(sp->type == BoxType::Inline);
}

TEST_CASE("LTB build: display:inline-block → BoxType::Inline",
    "[ltb][build][classify]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<button></button>",
        "button { display: inline-block; }");

    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    REQUIRE(body != nullptr);

    LayoutNode* anon = child_of_type(body, BoxType::AnonymousBlock);
    REQUIRE(anon != nullptr);

    LayoutNode* btn = child_of_type(anon, BoxType::Inline);
    REQUIRE(btn != nullptr);
    REQUIRE(btn->type == BoxType::Inline);
}

TEST_CASE("LTB build: display:flex → BoxType::Block",
    "[ltb][build][classify]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div></div>",
        "div { display: flex; }");
    REQUIRE(p.root->type == BoxType::Block);
}




TEST_CASE("LTB flex: display:flex recognized as flex container",
    "[ltb][layout][flex]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'></div>", "#f { display: flex; }");
    LayoutNode* f = find_by_soa_idx(p.root.get(), 2);

    REQUIRE(f != nullptr);
    REQUIRE(f->style_soa_idx != UINT32_MAX);
    REQUIRE(p.storage.displays[f->style_soa_idx] == style::Display::Flex);
}

TEST_CASE("LTB flex: flex container stays BoxType::Block",
    "[ltb][layout][flex]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'></div>", "#f { display: flex; }");
    LayoutNode* f = find_by_soa_idx(p.root.get(), 2);

    REQUIRE(f != nullptr);
    REQUIRE(f->type == BoxType::Block);
}

TEST_CASE("LTB flex: display:block unchanged",
    "[ltb][layout][flex]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'></div>", "#f { display: block; }");
    LayoutNode* f = find_by_soa_idx(p.root.get(), 2);

    REQUIRE(f != nullptr);
    REQUIRE(f->type == BoxType::Block);
    REQUIRE(p.storage.displays[f->style_soa_idx] == style::Display::Block);
}

TEST_CASE("LTB flex: real flex layout applied",
    "[ltb][layout][flex]") {
    // После группы B flex-контейнер раскладывает детей в строку,
    // а не стопкой. Заглушка A1 удалена.
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div id='f'><p>x</p></div>",
        "#f { display: flex; width: 200px; } "
        "p { display: block; width: 100px; height: 20px; }");
    LayoutNode* f = find_by_soa_idx(p.root.get(), 2);
    LayoutNode* paragraph = find_by_soa_idx(p.root.get(), 3);

    REQUIRE(f != nullptr);
    REQUIRE(paragraph != nullptr);
    REQUIRE(f->geometry.width == Approx(200.0f).epsilon(0.001));
    REQUIRE(paragraph->geometry.x == Approx(0.0f).epsilon(0.001));
    REQUIRE(paragraph->geometry.y == Approx(0.0f).epsilon(0.001));
    REQUIRE(paragraph->geometry.width == Approx(100.0f).epsilon(0.001));
    REQUIRE(paragraph->geometry.height == Approx(20.0f).epsilon(0.001));
}


TEST_CASE("LTB flex: text-only children grouped into single AnonymousBlock",
    "[ltb][layout][flex][normalize]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'>hello world</div>", "#f { display: flex; }");
    LayoutNode* f = find_by_soa_idx(p.root.get(), 2);
    REQUIRE(f != nullptr);
    REQUIRE(f->children.size() == 1);
    REQUIRE(f->children[0]->type == BoxType::AnonymousBlock);
    REQUIRE_FALSE(f->children[0]->children.empty());
}

TEST_CASE("LTB flex: text + block + text → three groups",
    "[ltb][layout][flex][normalize]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'>a<p>b</p>c</div>",
        "#f { display: flex; } p { display: block; }");
    LayoutNode* f = find_by_soa_idx(p.root.get(), 2);
    REQUIRE(f->children.size() == 3);
    REQUIRE(f->children[0]->type == BoxType::AnonymousBlock);
    REQUIRE(f->children[1]->type == BoxType::Block);
    REQUIRE(f->children[2]->type == BoxType::AnonymousBlock);
}

TEST_CASE("LTB flex: text + inline span + text → single group",
    "[ltb][layout][flex][normalize]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'>a<span>b</span>c</div>",
        "#f { display: flex; } span { display: inline; }");
    LayoutNode* f = find_by_soa_idx(p.root.get(), 2);
    REQUIRE(f->children.size() == 1);
    REQUIRE(f->children[0]->type == BoxType::AnonymousBlock);
    REQUIRE(f->children[0]->children.size() == 3);
    REQUIRE(f->children[0]->children[0]->type == BoxType::Text);
    REQUIRE(f->children[0]->children[1]->type == BoxType::Inline);
    REQUIRE(f->children[0]->children[2]->type == BoxType::Text);
}

TEST_CASE("LTB flex: absolute child breaks the group",
    "[ltb][layout][flex][normalize]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'>a<span style='position:absolute'></span>b</div>",
        "#f { display: flex; }");
    LayoutNode* f = find_by_soa_idx(p.root.get(), 2);
    REQUIRE(f->children.size() == 3);
    REQUIRE(f->children[0]->type == BoxType::AnonymousBlock);
    REQUIRE(f->children[1]->type == BoxType::Block);
    REQUIRE(f->children[2]->type == BoxType::AnonymousBlock);
}

TEST_CASE("LTB flex: consecutive blocks stay separate",
    "[ltb][layout][flex][normalize]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'><p>1</p><p>2</p></div>",
        "#f { display: flex; } p { display: block; }");
    LayoutNode* f = find_by_soa_idx(p.root.get(), 2);
    REQUIRE(f->children.size() == 2);
    REQUIRE(f->children[0]->type == BoxType::Block);
    REQUIRE(f->children[1]->type == BoxType::Block);
}

TEST_CASE("LTB flex: regular block still uses BFC wrapping",
    "[ltb][layout][flex][normalize]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='b'>a<p>b</p>c</div>",
        "#b { display: block; } p { display: block; }");
    LayoutNode* b = find_by_soa_idx(p.root.get(), 2);
    REQUIRE(b->children.size() == 3);
    REQUIRE(b->children[0]->type == BoxType::AnonymousBlock);
    REQUIRE(b->children[1]->type == BoxType::Block);
    REQUIRE(b->children[2]->type == BoxType::AnonymousBlock);
}

TEST_CASE("LTB flex: absolute does not join any text group",
    "[ltb][layout][flex][normalize]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'><span style='position:absolute'>x</span>text</div>",
        "#f { display: flex; }");
    LayoutNode* f = find_by_soa_idx(p.root.get(), 2);
    REQUIRE(f->children.size() == 2);
    REQUIRE(f->children[0]->type == BoxType::Block);
    REQUIRE(f->children[1]->type == BoxType::AnonymousBlock);
}


TEST_CASE("LTB build: display:none filters whole subtree",
    "[ltb][build][display_none]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div><span><b>hi</b></span></div><p></p>",
        "html, body, div, p { display: block; } "
        "div { display: none; }");
    REQUIRE(p.root != nullptr);
    // html + body + p = 3
    REQUIRE(count_nodes(p.root.get()) == 3);
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    REQUIRE(body->children.size() == 1);
    REQUIRE(body->children[0]->type == BoxType::Block);   // <p>
}



TEST_CASE("LTB build: mixed inline/block triggers anonymous wrapping",
    "[ltb][build][anon]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div>hello<span>world</span><p>para</p>tail</div>",
        "html, body, div, p { display: block; } "
        "span { display: inline; }");

    // root = html, тогда body, потом div
    REQUIRE(p.root != nullptr);
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    REQUIRE(body != nullptr);
    LayoutNode* div = child_of_type(body, BoxType::Block);
    REQUIRE(div != nullptr);

    // Ожидаем: [AnonBlock[Text, Inline[AnonBlock[Text]]], Block[...], AnonBlock[Text]]
    REQUIRE(div->children.size() == 3);
    REQUIRE(div->children[0]->type == BoxType::AnonymousBlock);
    REQUIRE(div->children[0]->children.size() == 2);
    REQUIRE(div->children[0]->children[0]->type == BoxType::Text);
    REQUIRE(div->children[0]->children[1]->type == BoxType::Inline);

    REQUIRE(div->children[1]->type == BoxType::Block);

    REQUIRE(div->children[2]->type == BoxType::AnonymousBlock);
    REQUIRE(div->children[2]->children.size() == 1);
    REQUIRE(div->children[2]->children[0]->type == BoxType::Text);
}

TEST_CASE("LTB build: all-inline children wrapped in a single anonymous block",
    "[ltb][build][anon]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div>a<span>b</span>c</div>",
        "html, body, div { display: block; } "
        "span { display: inline; }");

    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* div = child_of_type(body, BoxType::Block);

    // Все inline-дети обёрнуты в один AnonymousBlock.
    REQUIRE(div->children.size() == 1);
    REQUIRE(div->children[0]->type == BoxType::AnonymousBlock);

    const auto* anon = div->children[0].get();
    // Text "a", Inline <span>, Text "c"
    REQUIRE(anon->children.size() == 3);
    REQUIRE(anon->children[0]->type == BoxType::Text);
    REQUIRE(anon->children[1]->type == BoxType::Inline);
    REQUIRE(anon->children[2]->type == BoxType::Text);
}

TEST_CASE("LTB build: anonymous block has no style_soa_idx",
    "[ltb][build][anon]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div>text<div></div></div>",
        "html, body, div { display: block; }");
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* div = child_of_type(body, BoxType::Block);
    LayoutNode* anon = child_of_type(div, BoxType::AnonymousBlock);
    REQUIRE(anon != nullptr);
    REQUIRE(anon->style_soa_idx == UINT32_MAX);
}



TEST_CASE("LTB build: text node preserves content",
    "[ltb][build][text]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div>hello</div>",
        "html, body, div { display: block; }");
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* div = child_of_type(body, BoxType::Block);
    REQUIRE(div->children.size() == 1);
    REQUIRE(div->children[0]->type == BoxType::AnonymousBlock);

    LayoutNode* txt = first_text_deep(div->children[0].get());
    REQUIRE(txt != nullptr);
    REQUIRE(txt->text_content == "hello");
}

TEST_CASE("LTB build: whitespace-only text is filtered",
    "[ltb][build][text]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div>   \n\t  </div>",
        "html, body, div { display: block; }");
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* div = child_of_type(body, BoxType::Block);
    REQUIRE(div->children.empty());
}

TEST_CASE("LTB build: text mixed with whitespace — collapsed inside anon block",
    "[ltb][build][text]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div>  hello  </div>",
        "html, body, div { display: block; }");
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* div = child_of_type(body, BoxType::Block);
    REQUIRE(div->children.size() == 1);
    REQUIRE(div->children[0]->type == BoxType::AnonymousBlock);

    // Ведущие/замыкающие пробелы схлопываются в одиночные:
    // [Text " "] [Text "hello"] [Text " "]
    const auto* anon = div->children[0].get();
    REQUIRE(anon->children.size() == 3);
    REQUIRE(anon->children[0]->text_content == " ");
    REQUIRE(anon->children[1]->text_content == "hello");
    REQUIRE(anon->children[2]->text_content == " ");
}

TEST_CASE("LTB build: text node has no style_soa_idx",
    "[ltb][build][text]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div>hi</div>",
        "html, body, div { display: block; }");
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* div = child_of_type(body, BoxType::Block);
    LayoutNode* anon = child_of_type(div, BoxType::AnonymousBlock);
    REQUIRE(anon != nullptr);

    LayoutNode* txt = first_text_deep(anon);
    REQUIRE(txt != nullptr);
    REQUIRE(txt->style_soa_idx == UINT32_MAX);
}



TEST_CASE("LTB layout: two blocks stack vertically",
    "[ltb][layout][stack]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div id='d1'></div><div id='d2'></div>",
        "html, body, div { display: block; } "
        "#d1 { height: 50px; } "
        "#d2 { height: 30px; }");
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    REQUIRE(body->children.size() == 2);
    REQUIRE(body->children[0]->geometry.y == 0.0f);
    REQUIRE(body->children[0]->geometry.height == 50.0f);
    REQUIRE(body->children[1]->geometry.y == 50.0f);
    REQUIRE(body->children[1]->geometry.height == 30.0f);
    REQUIRE(body->geometry.height == 80.0f);
}

TEST_CASE("LTB layout: adjacent sibling margins collapse",
    "[ltb][layout][stack][margin]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div id='d1'></div><div id='d2'></div>",
        "html, body { display: block; } "
        "div { display: block; } "
        "#d1 { height: 50px; margin-bottom: 10px; } "
        "#d2 { height: 30px; margin-top: 20px; }");

    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* d1 = body->children[0].get();
    LayoutNode* d2 = body->children[1].get();

    REQUIRE(d1->geometry.y == 0.0f);
    REQUIRE(d1->geometry.height == 50.0f);
    // Зазор = max(10, 20) = 20, а не 10 + 20 = 30
    REQUIRE(d2->geometry.y == 70.0f);
    REQUIRE(body->geometry.height == 100.0f);  // 50 + 20 + 30
}

TEST_CASE("LTB layout: opposite-sign margins collapse to sum",
    "[ltb][layout][margin][collapse]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div id='d1'></div><div id='d2'></div>",
        "html, body { display: block; margin: 0; padding: 0; } "
        "div { display: block; height: 40px; } "
        "#d1 { margin-bottom: -5px; } "
        "#d2 { margin-top: 10px; }",
        500.0f);
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* d1 = body->children[0].get();
    LayoutNode* d2 = body->children[1].get();
    REQUIRE(d1->geometry.y == 0.0f);
    // -5 + 10 = 5 (разные знаки → сумма)
    REQUIRE(d2->geometry.y == 45.0f);
    REQUIRE(body->geometry.height == 85.0f);  // 40 + 5 + 40
}

TEST_CASE("LTB layout: two negative margins collapse to most negative",
    "[ltb][layout][margin][collapse]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div id='d1'></div><div id='d2'></div>",
        "html, body { display: block; margin: 0; padding: 0; } "
        "div { display: block; height: 40px; } "
        "#d1 { margin-bottom: -5px; } "
        "#d2 { margin-top: -10px; }",
        500.0f);
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* d2 = body->children[1].get();
    // min(-5, -10) = -10
    REQUIRE(d2->geometry.y == 30.0f);
}

TEST_CASE("LTB layout: child margin-left shifts x",
    "[ltb][layout][stack][margin]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div id='d1'></div>",
        "html, body { display: block; } "
        "div { display: block; height: 40px; margin-left: 32px; }",
        800.0f);
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* d1 = body->children[0].get();
    REQUIRE(d1->geometry.margin_left == 32.0f);
    REQUIRE(d1->geometry.x == 32.0f);
}




TEST_CASE("LTB layout: content-box — width excludes padding/border",
    "[ltb][layout][box-sizing]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div></div>",
        "html, body { display: block; } "
        "div { display: block; width: 200px; "
        "      padding-left: 10px; padding-right: 20px; "
        "      border-left-width: 5px; border-right-width: 5px; }");
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* div = child_of_type(body, BoxType::Block);
    // content 200 + padding 30 + border 10 = 240
    REQUIRE(div->geometry.width == 240.0f);
}

TEST_CASE("LTB layout: border-box — width is total",
    "[ltb][layout][box-sizing]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div></div>",
        "html, body { display: block; } "
        "div { display: block; box-sizing: border-box; "
        "      width: 200px; "
        "      padding-left: 10px; padding-right: 20px; "
        "      border-left-width: 5px; border-right-width: 5px; }");
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* div = child_of_type(body, BoxType::Block);
    // total width = 200
    REQUIRE(div->geometry.width == 200.0f);
}



TEST_CASE("LTB layout: inline shrink-to-fit around text",
    "[ltb][layout][inline]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<p><span>ab</span></p>",
        "html, body, p { display: block; } "
        "span { display: inline; }");
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* p_ = child_of_type(body, BoxType::Block);
    LayoutNode* anon = child_of_type(p_, BoxType::AnonymousBlock);
    REQUIRE(anon != nullptr);

    LayoutNode* sp = child_of_type(anon, BoxType::Inline);
    REQUIRE(sp != nullptr);
    // span не задан явно — shrink-to-fit по содержимому
    REQUIRE(sp->geometry.width > 0.0f);
}



TEST_CASE("LTB layout: single-line text height = line-height",
    "[ltb][layout][text]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div>hi</div>",
        "html, body, div { display: block; font-size: 16px; }",
        500.0f);
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* div = child_of_type(body, BoxType::Block);
    LayoutNode* anon = child_of_type(div, BoxType::AnonymousBlock);
    REQUIRE(anon != nullptr);

    LayoutNode* txt = first_text_deep(anon);
    REQUIRE(txt != nullptr);
    // line-height normal = 16 * 1.2 = 19.2
    REQUIRE(txt->geometry.height == Approx(19.2f).epsilon(0.001));
}

TEST_CASE("LTB layout: text wrapping when longer than parent",
    "[ltb][layout][text]") {
    ArenaAllocator arena;
    // "hello world" разбивается на: [Text "hello"] [Text " "] [Text "world"]
    // fs=16, char_w=8.8 → "hello"=44, " "=8.8, "world"=44
    // avail=50: "hello"(44) помещается, " "(8.8) не влезает → схлопывается,
    // "world"(44) идёт на новую строку.
    // Итог: 2 строки.
    auto p = run_pipeline(arena,
        "<div>hello world</div>",
        "html, body { display: block; } "
        "div { display: block; width: 50px; font-size: 16px; }",
        500.0f);
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* div = child_of_type(body, BoxType::Block);
    LayoutNode* anon = child_of_type(div, BoxType::AnonymousBlock);
    REQUIRE(anon != nullptr);
    // 2 строки × 19.2 = 38.4
    REQUIRE(anon->geometry.height == Approx(38.4f).epsilon(0.01));
    REQUIRE(anon->geometry.width == 50.0f);
}

TEST_CASE("LTB layout: text shorter than parent — no wrap",
    "[ltb][layout][text]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div>hi</div>",
        "html, body, div { display: block; font-size: 16px; }",
        500.0f);
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* div = child_of_type(body, BoxType::Block);
    LayoutNode* anon = child_of_type(div, BoxType::AnonymousBlock);
    REQUIRE(anon != nullptr);

    LayoutNode* txt = first_text_deep(anon);
    REQUIRE(txt != nullptr);
    // "hi" = 2 симв → one_line_w = 16px, меньше parent 500
    REQUIRE(txt->geometry.width == Catch::Approx(17.6f).epsilon(0.001));
}



TEST_CASE("LTB integration: card example — full pipeline",
    "[ltb][integration]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div class='card'><h1>Title</h1><p>Body</p></div>",
        "html, body { display: block; margin: 0; padding: 0; } "
        ".card { display: block; "
        "        margin: 24px; padding: 24px; "
        "        width: 400px; } "
        "h1 { display: block; font-size: 32px; margin-bottom: 12px; } "
        "p  { display: block; font-size: 16px; margin-bottom: 16px; }",
        1024.0f);

    REQUIRE(p.root != nullptr);
    // html → body → .card → h1, p
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* card = child_of_type(body, BoxType::Block);
    REQUIRE(card->geometry.margin_left == 24.0f);
    REQUIRE(card->geometry.margin_right == 24.0f);
    REQUIRE(card->geometry.padding_left == 24.0f);
    REQUIRE(card->geometry.padding_right == 24.0f);
    // total width = 400 (content) + 48 (padding) = 448 (content-box)
    REQUIRE(card->geometry.width == 448.0f);

    // У card два блочных ребёнка
    REQUIRE(card->children.size() == 2);
    LayoutNode* h1 = card->children[0].get();
    LayoutNode* p_ = card->children[1].get();
    REQUIRE(h1->type == BoxType::Block);
    REQUIRE(p_->type == BoxType::Block);

    // h1 стоит первым: y = border-top + padding-top = 24
    REQUIRE(h1->geometry.y == 24.0f);
    // p ниже, с учётом margin-bottom у h1
    REQUIRE(p_->geometry.y > h1->geometry.y + h1->geometry.height);
}

TEST_CASE("LTB integration: html>body>div>p vertical positions",
    "[ltb][integration]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div><p>one</p><p>two</p><p>three</p></div>",
        "html, body, div, p { display: block; } "
        "div { padding: 10px; } "
        "p   { height: 20px; margin: 0; }",
        600.0f);
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* div = child_of_type(body, BoxType::Block);
    REQUIRE(div->children.size() == 3);
    REQUIRE(div->children[0]->geometry.y == 10.0f);
    REQUIRE(div->children[1]->geometry.y == 30.0f);
    REQUIRE(div->children[2]->geometry.y == 50.0f);
    // height = padding-top(10) + 3 * 20 + padding-bottom(10) = 80
    REQUIRE(div->geometry.height == 80.0f);
}

TEST_CASE("LTB integration: display:none child skipped in layout",
    "[ltb][integration]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div><p id='a'></p><p id='b'></p><p id='c'></p></div>",
        "html, body, div, p { display: block; } "
        "p { height: 30px; margin: 0; } "
        "#b { display: none; }");
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* div = child_of_type(body, BoxType::Block);
    REQUIRE(div->children.size() == 2);
    REQUIRE(div->children[0]->geometry.y == 0.0f);
    REQUIRE(div->children[1]->geometry.y == 30.0f);   // без пропуска
    REQUIRE(div->geometry.height == 60.0f);
}

TEST_CASE("LTB layout: br starts a new line", "[ltb][layout][br]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div>line1<br>line2</div>",
        "html, body, div { display: block; margin: 0; padding: 0; }", 500.0f);

    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    REQUIRE(body != nullptr);
    LayoutNode* div = child_of_type(body, BoxType::Block);
    REQUIRE(div != nullptr);
    LayoutNode* first = nullptr;
    LayoutNode* second = nullptr;
    for (const auto& child : div->children) {
        if (child->type != BoxType::AnonymousBlock) continue;
        for (const auto& inline_child : child->children) {
            if (inline_child->type != BoxType::Text || inline_child->text_content == " ") continue;
            if (!first) first = inline_child.get();
            else if (!second) second = inline_child.get();
        }
    }
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    REQUIRE(second->geometry.y > first->geometry.y);
    REQUIRE(div->geometry.height == Approx(2.0f * 16.0f * 1.2f));
}
// ============================================================

TEST_CASE("LTB layout: auto width fills parent",
    "[ltb][layout][auto]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div></div>",
        "html, body { display: block; margin: 0; padding: 0; } "
        "div { display: block; }",
        800.0f);
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* div = child_of_type(body, BoxType::Block);
    REQUIRE(div->geometry.width == 800.0f);
}

TEST_CASE("LTB layout: min-width overrides smaller auto",
    "[ltb][layout][min_max]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div></div>",
        "html, body { display: block; margin: 0; padding: 0; width: 100px; } "
        "div { display: block; width: auto; min-width: 200px; }",
        800.0f);
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* div = child_of_type(body, BoxType::Block);
    REQUIRE(div->geometry.width == 200.0f);
}

TEST_CASE("LTB layout: max-width caps larger width",
    "[ltb][layout][min_max]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div></div>",
        "html, body { display: block; margin: 0; padding: 0; width: 1000px; } "
        "div { display: block; width: auto; max-width: 300px; }",
        800.0f);
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* div = child_of_type(body, BoxType::Block);
    REQUIRE(div->geometry.width == 300.0f);
}

TEST_CASE("LTB layout: min-height on empty div",
    "[ltb][layout][min_max]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div></div>",
        "html, body { display: block; margin: 0; padding: 0; } "
        "div { display: block; min-height: 50px; }",
        500.0f);
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* div = child_of_type(body, BoxType::Block);
    REQUIRE(div->geometry.height == 50.0f);
}

TEST_CASE("LTB layout: two inline-blocks sit side by side",
    "[ltb][layout][inline]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<p><span>a</span><span>b</span></p>",
        "html, body, p { display: block; } "
        "span { display: inline-block; width: 20px; height: 10px; }",
        500.0f);
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* p_ = child_of_type(body, BoxType::Block);
    LayoutNode* anon = child_of_type(p_, BoxType::AnonymousBlock);
    REQUIRE(anon != nullptr);
    REQUIRE(anon->children.size() == 2);

    LayoutNode* s1 = anon->children[0].get();
    LayoutNode* s2 = anon->children[1].get();
    REQUIRE(s1->geometry.x == 0.0f);
    REQUIRE(s2->geometry.x == 20.0f);   // теперь рядом
}

TEST_CASE("LTB layout: negative margin-top shifts upwards",
    "[ltb][layout][margin]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div id='a'></div><div id='b'></div>",
        "html, body { display: block; margin: 0; padding: 0; } "
        "div { display: block; height: 40px; margin: 0; } "
        "#b { margin-top: -10px; }",
        500.0f);
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* a = body->children[0].get();
    LayoutNode* b = body->children[1].get();
    REQUIRE(a->geometry.y == 0.0f);
    REQUIRE(b->geometry.y == 30.0f);
}

TEST_CASE("LTB layout: border-box with padding and border",
    "[ltb][layout][box-sizing]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div></div>",
        "html, body { display: block; margin: 0; padding: 0; } "
        "div { display: block; box-sizing: border-box; "
        "      width: 200px; height: 100px; padding: 10px; "
        "      border-top-width: 2px; border-right-width: 2px; "
        "      border-bottom-width: 2px; border-left-width: 2px; }",
        500.0f);
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* div = child_of_type(body, BoxType::Block);
    REQUIRE(div->geometry.width == 200.0f);
    REQUIRE(div->geometry.height == 100.0f);
}

TEST_CASE("LTB layout: zero-size div keeps 0 height",
    "[ltb][layout][empty]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div></div>",
        "html, body { display: block; margin: 0; padding: 0; } "
        "div { display: block; }",
        500.0f);
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* div = child_of_type(body, BoxType::Block);
    REQUIRE(div->geometry.height == 0.0f);
    REQUIRE(div->geometry.width == 500.0f);
}

TEST_CASE("LTB integration: nested cards stacked vertically",
    "[ltb][integration]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div class='card'><div class='inner'>A</div></div>"
        "<div class='card'><div class='inner'>B</div></div>",
        "html, body { display: block; margin: 0; padding: 0; } "
        ".card  { display: block; padding: 10px; margin-bottom: 20px; height: 60px; } "
        ".inner { display: block; height: 20px; }",
        500.0f);
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    REQUIRE(body->children.size() == 2);
    LayoutNode* c1 = body->children[0].get();
    LayoutNode* c2 = body->children[1].get();
    REQUIRE(c1->geometry.y == 0.0f);
    REQUIRE(c2->geometry.y == 100.0f);
}

TEST_CASE("LTB layout: parent-child margin collapses through body",
    "[ltb][layout][margin][collapse]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div id='parent'><div id='child'></div></div>",
        "html, body { display: block; margin: 0; padding: 0; } "
        "#parent, #child { display: block; height: 10px; } "
        "#child { margin-top: 15px; }");

    LayoutNode* html = p.root.get();
    LayoutNode* body = child_of_type(html, BoxType::Block);
    LayoutNode* parent = child_of_type(body, BoxType::Block);
    LayoutNode* child = child_of_type(parent, BoxType::Block);

    REQUIRE(parent != nullptr);
    REQUIRE(child != nullptr);

    // Margin вытек из child в parent, потом в body (цепочка collapse).
    REQUIRE(parent->geometry.margin_top == 15.0f);
    REQUIRE(body->geometry.margin_top == 15.0f);

    // Позиции в локальных координатах:
    REQUIRE(parent->geometry.y == 0.0f);    // относительно body
    REQUIRE(body->geometry.y == 15.0f);   // относительно html
    REQUIRE(child->geometry.y == 0.0f);    // относительно parent
}

TEST_CASE("LTB layout: parent bottom margin collapses with last block child",
    "[ltb][layout][margin]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div id='parent'><div id='child'></div></div><div id='next'></div>",
        "html, body { display: block; margin: 0; padding: 0; } "
        "#parent { display: block; } "
        "#child { display: block; height: 20px; margin-bottom: 15px; } "
        "#next { display: block; height: 10px; }",
        500.0f);
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* parent = body->children[0].get();
    LayoutNode* child = child_of_type(parent, BoxType::Block);
    LayoutNode* next = body->children[1].get();

    REQUIRE(parent->geometry.height == 20.0f);
    REQUIRE(parent->geometry.margin_bottom == 15.0f);
    REQUIRE(child->geometry.y == 0.0f);
    REQUIRE(next->geometry.y == 35.0f);
}

TEST_CASE("LTB relative: left shifts x, y unchanged",
    "[ltb][layout][position][relative]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div id='a'></div>",
        "#a { display: block; position: relative; left: 10px; "
        "width: 50px; height: 50px; }");
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* a = child_of_type(body, BoxType::Block);
    REQUIRE(a != nullptr);
    REQUIRE(a->geometry.x == Approx(10.0f).epsilon(0.001));
    REQUIRE(a->geometry.y == Approx(0.0f).epsilon(0.001));
}

TEST_CASE("LTB relative: top shifts y",
    "[ltb][layout][position][relative]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div id='a'></div>",
        "#a { display: block; position: relative; top: 20px; "
        "width: 50px; height: 50px; }");
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* a = child_of_type(body, BoxType::Block);
    REQUIRE(a != nullptr);
    REQUIRE(a->geometry.y == Approx(20.0f).epsilon(0.001));
    REQUIRE(a->geometry.x == Approx(0.0f).epsilon(0.001));
}

TEST_CASE("LTB relative: negative offset",
    "[ltb][layout][position][relative]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div id='a'></div>",
        "#a { display: block; position: relative; left: -5px; "
        "width: 50px; height: 50px; }");
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* a = child_of_type(body, BoxType::Block);
    REQUIRE(a != nullptr);
    REQUIRE(a->geometry.x == Approx(-5.0f).epsilon(0.001));
}

TEST_CASE("LTB relative: no inset does nothing",
    "[ltb][layout][position][relative]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div id='a'></div>",
        "#a { display: block; position: relative; width: 50px; height: 50px; }");
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* a = child_of_type(body, BoxType::Block);
    REQUIRE(a != nullptr);
    REQUIRE(a->geometry.x == Approx(0.0f).epsilon(0.001));
    REQUIRE(a->geometry.y == Approx(0.0f).epsilon(0.001));
}

TEST_CASE("LTB relative: sibling not shifted by previous relative",
    "[ltb][layout][position][relative]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div id='a'></div><div id='b'></div>",
        "#a { display: block; position: relative; left: 20px; height: 40px; } "
        "#b { display: block; height: 40px; }");
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    REQUIRE(body != nullptr);
    REQUIRE(body->children.size() == 2);
    LayoutNode* a = body->children[0].get();
    LayoutNode* b = body->children[1].get();
    REQUIRE(a->geometry.x == Approx(20.0f).epsilon(0.001));
    REQUIRE(b->geometry.y == Approx(40.0f).epsilon(0.001));
    REQUIRE(b->geometry.x == Approx(0.0f).epsilon(0.001));
}

TEST_CASE("LTB relative: flow_x/flow_y preserved",
    "[ltb][layout][position][relative]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div id='a'></div>",
        "#a { display: block; position: relative; left: 10px; top: 5px; }");

    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* a = child_of_type(body, BoxType::Block);
    REQUIRE(a != nullptr);
    REQUIRE(a->geometry.x == Approx(10.0f).epsilon(0.001));
    REQUIRE(a->geometry.y == Approx(5.0f).epsilon(0.001));
    REQUIRE(a->geometry.flow_x == Approx(0.0f).epsilon(0.001));
    REQUIRE(a->geometry.flow_y == Approx(0.0f).epsilon(0.001));
}



TEST_CASE("LTB absolute: left+top+width+height positions absolutely",
    "[ltb][layout][position][absolute]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div id='cb'><span id='a'></span></div>",
        "#cb { display: block; position: relative; width: 200px; height: 100px; } "
        "#a { position: absolute; left: 10px; top: 20px; width: 30px; height: 40px; }");
    LayoutNode* a = find_by_soa_idx(p.root.get(), 3);
    REQUIRE(a != nullptr);
    REQUIRE(a->geometry.x == Approx(10.0f).epsilon(0.001));
    REQUIRE(a->geometry.y == Approx(20.0f).epsilon(0.001));
    REQUIRE(a->geometry.width == Approx(30.0f).epsilon(0.001));
    REQUIRE(a->geometry.height == Approx(40.0f).epsilon(0.001));
}

TEST_CASE("LTB absolute: not in flow — doesn't affect sibling",
    "[ltb][layout][position][absolute]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div><p id='a'></p><p id='b'></p></div>",
        "div { display: block; } "                       // ← добавлено
        "p { display: block; height: 30px; margin: 0; } "
        "#a { position: absolute; top: 0; left: 0; }");
    LayoutNode* b = find_by_soa_idx(p.root.get(), 4);
    REQUIRE(b != nullptr);
    REQUIRE(b->geometry.y == Approx(0.0f).epsilon(0.001));
}

TEST_CASE("LTB absolute: right + width",
    "[ltb][layout][position][absolute]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='cb'><p id='a'></p></div>",
        "#cb { position: relative; width: 200px; height: 100px; } "
        "#a { position: absolute; right: 10px; width: 30px; height: 20px; }");
    LayoutNode* a = find_by_soa_idx(p.root.get(), 3);
    REQUIRE(a != nullptr);
    REQUIRE(a->geometry.x == Approx(160.0f).epsilon(0.001));
}

TEST_CASE("LTB absolute: no left/right — static position",
    "[ltb][layout][position][absolute]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div id='cb'><p>x</p><span id='a'></span></div>",
        "#cb { position: relative; width: 200px; height: 100px; } "
        "#a { position: absolute; top: 50px; width: 20px; height: 10px; }");
    LayoutNode* a = find_by_soa_idx(p.root.get(), 4);
    REQUIRE(a != nullptr);
    REQUIRE(a->geometry.y == Approx(50.0f).epsilon(0.001));
    REQUIRE(a->geometry.x == Approx(a->geometry.flow_x).epsilon(0.001));
}


TEST_CASE("LTB absolute: inline blockified to block", "[ltb][layout][position][absolute]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<span style='position:absolute'>x</span>", "span { position: absolute; }");
    REQUIRE(p.root != nullptr);
    REQUIRE(p.root->absolute_children.size() == 1);
    REQUIRE(p.root->absolute_children[0]->type == BoxType::Block);
}

TEST_CASE("LTB absolute: registered under relative parent", "[ltb][layout][position][absolute]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='p'><span id='c'></span></div>", "#p { display: block; position: relative; } #c { position: absolute; }");
    REQUIRE(p.storage.size() >= 4);
    LayoutNode* parent = find_by_soa_idx(p.root.get(), 2);
    REQUIRE(parent != nullptr);
    REQUIRE(parent->absolute_children.size() == 1);
    REQUIRE(parent->absolute_children[0] == find_by_soa_idx(p.root.get(), 3));
}

TEST_CASE("LTB absolute: registered under nearest relative ancestor", "[ltb][layout][position][absolute]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='outer'><div id='inner'><span id='c'></span></div></div>", "#outer { position: relative; } #inner { display: block; } #c { position: absolute; }");
    LayoutNode* outer = find_by_soa_idx(p.root.get(), 2);
    LayoutNode* inner = find_by_soa_idx(p.root.get(), 3);
    REQUIRE(outer != nullptr);
    REQUIRE(inner != nullptr);
    REQUIRE(outer->absolute_children.size() == 1);
    REQUIRE(inner->absolute_children.empty());
    REQUIRE(outer->absolute_children[0] == find_by_soa_idx(p.root.get(), 4));
}

TEST_CASE("LTB absolute: no CB registered under root", "[ltb][layout][position][absolute]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div><span style='position:absolute'></span></div>", "");
    REQUIRE(p.root != nullptr);
    REQUIRE(p.root->absolute_children.size() == 1);
    REQUIRE(p.root->absolute_children[0]->type == BoxType::Block);
}

TEST_CASE("LTB absolute: blockification not for static inline", "[ltb][layout][position][absolute]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<span>x</span>", "span { display: inline; position: static; }");
    LayoutNode* span = find_by_soa_idx(p.root.get(), 2);
    REQUIRE(span != nullptr);
    REQUIRE(span->type == BoxType::Inline);
}


TEST_CASE("LTB absolute: nested absolute CB chain",
    "[ltb][layout][position][absolute][nested]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div id='cb'><div id='outer'><span id='inner'></span></div></div>",
        "#cb    { display: block; position: relative; width: 300px; height: 200px; } "
        "#outer { position: absolute; } "
        "#inner { position: absolute; }");

    LayoutNode* cb    = find_by_soa_idx(p.root.get(), 2);
    LayoutNode* outer = find_by_soa_idx(p.root.get(), 3);
    LayoutNode* inner = find_by_soa_idx(p.root.get(), 4);

    REQUIRE(cb    != nullptr);
    REQUIRE(outer != nullptr);
    REQUIRE(inner != nullptr);

    // outer зарегистрирован у cb (единственный CB между ним и root)
    REQUIRE(cb->absolute_children.size() == 1);
    REQUIRE(cb->absolute_children[0] == outer);

    // inner зарегистрирован у outer (outer сам absolute → CB)
    REQUIRE(outer->absolute_children.size() == 1);
    REQUIRE(outer->absolute_children[0] == inner);
}

TEST_CASE("LTB absolute: nested absolute layout",
    "[ltb][layout][position][absolute][nested]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div id='cb'><div id='outer'><span id='inner'></span></div></div>",
        "#cb    { display: block; position: relative; width: 300px; height: 200px; } "
        "#outer { position: absolute; left: 50px; top: 50px; "
        "         width: 100px; height: 100px; } "
        "#inner { position: absolute; left: 10px; top: 10px; "
        "         width: 20px; height: 20px; }");

    LayoutNode* outer = find_by_soa_idx(p.root.get(), 3);
    LayoutNode* inner = find_by_soa_idx(p.root.get(), 4);
    REQUIRE(outer != nullptr);
    REQUIRE(inner != nullptr);

    // outer относительно cb content box
    REQUIRE(outer->geometry.x == Approx(50.0f).epsilon(0.001));
    REQUIRE(outer->geometry.y == Approx(50.0f).epsilon(0.001));
    REQUIRE(outer->geometry.width  == Approx(100.0f).epsilon(0.001));
    REQUIRE(outer->geometry.height == Approx(100.0f).epsilon(0.001));

    // inner относительно outer content box (outer — его CB)
    REQUIRE(inner->geometry.x == Approx(10.0f).epsilon(0.001));
    REQUIRE(inner->geometry.y == Approx(10.0f).epsilon(0.001));
    REQUIRE(inner->geometry.width  == Approx(20.0f).epsilon(0.001));
    REQUIRE(inner->geometry.height == Approx(20.0f).epsilon(0.001));
}


TEST_CASE("LTB flex row: single child with fixed width", "[ltb][layout][flex][row]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'><p id='c'></p></div>",
        "#f { display: flex; } #c { display: block; width: 100px; height: 30px; }");
    auto* c = find_by_soa_idx(p.root.get(), 3);
    REQUIRE(c != nullptr);
    REQUIRE(c->geometry.x == Approx(0.0f).epsilon(0.001));
    REQUIRE(c->geometry.y == Approx(0.0f).epsilon(0.001));
    REQUIRE(c->geometry.width == Approx(100.0f).epsilon(0.001));
    REQUIRE(c->geometry.height == Approx(30.0f).epsilon(0.001));
}

TEST_CASE("LTB flex row: two children side by side", "[ltb][layout][flex][row]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'><p id='a'></p><p id='b'></p></div>",
        "#f { display: flex; } #a { display: block; width: 50px; height: 20px; } "
        "#b { display: block; width: 70px; height: 30px; }");
    auto* f = find_by_soa_idx(p.root.get(), 2);
    auto* a = find_by_soa_idx(p.root.get(), 3);
    auto* b = find_by_soa_idx(p.root.get(), 4);
    REQUIRE(a->geometry.x == Approx(0.0f).epsilon(0.001));
    REQUIRE(b->geometry.x == Approx(50.0f).epsilon(0.001));
    REQUIRE(a->geometry.width == Approx(50.0f).epsilon(0.001));
    REQUIRE(b->geometry.width == Approx(70.0f).epsilon(0.001));
    REQUIRE(f->geometry.height == Approx(30.0f).epsilon(0.001));
}

TEST_CASE("LTB flex row: child margin-right shifts next item", "[ltb][layout][flex][row]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'><p id='a'></p><p id='b'></p></div>",
        "#f { display: flex; } #a { display: block; width: 50px; height: 20px; margin-right: 10px; } "
        "#b { display: block; width: 70px; height: 20px; }");
    REQUIRE(find_by_soa_idx(p.root.get(), 4)->geometry.x == Approx(60.0f).epsilon(0.001));
}

TEST_CASE("LTB flex row: container padding", "[ltb][layout][flex][row]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'><p id='a'></p></div>",
        "#f { display: flex; padding: 10px; } #a { display: block; width: 50px; height: 20px; }");
    auto* f = find_by_soa_idx(p.root.get(), 2);
    auto* a = find_by_soa_idx(p.root.get(), 3);
    REQUIRE(a->geometry.x == Approx(10.0f).epsilon(0.001));
    REQUIRE(a->geometry.y == Approx(10.0f).epsilon(0.001));
    REQUIRE(f->geometry.height == Approx(40.0f).epsilon(0.001));
    REQUIRE(f->geometry.width == Approx(70.0f).epsilon(0.001));
}

TEST_CASE("LTB flex row: margin collapsing NOT applied",
    "[ltb][layout][flex][row]") {
    // В block layout a и b стекались бы вертикально с collapsed margin.
    // В flex они на одной строке: b.x сдвинут, b.y = собственный margin_top,
    // а не collapse с a.margin_bottom.
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div id='f'><p id='a'></p><p id='b'></p></div>",
        "#f { display: flex; } "
        "#a { display: block; width: 50px; height: 20px; margin-bottom: 10px; } "
        "#b { display: block; width: 50px; height: 20px; margin-top: 20px; }");
    LayoutNode* a = find_by_soa_idx(p.root.get(), 3);
    LayoutNode* b = find_by_soa_idx(p.root.get(), 4);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    // На одной строке
    REQUIRE(a->geometry.x == Approx(0.0f).epsilon(0.001));
    REQUIRE(b->geometry.x == Approx(50.0f).epsilon(0.001));
    // a без margin-top, b со своим margin-top (не collapsed)
    REQUIRE(a->geometry.y == Approx(0.0f).epsilon(0.001));
    REQUIRE(b->geometry.y == Approx(20.0f).epsilon(0.001));
}

TEST_CASE("LTB flex row: container fixed width and height", "[ltb][layout][flex][row]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'><p id='a'></p></div>",
        "#f { display: flex; width: 200px; height: 100px; } #a { display: block; width: 50px; height: 20px; }");
    auto* f = find_by_soa_idx(p.root.get(), 2);
    auto* a = find_by_soa_idx(p.root.get(), 3);
    REQUIRE(f->geometry.width == Approx(200.0f).epsilon(0.001));
    REQUIRE(f->geometry.height == Approx(100.0f).epsilon(0.001));
    REQUIRE(a->geometry.x == Approx(0.0f).epsilon(0.001));
    REQUIRE(a->geometry.y == Approx(0.0f).epsilon(0.001));
}

TEST_CASE("LTB flex row: absolute child not in flow", "[ltb][layout][flex][row]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'><p id='a'></p><span id='abs' style='position:absolute'></span><p id='b'></p></div>",
        "#f { display: flex; } #a, #b { display: block; width: 40px; height: 20px; }");
    auto* b = find_by_soa_idx(p.root.get(), 5);
    REQUIRE(b != nullptr);
    REQUIRE(b->geometry.x == Approx(40.0f).epsilon(0.001));
}

TEST_CASE("LTB flex row: inline-block child with fixed width", "[ltb][layout][flex][row]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'><span id='a'></span></div>",
        "#f { display: flex; } #a { display: inline-block; width: 60px; height: 20px; }");
    auto* a = find_by_soa_idx(p.root.get(), 3);
    REQUIRE(a->geometry.x == Approx(0.0f).epsilon(0.001));
    REQUIRE(a->geometry.width == Approx(60.0f).epsilon(0.001));
}

TEST_CASE("LTB flex row: three items", "[ltb][layout][flex][row]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'><p id='a'></p><p id='b'></p><p id='c'></p></div>",
        "#f { display: flex; } #a { display: block; width: 30px; height: 10px; } "
        "#b { display: block; width: 40px; height: 20px; } #c { display: block; width: 50px; height: 15px; }");
    auto* f = find_by_soa_idx(p.root.get(), 2);
    REQUIRE(find_by_soa_idx(p.root.get(), 3)->geometry.x == Approx(0.0f).epsilon(0.001));
    REQUIRE(find_by_soa_idx(p.root.get(), 4)->geometry.x == Approx(30.0f).epsilon(0.001));
    REQUIRE(find_by_soa_idx(p.root.get(), 5)->geometry.x == Approx(70.0f).epsilon(0.001));
    REQUIRE(f->geometry.height == Approx(20.0f).epsilon(0.001));
}

TEST_CASE("LTB flex row: border-box on flex item", "[ltb][layout][flex][row]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'><p id='a'></p></div>",
        "#f { display: flex; } #a { display: block; box-sizing: border-box; width: 100px; padding: 10px; height: 30px; }");
    auto* a = find_by_soa_idx(p.root.get(), 3);
    REQUIRE(a->geometry.width == Approx(100.0f).epsilon(0.001));
    REQUIRE(a->geometry.height == Approx(30.0f).epsilon(0.001));
}


TEST_CASE("LTB flex intrinsic: single text child measures its width", "[ltb][layout][flex][intrinsic]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'><p id='c'>hello</p></div>",
        "#f { display: flex; } #c { display: block; }");
    REQUIRE(find_by_soa_idx(p.root.get(), 3)->geometry.width > 0.0f);
}

TEST_CASE("LTB flex intrinsic: two text children stack in row", "[ltb][layout][flex][intrinsic]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'><p id='a'>hello</p><p id='b'>world</p></div>",
        "#f { display: flex; } #a, #b { display: block; }");
    auto* a = find_by_soa_idx(p.root.get(), 3);
    auto* b = find_by_soa_idx(p.root.get(), 4);
    REQUIRE(b->geometry.x >= a->geometry.width);
}

TEST_CASE("LTB flex intrinsic: explicit width overrides intrinsic", "[ltb][layout][flex][intrinsic]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'><p id='a'>long text here</p></div>",
        "#f { display: flex; } #a { display: block; width: 50px; }");
    REQUIRE(find_by_soa_idx(p.root.get(), 3)->geometry.width == Approx(50.0f));
}

TEST_CASE("LTB flex basis: explicit px", "[ltb][layout][flex][basis]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'><p id='a'>ignored text</p></div>",
        "#f { display: flex; } #a { display: block; flex-basis: 100px; }");
    REQUIRE(find_by_soa_idx(p.root.get(), 3)->geometry.width == Approx(100.0f));
}

TEST_CASE("LTB flex basis: auto with explicit width", "[ltb][layout][flex][basis]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'><p id='a'>text</p></div>",
        "#f { display: flex; } #a { display: block; flex-basis: auto; width: 150px; }");
    REQUIRE(find_by_soa_idx(p.root.get(), 3)->geometry.width == Approx(150.0f));
}

TEST_CASE("LTB flex basis: percent of container", "[ltb][layout][flex][basis]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'><p id='a'></p></div>",
        "#f { display: flex; width: 400px; } #a { display: block; flex-basis: 25%; height: 10px; }");
    REQUIRE(find_by_soa_idx(p.root.get(), 3)->geometry.width == Approx(100.0f));
}

TEST_CASE("LTB flex basis: auto with content", "[ltb][layout][flex][basis]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'><p id='a'>hello</p></div>",
        "#f { display: flex; } #a { display: block; flex-basis: auto; }");
    REQUIRE(find_by_soa_idx(p.root.get(), 3)->geometry.width > 0.0f);
}

TEST_CASE("LTB flex intrinsic: block child with nested block", "[ltb][layout][flex][intrinsic]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div id='f'><div id='a'><p id='p1'>hello</p><p id='p2'>world</p></div></div>",
        "#f { display: flex; } #a, #p1, #p2 { display: block; }");
    auto* a = find_by_soa_idx(p.root.get(), 3);
    auto* p1 = find_by_soa_idx(p.root.get(), 4);
    auto* p2 = find_by_soa_idx(p.root.get(), 5);
    REQUIRE(a->geometry.width >= p1->geometry.width);
    REQUIRE(a->geometry.width >= p2->geometry.width);
    REQUIRE(a->geometry.width < p1->geometry.width + p2->geometry.width);
}

TEST_CASE("LTB flex intrinsic: intrinsic height from stacked blocks", "[ltb][layout][flex][intrinsic]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div id='f'><div id='a'><p style='height:20px'></p><p style='height:30px'></p></div></div>",
        "#f { display: flex; } #a { display: block; } #a p { display: block; margin: 0; }");
    REQUIRE(find_by_soa_idx(p.root.get(), 3)->geometry.height == Approx(50.0f));
}

TEST_CASE("LTB flex intrinsic: absolute child ignored", "[ltb][layout][flex][intrinsic]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div id='f'><p id='a'>text</p><span id='abs' style='position:absolute'></span></div>",
        "#f { display: flex; } #a { display: block; }");
    auto* f = find_by_soa_idx(p.root.get(), 2);
    auto* a = find_by_soa_idx(p.root.get(), 3);
    REQUIRE(a->geometry.width > 0.0f);
    REQUIRE(f->children.size() == 2);
    REQUIRE(f->children[0].get() == a);
}

// ============================================================
//  Этап 2: text-decoration + цвет ссылок
// ============================================================

namespace {

    inline const DisplayItem* find_text_item(const DisplayList& dl,
        const std::string& text) {
        for (const auto& it : dl.items) {
            if (it.type == DisplayItemType::Text && it.text == text)
                return &it;
        }
        return nullptr;
    }

} // namespace


TEST_CASE("LTB decoration: link has underline and blue color",
    "[ltb][decoration]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<a href='#'>link</a>",
        "",
        1024.0f,
        /*use_full_ua=*/true);

    REQUIRE(p.root != nullptr);

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    const DisplayItem* item = find_text_item(dl, "link");
    REQUIRE(item != nullptr);
    REQUIRE(item->underline == true);
    REQUIRE(item->line_through == false);
    REQUIRE(item->overline == false);
    REQUIRE(item->color == 0x0000EEFFu);
}

TEST_CASE("LTB decoration: inline style underline",
    "[ltb][decoration]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<span style='text-decoration: underline'>x</span>",
        "html, body, span { display: inline; }");

    REQUIRE(p.root != nullptr);

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    const DisplayItem* item = find_text_item(dl, "x");
    REQUIRE(item != nullptr);
    REQUIRE(item->underline == true);
    REQUIRE(item->line_through == false);
    REQUIRE(item->overline == false);
}

TEST_CASE("LTB decoration: default has no decorations",
    "[ltb][decoration]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<span>x</span>",
        "html, body, span { display: inline; }");

    REQUIRE(p.root != nullptr);

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    const DisplayItem* item = find_text_item(dl, "x");
    REQUIRE(item != nullptr);
    REQUIRE(item->underline == false);
    REQUIRE(item->line_through == false);
    REQUIRE(item->overline == false);
}

TEST_CASE("LTB decoration: underline + line-through bitmask",
    "[ltb][decoration]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<span style='text-decoration: underline line-through'>x</span>",
        "html, body, span { display: inline; }");

    REQUIRE(p.root != nullptr);

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    const DisplayItem* item = find_text_item(dl, "x");
    REQUIRE(item != nullptr);
    REQUIRE(item->underline == true);
    REQUIRE(item->line_through == true);
    REQUIRE(item->overline == false);
}

TEST_CASE("LTB decoration: not inherited from parent",
    "[ltb][decoration]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='text-decoration: underline'><span>x</span></div>",
        "html, body, div { display: block; } span { display: inline; }");

    REQUIRE(p.root != nullptr);

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    const DisplayItem* item = find_text_item(dl, "x");
    REQUIRE(item != nullptr);
    // text-decoration НЕ наследуется через каскад.
    REQUIRE(item->underline == false);
    REQUIRE(item->line_through == false);
    REQUIRE(item->overline == false);
}


// ============================================================
//  Этап 3: font-style: italic + letter-spacing
// ============================================================

TEST_CASE("LTB italic: <em> is italic",
    "[ltb][italic]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<em>x</em>",
        "",
        1024.0f,
        /*use_full_ua=*/true);

    REQUIRE(p.root != nullptr);
    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    const DisplayItem* item = find_text_item(dl, "x");
    REQUIRE(item != nullptr);
    REQUIRE(item->italic == true);
}

TEST_CASE("LTB italic: inline style font-style:italic",
    "[ltb][italic]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<span style='font-style: italic'>x</span>",
        "html, body, span { display: inline; }");

    REQUIRE(p.root != nullptr);
    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    const DisplayItem* item = find_text_item(dl, "x");
    REQUIRE(item != nullptr);
    REQUIRE(item->italic == true);
}

TEST_CASE("LTB italic: default is not italic",
    "[ltb][italic]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<span>x</span>",
        "html, body, span { display: inline; }");

    REQUIRE(p.root != nullptr);
    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    const DisplayItem* item = find_text_item(dl, "x");
    REQUIRE(item != nullptr);
    REQUIRE(item->italic == false);
}

TEST_CASE("LTB letter-spacing: adds 6px for 3 chars at 2px",
    "[ltb][letter-spacing]") {
    ArenaAllocator arena;
    auto p1 = run_pipeline(arena,
        "<span style='letter-spacing: 2px'>abc</span>",
        "");
    auto p2 = run_pipeline(arena,
        "<span style='letter-spacing: 0px'>abc</span>",
        "");

    // Проверяем через DisplayList: LayoutNode-форма дерева здесь
    // может быть Inline → Inline → Inline, first_text_deep туда
    // не заходит. DisplayList обходится плоско.
    DisplayList dl1 = DisplayListBuilder::build(p1.root.get(), p1.storage);
    DisplayList dl2 = DisplayListBuilder::build(p2.root.get(), p2.storage);

    const DisplayItem* it1 = find_text_item(dl1, "abc");
    const DisplayItem* it2 = find_text_item(dl2, "abc");
    REQUIRE(it1 != nullptr);
    REQUIRE(it2 != nullptr);
    REQUIRE(it1->width - it2->width == Approx(6.0f).epsilon(0.01));
}

TEST_CASE("LTB letter-spacing: DisplayItem carries value",
    "[ltb][letter-spacing]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<span style='letter-spacing: 3px'>x</span>",
        "html, body, span { display: inline; }");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    const DisplayItem* item = find_text_item(dl, "x");
    REQUIRE(item != nullptr);
    REQUIRE(item->letter_spacing == Approx(3.0f).epsilon(0.001));
}

TEST_CASE("LTB letter-spacing: inherited from parent",
    "[ltb][letter-spacing]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='letter-spacing: 3px'><span>x</span></div>",
        "html, body, div { display: block; } span { display: inline; }");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    const DisplayItem* item = find_text_item(dl, "x");
    REQUIRE(item != nullptr);
    REQUIRE(item->letter_spacing == Approx(3.0f).epsilon(0.001));
}