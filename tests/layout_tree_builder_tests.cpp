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
#include "../layout/headers/float_context.h"

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
    REQUIRE(h1->geometry.y == 48.0f);
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
    REQUIRE(body->geometry.y == 15.0f);
    REQUIRE(parent->geometry.y == 15.0f);
    REQUIRE(child->geometry.y == 15.0f);
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
    REQUIRE(inner->geometry.x == Approx(60.0f).epsilon(0.001));
    REQUIRE(inner->geometry.y == Approx(60.0f).epsilon(0.001));
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



// ============================================================
//  Этап 4: border-color + <hr>
// ============================================================

namespace {

    inline bool dl_has_rect(const DisplayList& dl, uint32_t color,
        float w, float h)
    {
        for (const auto& it : dl.items) {
            if (it.type != DisplayItemType::Rect) continue;
            if (it.color != color) continue;
            if (std::abs(it.width - w) > 0.01f) continue;
            if (std::abs(it.height - h) > 0.01f) continue;
            return true;
        }
        return false;
    }

} // namespace


TEST_CASE("LTB border-color: top border uses inline color",
    "[ltb][border-color]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='border-top-width: 2px;"
        "            border-top-color: red;"
        "            width: 100px; height: 50px'></div>",
        "html, body, div { display: block; margin: 0; padding: 0; }");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    // Верхняя граница: ширина = w (100), высота = 2, цвет = красный.
    REQUIRE(dl_has_rect(dl, 0xFF0000FFu, 100.0f, 2.0f));
}

TEST_CASE("LTB border-color: per-side colors",
    "[ltb][border-color]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='"
        "  border-top-width: 1px; border-top-color: red;"
        "  border-right-width: 1px; border-right-color: green;"
        "  border-bottom-width: 1px; border-bottom-color: blue;"
        "  border-left-width: 1px; border-left-color: yellow;"
        "  width: 50px; height: 50px;'></div>",
        "html, body, div { display: block; margin: 0; padding: 0; }");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);

    auto has_color = [&](uint32_t c) {
        for (const auto& it : dl.items)
            if (it.type == DisplayItemType::Rect && it.color == c) return true;
        return false;
        };
    REQUIRE(has_color(0xFF0000FFu));  // red
    REQUIRE(has_color(0x008000FFu));  // green
    REQUIRE(has_color(0x0000FFFFu));  // blue
    REQUIRE(has_color(0xFFFF00FFu));  // yellow
}

TEST_CASE("LTB border-color: shorthand sets all sides",
    "[ltb][border-color]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='"
        "  border-top-width: 1px; border-right-width: 1px;"
        "  border-bottom-width: 1px; border-left-width: 1px;"
        "  border-color: #ff0000;"
        "  width: 30px; height: 30px;'></div>",
        "html, body, div { display: block; margin: 0; padding: 0; }");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);

    int red_count = 0;
    for (const auto& it : dl.items)
        if (it.type == DisplayItemType::Rect && it.color == 0xFF0000FFu)
            ++red_count;
    REQUIRE(red_count == 4);  // 4 стороны
}

TEST_CASE("LTB border-color: default is black",
    "[ltb][border-color]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='border-top-width: 1px; width: 20px; height: 20px'></div>",
        "html, body, div { display: block; margin: 0; padding: 0; }");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    REQUIRE(dl_has_rect(dl, 0x000000FFu, 20.0f, 1.0f));
}

TEST_CASE("LTB border-color: transparent emits no rect",
    "[ltb][border-color]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='border-top-width: 5px;"
        "            border-top-color: transparent;"
        "            width: 10px; height: 10px'></div>",
        "html, body, div { display: block; margin: 0; padding: 0; }");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);

    // Ни одного rect высотой 5 быть не должно.
    bool found = false;
    for (const auto& it : dl.items)
        if (it.type == DisplayItemType::Rect
            && std::abs(it.height - 5.0f) < 0.01f) {
            found = true; break;
        }
    REQUIRE_FALSE(found);
}

TEST_CASE("LTB integration: <hr> border is gray",
    "[ltb][integration][hr]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<p>Above</p><hr><p>Below</p>",
        "",
        1024.0f,
        /*use_full_ua=*/true);

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);

    // <hr> → border-top-width: 1px; border-color: #808080 (UA CSS).
    // Ищем серый rect высотой 1. Ширину не проверяем — она зависит
    // от того, как body распределил ширину.
    bool found_gray = false;
    for (const auto& it : dl.items) {
        if (it.type == DisplayItemType::Rect
            && it.color == 0x808080FFu
            && std::abs(it.height - 1.0f) < 0.01f) {
            found_gray = true;
            break;
        }
    }
    REQUIRE(found_gray);
}



// ============================================================
//  Этап 5: visibility: hidden + opacity
// ============================================================

TEST_CASE("LTB visibility: hidden skips own paint",
    "[ltb][visibility]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='visibility: hidden; background-color: red;"
        "            width: 50px; height: 50px'></div>",
        "html, body, div { display: block; margin: 0; padding: 0; }");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);

    // Ни одного красного rect быть не должно.
    for (const auto& it : dl.items) {
        if (it.type == DisplayItemType::Rect)
            REQUIRE(it.color != 0xFF0000FFu);
    }
}

TEST_CASE("LTB visibility: hidden still lays out children",
    "[ltb][visibility]") {
    // visibility: hidden не убирает из layout (в отличие от display: none).
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='visibility: hidden; height: 40px'></div>",
        "html, body, div { display: block; margin: 0; padding: 0; }");

    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    REQUIRE(body != nullptr);
    LayoutNode* div = child_of_type(body, BoxType::Block);
    REQUIRE(div != nullptr);
    REQUIRE(div->geometry.height == 40.0f);
}

TEST_CASE("LTB visibility: visible child inside hidden parent",
    "[ltb][visibility]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='visibility: hidden; background-color: red;"
        "            width: 100px; height: 100px'>"
        "  <span style='visibility: visible; background-color: blue;"
        "               display: inline-block; width: 10px; height: 10px'></span>"
        "</div>",
        "html, body, div { display: block; margin: 0; padding: 0; }");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);

    bool has_red = false, has_blue = false;
    for (const auto& it : dl.items) {
        if (it.type != DisplayItemType::Rect) continue;
        if (it.color == 0xFF0000FFu) has_red = true;
        if (it.color == 0x0000FFFFu) has_blue = true;
    }
    REQUIRE_FALSE(has_red);
    REQUIRE(has_blue);
}

TEST_CASE("LTB opacity: carried to DisplayItem",
    "[ltb][opacity]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<span style='opacity: 0.5'>x</span>",
        "html, body, span { display: inline; }");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    const DisplayItem* item = find_text_item(dl, "x");
    REQUIRE(item != nullptr);
    REQUIRE(item->opacity == Approx(0.5f).epsilon(0.01));
}

TEST_CASE("LTB opacity: zero carried to DisplayItem",
    "[ltb][opacity]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<span style='opacity: 0'>x</span>",
        "html, body, span { display: inline; }");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    const DisplayItem* item = find_text_item(dl, "x");
    REQUIRE(item != nullptr);
    REQUIRE(item->opacity == Approx(0.0f).epsilon(0.01));
}

TEST_CASE("LTB opacity: default is 1.0",
    "[ltb][opacity]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<span>x</span>",
        "html, body, span { display: inline; }");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    const DisplayItem* item = find_text_item(dl, "x");
    REQUIRE(item != nullptr);
    REQUIRE(item->opacity == Approx(1.0f).epsilon(0.01));
}

TEST_CASE("LTB opacity: not inherited",
    "[ltb][opacity]") {
    // opacity не наследуется: у span'а свой opacity_q = 255 (default),
    // даже если у div'а 0.5.
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='opacity: 0.5'><span>x</span></div>",
        "html, body, div { display: block; } span { display: inline; }");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    const DisplayItem* item = find_text_item(dl, "x");
    REQUIRE(item != nullptr);
    REQUIRE(item->opacity == Approx(1.0f).epsilon(0.01));
}



// ============================================================
//  Этап 6: <img> — replaced element
// ============================================================

namespace {

    // Dummy ImageMetrics: "test.png" всегда 100×50, остальное — 0×0.
    inline LayoutTreeBuilder::ImageMetrics dummy_img_metrics() {
        LayoutTreeBuilder::ImageMetrics m;
        m.intrinsic_size = [](const std::string& src)
            -> std::pair<float, float>
            {
                if (src == "test.png") return { 100.0f, 50.0f };
                return { 0.0f, 0.0f };
            };
        return m;
    }

    inline Pipeline run_pipeline_img(ArenaAllocator& arena,
        const std::string& inner_html,
        const std::string& css,
        const LayoutTreeBuilder::ImageMetrics& im,
        float viewport = 1024.0f,
        bool use_full_ua = false)
    {
        std::string html = "<html><body>" + inner_html + "</body></html>";
        std::string full_css;
        if (use_full_ua) full_css = std::string(ua_css::kSource) + " " + css;
        else full_css = "html, body { display: block; } " + css;

        HTMLParser hp(arena);
        DOMNode* dom = hp.parse(html);
        DOMNode* root_elem = first_element(dom);
        REQUIRE(root_elem != nullptr);

        CSSParser cp(arena);
        StyleSheet* sheet = cp.parse(full_css);
        REQUIRE(sheet != nullptr);

        Pipeline p;
        p.storage = StyleTreeBuilder::build(root_elem, *sheet);
        p.root = LayoutTreeBuilder::build(p.storage, im);
        if (p.root) {
            LayoutTreeBuilder::Viewport vp{ viewport, 768.0f };
            LayoutTreeBuilder::compute_layout(p.root.get(), vp, p.storage);
        }
        return p;
    }

    inline LayoutNode* find_image(LayoutNode* node) {
        if (!node) return nullptr;
        if (node->type == BoxType::Image) return node;
        for (auto& c : node->children)
            if (auto* r = find_image(c.get())) return r;
        return nullptr;
    }

} // namespace


TEST_CASE("LTB img: element becomes BoxType::Image", "[ltb][img]") {
    ArenaAllocator arena;
    auto p = run_pipeline_img(arena,
        "<img src='test.png'>",
        "html, body { display: block; }",
        dummy_img_metrics());
    REQUIRE(p.root != nullptr);
    LayoutNode* img = find_image(p.root.get());
    REQUIRE(img != nullptr);
    REQUIRE(img->image_src == "test.png");
}

TEST_CASE("LTB img: intrinsic size used when no CSS sizes", "[ltb][img]") {
    ArenaAllocator arena;
    auto p = run_pipeline_img(arena,
        "<img src='test.png'>",
        "html, body { display: block; margin: 0; padding: 0; }",
        dummy_img_metrics());
    LayoutNode* img = find_image(p.root.get());
    REQUIRE(img != nullptr);
    REQUIRE(img->geometry.width == Approx(100.0f).epsilon(0.01));
    REQUIRE(img->geometry.height == Approx(50.0f).epsilon(0.01));
}

TEST_CASE("LTB img: explicit width scales height proportionally", "[ltb][img]") {
    ArenaAllocator arena;
    auto p = run_pipeline_img(arena,
        "<img src='test.png' style='width: 200px'>",
        "html, body { display: block; margin: 0; padding: 0; }",
        dummy_img_metrics());
    LayoutNode* img = find_image(p.root.get());
    REQUIRE(img != nullptr);
    REQUIRE(img->geometry.width == Approx(200.0f).epsilon(0.01));
    REQUIRE(img->geometry.height == Approx(100.0f).epsilon(0.01));
}

TEST_CASE("LTB img: explicit height scales width proportionally", "[ltb][img]") {
    ArenaAllocator arena;
    auto p = run_pipeline_img(arena,
        "<img src='test.png' style='height: 100px'>",
        "html, body { display: block; margin: 0; padding: 0; }",
        dummy_img_metrics());
    LayoutNode* img = find_image(p.root.get());
    REQUIRE(img != nullptr);
    REQUIRE(img->geometry.width == Approx(200.0f).epsilon(0.01));
    REQUIRE(img->geometry.height == Approx(100.0f).epsilon(0.01));
}

TEST_CASE("LTB img: both sizes explicit — no scaling", "[ltb][img]") {
    ArenaAllocator arena;
    auto p = run_pipeline_img(arena,
        "<img src='test.png' style='width: 30px; height: 40px'>",
        "html, body { display: block; margin: 0; padding: 0; }",
        dummy_img_metrics());
    LayoutNode* img = find_image(p.root.get());
    REQUIRE(img != nullptr);
    REQUIRE(img->geometry.width == Approx(30.0f).epsilon(0.01));
    REQUIRE(img->geometry.height == Approx(40.0f).epsilon(0.01));
}

TEST_CASE("LTB img: missing file — 0×0 if no CSS sizes", "[ltb][img]") {
    ArenaAllocator arena;
    auto p = run_pipeline_img(arena,
        "<img src='missing.png'>",
        "html, body { display: block; margin: 0; padding: 0; }",
        dummy_img_metrics());
    LayoutNode* img = find_image(p.root.get());
    REQUIRE(img != nullptr);
    REQUIRE(img->geometry.width == Approx(0.0f));
    REQUIRE(img->geometry.height == Approx(0.0f));
}

TEST_CASE("LTB img: display list carries Image item", "[ltb][img]") {
    ArenaAllocator arena;
    auto p = run_pipeline_img(arena,
        "<img src='test.png'>",
        "html, body { display: block; margin: 0; padding: 0; }",
        dummy_img_metrics());
    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    bool has_image = false;
    for (const auto& it : dl.items) {
        if (it.type == DisplayItemType::Image
            && it.image_path == "test.png") {
            has_image = true; break;
        }
    }
    REQUIRE(has_image);
}


TEST_CASE("LTB img: max-width caps large image",
    "[ltb][img][max]") {
    ArenaAllocator arena;
    auto p = run_pipeline_img(arena,
        "<img src='test.png' style='max-width: 50px'>",
        "html, body { display: block; margin: 0; padding: 0; }",
        dummy_img_metrics());

    LayoutNode* img = find_image(p.root.get());
    REQUIRE(img != nullptr);
    // 100×50 intrinsic, max-width: 50 → 50×25 (пропорции сохранены).
    REQUIRE(img->geometry.width == Approx(50.0f).epsilon(0.01));
    REQUIRE(img->geometry.height == Approx(25.0f).epsilon(0.01));
}

TEST_CASE("LTB img: max-height caps tall image",
    "[ltb][img][max]") {
    ArenaAllocator arena;
    auto p = run_pipeline_img(arena,
        "<img src='test.png' style='max-height: 20px'>",
        "html, body { display: block; margin: 0; padding: 0; }",
        dummy_img_metrics());

    LayoutNode* img = find_image(p.root.get());
    REQUIRE(img != nullptr);
    // 100×50 intrinsic, max-height: 20 → 40×20.
    REQUIRE(img->geometry.width == Approx(40.0f).epsilon(0.01));
    REQUIRE(img->geometry.height == Approx(20.0f).epsilon(0.01));
}

TEST_CASE("LTB img: max-width does not upscale small image",
    "[ltb][img][max]") {
    ArenaAllocator arena;
    auto p = run_pipeline_img(arena,
        "<img src='test.png' style='max-width: 500px'>",
        "html, body { display: block; margin: 0; padding: 0; }",
        dummy_img_metrics());

    LayoutNode* img = find_image(p.root.get());
    REQUIRE(img != nullptr);
    // max-width больше intrinsic — размер остаётся intrinsic.
    REQUIRE(img->geometry.width == Approx(100.0f).epsilon(0.01));
    REQUIRE(img->geometry.height == Approx(50.0f).epsilon(0.01));
}

TEST_CASE("LTB img: explicit width wins over max-width",
    "[ltb][img][max]") {
    // Если width <= max-width, обе применяются нормально.
    // Если width > max-width, срабатывает max-width.
    ArenaAllocator arena;
    auto p = run_pipeline_img(arena,
        "<img src='test.png' style='width: 200px; max-width: 50px'>",
        "html, body { display: block; margin: 0; padding: 0; }",
        dummy_img_metrics());

    LayoutNode* img = find_image(p.root.get());
    REQUIRE(img != nullptr);
    // width: 200 → max-width: 50 отсекает до 50×25.
    REQUIRE(img->geometry.width == Approx(50.0f).epsilon(0.01));
    REQUIRE(img->geometry.height == Approx(25.0f).epsilon(0.01));
}

TEST_CASE("LTB img: UA max-width: 100% shrinks to parent",
    "[ltb][img][max][ua]") {
    // В UA CSS добавлено img { max-width: 100%; }.
    // Картинка 100×50 при viewport 60 → ужимается до 60×30.
    ArenaAllocator arena;
    auto p = run_pipeline_img(arena,
        "<img src='test.png'>",
        "html, body { display: block; margin: 0; padding: 0; }",
        dummy_img_metrics(),
        /*viewport=*/60.0f,
        /*use_full_ua=*/true);

    LayoutNode* img = find_image(p.root.get());
    REQUIRE(img != nullptr);
    REQUIRE(img->geometry.width == Approx(60.0f).epsilon(0.01));
    REQUIRE(img->geometry.height == Approx(30.0f).epsilon(0.01));
}

TEST_CASE("LTB img: author max-width: none overrides UA",
    "[ltb][img][max][ua]") {
    // Авторский max-width: none должен перекрыть UA-правило.
    // Парсер: "none" → Length{0, Unit::None}, is_none() == true.
    ArenaAllocator arena;
    auto p = run_pipeline_img(arena,
        "<img src='test.png' style='max-width: none'>",
        "html, body { display: block; margin: 0; padding: 0; }",
        dummy_img_metrics(),
        /*viewport=*/60.0f,
        /*use_full_ua=*/true);

    LayoutNode* img = find_image(p.root.get());
    REQUIRE(img != nullptr);
    // Без UA-ограничения — intrinsic 100×50, хотя parent всего 60.
    REQUIRE(img->geometry.width == Approx(100.0f).epsilon(0.01));
    REQUIRE(img->geometry.height == Approx(50.0f).epsilon(0.01));
}



// ============================================================
//  Этап 7: text-align
// ============================================================

TEST_CASE("LTB text-align: left is default (no shift)",
    "[ltb][text-align]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='width: 100px'>hi</div>",
        "html, body, div { display: block; margin: 0; padding: 0; }");
    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    const DisplayItem* item = find_text_item(dl, "hi");
    REQUIRE(item != nullptr);
    REQUIRE(item->x == Approx(0.0f).epsilon(0.5));
}

TEST_CASE("LTB text-align: center shifts to middle",
    "[ltb][text-align]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='width: 100px; text-align: center'>hi</div>",
        "html, body, div { display: block; margin: 0; padding: 0; }");
    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    const DisplayItem* item = find_text_item(dl, "hi");
    REQUIRE(item != nullptr);
    // "hi" = 2 * 16 * 0.55 = 17.6. offset = (100 - 17.6) / 2 = 41.2.
    REQUIRE(item->x == Approx(41.2f).epsilon(0.5));
}

TEST_CASE("LTB text-align: right shifts to end",
    "[ltb][text-align]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='width: 100px; text-align: right'>hi</div>",
        "html, body, div { display: block; margin: 0; padding: 0; }");
    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    const DisplayItem* item = find_text_item(dl, "hi");
    REQUIRE(item != nullptr);
    // offset = 100 - 17.6 = 82.4.
    REQUIRE(item->x == Approx(82.4f).epsilon(0.5));
}

TEST_CASE("LTB text-align: inherited from parent",
    "[ltb][text-align]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='width: 100px; text-align: center'><p>hi</p></div>",
        "html, body, div, p { display: block; margin: 0; padding: 0; }");
    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    const DisplayItem* item = find_text_item(dl, "hi");
    REQUIRE(item != nullptr);
    REQUIRE(item->x == Approx(41.2f).epsilon(0.5));
}

TEST_CASE("LTB text-align: two lines centered",
    "[ltb][text-align]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='width: 100px; text-align: center'>aa<br>bb</div>",
        "html, body, div { display: block; margin: 0; padding: 0; }");
    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    const DisplayItem* it_aa = find_text_item(dl, "aa");
    const DisplayItem* it_bb = find_text_item(dl, "bb");
    REQUIRE(it_aa != nullptr);
    REQUIRE(it_bb != nullptr);
    REQUIRE(it_aa->x == Approx(41.2f).epsilon(0.5));
    REQUIRE(it_bb->x == Approx(41.2f).epsilon(0.5));
}

TEST_CASE("LTB text-align: justify acts as left on last line",
    "[ltb][text-align]") {
    // Однострочный текст — единственная строка является последней,
    // поэтому justify = left.
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='width: 200px; text-align: justify'>hi</div>",
        "html, body, div { display: block; margin: 0; padding: 0; }");
    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    const DisplayItem* item = find_text_item(dl, "hi");
    REQUIRE(item != nullptr);
    REQUIRE(item->x == Approx(0.0f).epsilon(0.5));
}


TEST_CASE("LTB baseline: image and text sit on same baseline",
    "[ltb][baseline]") {
    // Текст "x" (16px) и картинка (40px). По baseline они должны
    // стоять на одной линии: baseline совпадает.
    ArenaAllocator arena;
    auto p = run_pipeline_img(arena,
        "<p>x<img src='test.png' style='width: 20px; height: 40px'></p>",
        "html, body, p { display: block; margin: 0; padding: 0; }",
        dummy_img_metrics());   // у test.png intrinsic 100×50, но явные
    // width/height дают 20×40.

    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* para = child_of_type(body, BoxType::Block);
    LayoutNode* anon = child_of_type(para, BoxType::AnonymousBlock);
    REQUIRE(anon != nullptr);

    LayoutNode* txt = nullptr;
    LayoutNode* img = nullptr;
    for (auto& c : anon->children) {
        if (c->type == BoxType::Text && c->text_content == "x") txt = c.get();
        if (c->type == BoxType::Image) img = c.get();
    }
    REQUIRE(txt != nullptr);
    REQUIRE(img != nullptr);

    // baseline_ascent + ascent-ребёнка = y_baseline для обоих.
    const float text_baseline_y =
        txt->geometry.y + txt->geometry.baseline_ascent;
    const float img_baseline_y =
        img->geometry.y + img->geometry.baseline_ascent;
    REQUIRE(text_baseline_y == Approx(img_baseline_y).epsilon(0.01));
}

TEST_CASE("LTB baseline: line height is max(ascent) + max(descent)",
    "[ltb][baseline]") {
    // Только text 16px. baseline_ascent ≈ 0.8 * 16 + leading,
    // height = ascent + descent, как и раньше.
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div>hi</div>",
        "html, body, div { display: block; font-size: 16px; margin: 0; padding: 0; }",
        500.0f);
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* div = child_of_type(body, BoxType::Block);
    LayoutNode* anon = child_of_type(div, BoxType::AnonymousBlock);
    REQUIRE(anon != nullptr);

    LayoutNode* txt = first_text_deep(anon);
    REQUIRE(txt != nullptr);

    // baseline_ascent + baseline_descent = высота текста (19.2).
    REQUIRE(txt->geometry.baseline_ascent
        + txt->geometry.baseline_descent == Approx(19.2f).epsilon(0.01));

    // Внутри бокса текст стоит на baseline, а не на верхней грани.
    // baseline_ascent ≈ 14.4 (leading 1.6 + ascent 12.8).
    REQUIRE(txt->geometry.baseline_ascent == Approx(14.4f).epsilon(0.01));
}


// ============================================================
//  Этап 8: background-image: url(...)
// ============================================================

TEST_CASE("LTB background-image: url without quotes",
    "[ltb][background-image]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div class='a'></div>",
        "html, body, div { display: block; margin: 0; padding: 0; } "
        ".a { background-image: url(test.png); width: 50px; height: 30px; }");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);

    bool found = false;
    for (const auto& it : dl.items) {
        if (it.type == DisplayItemType::Image
            && it.image_path == "test.png") {
            found = true;
            // Растянуто на весь border-box.
            REQUIRE(it.width == Approx(50.0f).epsilon(0.01));
            REQUIRE(it.height == Approx(30.0f).epsilon(0.01));
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("LTB background-image: url with double quotes",
    "[ltb][background-image]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div class='a'></div>",
        "html, body, div { display: block; margin: 0; padding: 0; } "
        ".a { background-image: url(\"test.png\"); width: 10px; height: 10px; }");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    bool found = false;
    for (const auto& it : dl.items) {
        if (it.type == DisplayItemType::Image
            && it.image_path == "test.png") {
            found = true; break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("LTB background-image: url with single quotes",
    "[ltb][background-image]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div class='a'></div>",
        "html, body, div { display: block; margin: 0; padding: 0; } "
        ".a { background-image: url('test.png'); width: 10px; height: 10px; }");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    bool found = false;
    for (const auto& it : dl.items) {
        if (it.type == DisplayItemType::Image
            && it.image_path == "test.png") {
            found = true; break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("LTB background-image: none emits no image",
    "[ltb][background-image]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div class='a'></div>",
        "html, body, div { display: block; margin: 0; padding: 0; } "
        ".a { background-image: none; width: 20px; height: 20px; }");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    for (const auto& it : dl.items) {
        REQUIRE(it.type != DisplayItemType::Image);
    }
}

TEST_CASE("LTB background-image: not inherited",
    "[ltb][background-image]") {
    // Родитель с background-image, ребёнок без него — у ребёнка
    // не должно быть своего Image-элемента.
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div class='parent'><span class='child'>x</span></div>",
        "html, body, div { display: block; margin: 0; padding: 0; } "
        ".parent { background-image: url(test.png); width: 100px; height: 50px; } ");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);

    int img_count = 0;
    for (const auto& it : dl.items)
        if (it.type == DisplayItemType::Image) ++img_count;

    // Только один — от родителя. У span'а нет.
    REQUIRE(img_count == 1);
}

TEST_CASE("LTB background-image: painted above background-color",
    "[ltb][background-image]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div class='a'></div>",
        "html, body, div { display: block; margin: 0; padding: 0; } "
        ".a { background-color: red; background-image: url(test.png);"
        "      width: 20px; height: 20px; }");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);

    // Ищем красный Rect и Image — Image должен идти ПОСЛЕ Rect
    // (в display list более поздние items рисуются поверх).
    int red_idx = -1, img_idx = -1;
    for (size_t i = 0; i < dl.items.size(); ++i) {
        if (dl.items[i].type == DisplayItemType::Rect
            && dl.items[i].color == 0xFF0000FFu) red_idx = (int)i;
        if (dl.items[i].type == DisplayItemType::Image) img_idx = (int)i;
    }
    REQUIRE(red_idx >= 0);
    REQUIRE(img_idx >= 0);
    REQUIRE(img_idx > red_idx);
}


// ============================================================
//  Этап 9: vertical-align
// ============================================================

TEST_CASE("LTB vertical-align: baseline is default",
    "[ltb][vertical-align]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<p>a<span style='font-size: 32px'>B</span>c</p>",
        "html, body, p { display: block; margin: 0; padding: 0; }");
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* para = child_of_type(body, BoxType::Block);
    LayoutNode* anon = child_of_type(para, BoxType::AnonymousBlock);
    REQUIRE(anon != nullptr);

    LayoutNode* a = nullptr;
    LayoutNode* B = nullptr;
    for (auto& c : anon->children) {
        if (c->type == BoxType::Text && c->text_content == "a") a = c.get();
    }
    // B лежит внутри <span> → anon → span → anon → text
    LayoutNode* span = child_of_type(anon, BoxType::Inline);
    REQUIRE(span != nullptr);

    LayoutNode* text_B = first_text_deep(span);
    REQUIRE(text_B != nullptr);

    // baseline-выравнивание: baseline(B) == baseline(a) == baseline(c)
    const float a_base = a->geometry.y + a->geometry.baseline_ascent;
    const float B_base =
        span->geometry.y + span->geometry.baseline_ascent;
    REQUIRE(a_base == Approx(B_base).epsilon(0.5));
}

TEST_CASE("LTB vertical-align: middle centers image on text line",
    "[ltb][vertical-align]") {
    // Картинка 40px с vertical-align:middle должна "висеть" на середине
    // строки, а не сидеть на baseline.
    ArenaAllocator arena;
    auto p = run_pipeline_img(arena,
        "<p>text<img src='test.png' "
        "style='width: 20px; height: 40px; vertical-align: middle'></p>",
        "html, body, p { display: block; margin: 0; padding: 0; }",
        dummy_img_metrics());

    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* para = child_of_type(body, BoxType::Block);
    LayoutNode* anon = child_of_type(para, BoxType::AnonymousBlock);
    REQUIRE(anon != nullptr);

    LayoutNode* img = nullptr;
    for (auto& c : anon->children)
        if (c->type == BoxType::Image) { img = c.get(); break; }
    REQUIRE(img != nullptr);

    // При baseline низ картинки был бы на baseline. При middle картинка
    // сдвинута вверх. Проверяем, что верх картинки выше верхней грани
    // строки, то есть картинка реально ушла вверх от baseline.
    REQUIRE(img->geometry.y > 0.0f);
    const float img_center = img->geometry.y + img->geometry.height * 0.5f;
    REQUIRE(img_center == Approx(36.0f).epsilon(1.0));

}

TEST_CASE("LTB vertical-align: top aligns to line top",
    "[ltb][vertical-align]") {
    ArenaAllocator arena;
    auto p = run_pipeline_img(arena,
        "<p>text<img src='test.png' "
        "style='width: 20px; height: 40px; vertical-align: top'></p>",
        "html, body, p { display: block; margin: 0; padding: 0; }",
        dummy_img_metrics());

    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* para = child_of_type(body, BoxType::Block);
    LayoutNode* anon = child_of_type(para, BoxType::AnonymousBlock);
    LayoutNode* img = nullptr;
    for (auto& c : anon->children)
        if (c->type == BoxType::Image) { img = c.get(); break; }
    REQUIRE(img != nullptr);

    // top: верх картинки совпадает с верхом строки (y внутри IFC == 0).
    REQUIRE(img->geometry.y == Approx(0.0f).epsilon(0.5));
}

TEST_CASE("LTB vertical-align: bottom aligns to line bottom",
    "[ltb][vertical-align]") {
    // Картинка 20px с vertical-align: bottom. Строка = max(child, strut).
    // Низ картинки = низ строки.
    ArenaAllocator arena;
    auto p = run_pipeline_img(arena,
        "<p>x<img src='test.png' "
        "style='width: 20px; height: 20px; vertical-align: bottom'></p>",
        "html, body, p { display: block; margin: 0; padding: 0; }",
        dummy_img_metrics());

    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* para = child_of_type(body, BoxType::Block);
    LayoutNode* anon = child_of_type(para, BoxType::AnonymousBlock);
    REQUIRE(anon != nullptr);
    LayoutNode* img = nullptr;
    for (auto& c : anon->children)
        if (c->type == BoxType::Image) { img = c.get(); break; }
    REQUIRE(img != nullptr);

    // Низ картинки = height IFC (последняя строка).
    const float img_bottom = img->geometry.y + img->geometry.height;
    // Находим высоту IFC без margin/padding: anon.height целиком.
    const float line_bottom = anon->geometry.height;
    REQUIRE(img_bottom == Approx(line_bottom).epsilon(0.5));
}

TEST_CASE("LTB vertical-align: not inherited",
    "[ltb][vertical-align]") {
    // У родителя middle, у ребёнка без явного свойства — baseline.
    // Проверяем через Storage: vertical_aligns у ребёнка = Baseline.
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='vertical-align: middle'>"
        "  <span>x</span>"
        "</div>",
        "html, body, div { display: block; margin: 0; padding: 0; } "
        "span { display: inline; }");

    REQUIRE(p.storage.size() >= 3);
    // soa indices: 0=html, 1=body, 2=div, 3=span
    const uint32_t span_idx = 3;
    REQUIRE(p.storage.vertical_aligns[span_idx]
        == style::VerticalAlign::Baseline);
}

TEST_CASE("LTB vertical-align: property parsed from inline style",
    "[ltb][vertical-align]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<span style='vertical-align: middle'>x</span>",
        "html, body, span { display: inline; }");
    REQUIRE(p.storage.size() >= 3);
    const uint32_t span_idx = 2;
    REQUIRE(p.storage.vertical_aligns[span_idx]
        == style::VerticalAlign::Middle);
}




// ============================================================
//  Этап A: float — blockification + размещение
// ============================================================

TEST_CASE("LTB float: inline element becomes Block",
    "[ltb][float]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<span id='s' style='float: left; width: 100px'>x</span>",
        "html, body, span { display: inline; }");

    LayoutNode* s = find_by_soa_idx(p.root.get(), 2);
    REQUIRE(s != nullptr);
    REQUIRE(s->type == BoxType::Block);
}

TEST_CASE("LTB float: single left float at (0,0)",
    "[ltb][float]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div class='bfc'>"
        "  <div class='f'></div>"
        "</div>",
        "html, body { display: block; margin: 0; padding: 0; } "
        ".bfc { display: block; width: 400px; } "
        ".f   { float: left; width: 100px; height: 50px; }");

    LayoutNode* bfc = find_by_soa_idx(p.root.get(), 2);
    LayoutNode* f = find_by_soa_idx(p.root.get(), 3);
    REQUIRE(bfc != nullptr);
    REQUIRE(f != nullptr);

    // float слева вверху
    REQUIRE(f->geometry.x == Approx(0.0f).epsilon(0.01));
    REQUIRE(f->geometry.y == Approx(0.0f).epsilon(0.01));
    REQUIRE(f->geometry.width == Approx(100.0f).epsilon(0.01));
    REQUIRE(f->geometry.height == Approx(50.0f).epsilon(0.01));

    // Родитель НЕ растягивается от float'а.
    REQUIRE(bfc->geometry.height == Approx(0.0f).epsilon(0.01));
}

TEST_CASE("LTB float: two left floats side by side",
    "[ltb][float]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div class='bfc'>"
        "  <div class='a'></div>"
        "  <div class='b'></div>"
        "</div>",
        "html, body { display: block; margin: 0; padding: 0; } "
        ".bfc { display: block; width: 400px; } "
        ".a { float: left; width: 100px; height: 50px; } "
        ".b { float: left; width: 100px; height: 50px; }");

    LayoutNode* a = find_by_soa_idx(p.root.get(), 3);
    LayoutNode* b = find_by_soa_idx(p.root.get(), 4);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    REQUIRE(a->geometry.x == Approx(0.0f).epsilon(0.01));
    REQUIRE(a->geometry.y == Approx(0.0f).epsilon(0.01));
    REQUIRE(b->geometry.x == Approx(100.0f).epsilon(0.01));
    REQUIRE(b->geometry.y == Approx(0.0f).epsilon(0.01));
}

TEST_CASE("LTB float: too-wide second float wraps to next row",
    "[ltb][float]") {
    // Контейнер 200, два float'а по 150 → второй переносится вниз.
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div class='bfc'>"
        "  <div class='a'></div>"
        "  <div class='b'></div>"
        "</div>",
        "html, body { display: block; margin: 0; padding: 0; } "
        ".bfc { display: block; width: 200px; } "
        ".a { float: left; width: 150px; height: 50px; } "
        ".b { float: left; width: 150px; height: 50px; }");

    LayoutNode* a = find_by_soa_idx(p.root.get(), 3);
    LayoutNode* b = find_by_soa_idx(p.root.get(), 4);
    REQUIRE(a->geometry.x == Approx(0.0f).epsilon(0.01));
    REQUIRE(a->geometry.y == Approx(0.0f).epsilon(0.01));
    REQUIRE(b->geometry.x == Approx(0.0f).epsilon(0.01));
    REQUIRE(b->geometry.y == Approx(50.0f).epsilon(0.01));
}

TEST_CASE("LTB float: right float at right edge",
    "[ltb][float]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div class='bfc'>"
        "  <div class='f'></div>"
        "</div>",
        "html, body { display: block; margin: 0; padding: 0; } "
        ".bfc { display: block; width: 400px; } "
        ".f   { float: right; width: 100px; height: 50px; }");

    LayoutNode* f = find_by_soa_idx(p.root.get(), 3);
    REQUIRE(f->geometry.x == Approx(300.0f).epsilon(0.01));
    REQUIRE(f->geometry.y == Approx(0.0f).epsilon(0.01));
}

TEST_CASE("LTB float: left and right floats coexist on one row",
    "[ltb][float]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div class='bfc'>"
        "  <div class='l'></div>"
        "  <div class='r'></div>"
        "</div>",
        "html, body { display: block; margin: 0; padding: 0; } "
        ".bfc { display: block; width: 400px; } "
        ".l { float: left;  width: 100px; height: 50px; } "
        ".r { float: right; width: 100px; height: 50px; }");

    LayoutNode* l = find_by_soa_idx(p.root.get(), 3);
    LayoutNode* r = find_by_soa_idx(p.root.get(), 4);
    REQUIRE(l->geometry.x == Approx(0.0f).epsilon(0.01));
    REQUIRE(r->geometry.x == Approx(300.0f).epsilon(0.01));
    REQUIRE(l->geometry.y == Approx(0.0f).epsilon(0.01));
    REQUIRE(r->geometry.y == Approx(0.0f).epsilon(0.01));
}

TEST_CASE("LTB float: float does not push following block down",
    "[ltb][float]") {
    // Пока без обтекания: обычный блок идёт с y_cursor = 0,
    // накладываясь на float. Подэтап B починит.
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div class='bfc'>"
        "  <div class='f'></div>"
        "  <div class='plain'></div>"
        "</div>",
        "html, body { display: block; margin: 0; padding: 0; } "
        ".bfc   { display: block; width: 400px; } "
        ".f     { float: left; width: 100px; height: 50px; } "
        ".plain { display: block; width: 100px; height: 20px; }");

    LayoutNode* f = find_by_soa_idx(p.root.get(), 3);
    LayoutNode* plain = find_by_soa_idx(p.root.get(), 4);
    REQUIRE(f != nullptr);
    REQUIRE(plain != nullptr);

    REQUIRE(f->geometry.y == Approx(0.0f).epsilon(0.01));
    // Обычный блок остаётся на y_cursor = 0 (float его не сдвинул).
    REQUIRE(plain->geometry.y == Approx(0.0f).epsilon(0.01));
}

// ============================================================
//  clear
// ============================================================

TEST_CASE("LTB clear: clear-left pushes block below float",
    "[ltb][float][clear]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div class='bfc'>"
        "  <div class='f'></div>"
        "  <div class='p'></div>"
        "</div>",
        "html, body { display: block; margin: 0; padding: 0; } "
        ".bfc { display: block; width: 400px; } "
        ".f   { float: left; width: 100px; height: 50px; } "
        ".p   { display: block; clear: left; width: 100px; height: 20px; }");

    LayoutNode* f = find_by_soa_idx(p.root.get(), 3);
    LayoutNode* pl = find_by_soa_idx(p.root.get(), 4);
    REQUIRE(f != nullptr);
    REQUIRE(pl != nullptr);

    REQUIRE(f->geometry.y == Approx(0.0f).epsilon(0.01));
    // clear-left поднимает до y >= 50 (низ float'а).
    REQUIRE(pl->geometry.y == Approx(50.0f).epsilon(0.01));
}

TEST_CASE("LTB clear: clear-both pushes below both sides",
    "[ltb][float][clear]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div class='bfc'>"
        "  <div class='l'></div>"
        "  <div class='r'></div>"
        "  <div class='p'></div>"
        "</div>",
        "html, body { display: block; margin: 0; padding: 0; } "
        ".bfc { display: block; width: 400px; } "
        ".l   { float: left;  width: 100px; height: 60px; } "
        ".r   { float: right; width: 100px; height: 80px; } "
        ".p   { display: block; clear: both;  width: 100px; height: 20px; }");

    LayoutNode* pl = find_by_soa_idx(p.root.get(), 5);
    REQUIRE(pl != nullptr);
    // Максимум из bottom'ов: max(60, 80) = 80.
    REQUIRE(pl->geometry.y == Approx(80.0f).epsilon(0.01));
}

TEST_CASE("LTB clear: clear-left ignores right float",
    "[ltb][float][clear]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div class='bfc'>"
        "  <div class='r'></div>"
        "  <div class='p'></div>"
        "</div>",
        "html, body { display: block; margin: 0; padding: 0; } "
        ".bfc { display: block; width: 400px; } "
        ".r   { float: right; width: 100px; height: 80px; } "
        ".p   { clear: left;  width: 100px; height: 20px; }");

    LayoutNode* pl = find_by_soa_idx(p.root.get(), 4);
    REQUIRE(pl != nullptr);
    // clear-left не должен смотреть на правый float → y остаётся 0.
    REQUIRE(pl->geometry.y == Approx(0.0f).epsilon(0.01));
}

TEST_CASE("LTB clear: clear on float itself",
    "[ltb][float][clear]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div class='bfc'>"
        "  <div class='a'></div>"
        "  <div class='b'></div>"
        "</div>",
        "html, body { display: block; margin: 0; padding: 0; } "
        ".bfc { display: block; width: 400px; } "
        ".a { float: left; width: 100px; height: 50px; } "
        ".b { float: left; clear: left; width: 100px; height: 50px; }");

    LayoutNode* a = find_by_soa_idx(p.root.get(), 3);
    LayoutNode* b = find_by_soa_idx(p.root.get(), 4);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    // b с clear:left не встаёт рядом с a, а уходит под него.
    REQUIRE(b->geometry.x == Approx(0.0f).epsilon(0.01));
    REQUIRE(b->geometry.y == Approx(50.0f).epsilon(0.01));
}

// ============================================================
//  overflow: hidden — auto-height BFC
// ============================================================

TEST_CASE("LTB overflow: hidden establishes BFC and stretches under floats",
    "[ltb][float][bfc]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div class='bfc'>"
        "  <div class='f'></div>"
        "</div>",
        "html, body { display: block; margin: 0; padding: 0; } "
        ".bfc { display: block; width: 400px; overflow: hidden; } "
        ".f   { float: left; width: 100px; height: 60px; }");

    LayoutNode* bfc = find_by_soa_idx(p.root.get(), 2);
    REQUIRE(bfc != nullptr);
    // Раньше (без overflow) было бы 0; теперь растягивается под float.
    REQUIRE(bfc->geometry.height == Approx(60.0f).epsilon(0.01));
}

TEST_CASE("LTB overflow: hidden isolates inner floats from outer context",
    "[ltb][float][bfc]") {
    // Float внутри .inner не должен влиять на брата снаружи.
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div class='inner'>"
        "  <div class='f'></div>"
        "</div>"
        "<p class='outer'>X</p>",
        "html, body { display: block; margin: 0; padding: 0; } "
        ".inner { display: block; width: 200px; overflow: hidden; } "
        ".f     { float: left; width: 100px; height: 80px; } "
        ".outer { display: block; margin: 0; }");

    LayoutNode* para = find_by_soa_idx(p.root.get(), 4);
    REQUIRE(para != nullptr);
    // float упакован в .inner BFC — наружу не протекает,
    // значит X начинается с x = 0, а не с x = 100.
    REQUIRE(para->geometry.x == Approx(0.0f).epsilon(0.01));
}

// ============================================================
//  Border-radius
// ============================================================

namespace {
    inline const DisplayItem* find_rect_item(const DisplayList& dl,
        uint32_t color)
    {
        for (const auto& it : dl.items)
            if (it.type == DisplayItemType::Rect && it.color == color)
                return &it;
        return nullptr;
    }
}

TEST_CASE("LTB radius: shorthand 1 value sets all corners",
    "[ltb][radius]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='background-color: red; width: 100px; height: 50px;"
        "            border-radius: 8px'></div>",
        "html, body, div { display: block; margin: 0; padding: 0; }");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    const DisplayItem* it = find_rect_item(dl, 0xFF0000FFu);
    REQUIRE(it != nullptr);
    REQUIRE(it->radius_tl == Approx(8.0f));
    REQUIRE(it->radius_tr == Approx(8.0f));
    REQUIRE(it->radius_br == Approx(8.0f));
    REQUIRE(it->radius_bl == Approx(8.0f));
}

TEST_CASE("LTB radius: shorthand 2 values",
    "[ltb][radius]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='background-color: red; width: 100px; height: 50px;"
        "            border-radius: 4px 12px'></div>",
        "html, body, div { display: block; margin: 0; padding: 0; }");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    const DisplayItem* it = find_rect_item(dl, 0xFF0000FFu);
    REQUIRE(it != nullptr);
    REQUIRE(it->radius_tl == Approx(4.0f));
    REQUIRE(it->radius_tr == Approx(12.0f));
    REQUIRE(it->radius_br == Approx(4.0f));
    REQUIRE(it->radius_bl == Approx(12.0f));
}

TEST_CASE("LTB radius: 4 explicit corners",
    "[ltb][radius]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='background-color: red; width: 100px; height: 50px;"
        "            border-radius: 1px 2px 3px 4px'></div>",
        "html, body, div { display: block; margin: 0; padding: 0; }");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    const DisplayItem* it = find_rect_item(dl, 0xFF0000FFu);
    REQUIRE(it != nullptr);
    REQUIRE(it->radius_tl == Approx(1.0f));
    REQUIRE(it->radius_tr == Approx(2.0f));
    REQUIRE(it->radius_br == Approx(3.0f));
    REQUIRE(it->radius_bl == Approx(4.0f));
}

TEST_CASE("LTB radius: percent of min(w,h)",
    "[ltb][radius]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='background-color: red; width: 200px; height: 100px;"
        "            border-radius: 50%'></div>",
        "html, body, div { display: block; margin: 0; padding: 0; }");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    const DisplayItem* it = find_rect_item(dl, 0xFF0000FFu);
    REQUIRE(it != nullptr);
    // min(200,100)*0.5 = 50.
    REQUIRE(it->radius_tl == Approx(50.0f));
}

// ============================================================
//  background-size / position / repeat
// ============================================================

TEST_CASE("LTB bg-size: cover stored in DisplayItem",
    "[ltb][bg-size]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='width: 200px; height: 100px;"
        "            background-image: url(a.png); background-size: cover'></div>",
        "html, body, div { display: block; margin: 0; padding: 0; }");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    for (const auto& it : dl.items) {
        if (it.type == DisplayItemType::Image) {
            REQUIRE(it.is_background);
            REQUIRE(it.bg_size_kind == 1); // Cover
            return;
        }
    }
    FAIL("no image item");
}

TEST_CASE("LTB bg-size: contain", "[ltb][bg-size]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='width: 200px; height: 100px;"
        "            background-image: url(a.png); background-size: contain'></div>",
        "html, body, div { display: block; margin: 0; padding: 0; }");
    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    for (const auto& it : dl.items) {
        if (it.type == DisplayItemType::Image) {
            REQUIRE(it.bg_size_kind == 2);
            return;
        }
    }
    FAIL("no image");
}

TEST_CASE("LTB bg-size: explicit px", "[ltb][bg-size]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='width: 200px; height: 100px;"
        "            background-image: url(a.png);"
        "            background-size: 50px 30px'></div>",
        "html, body, div { display: block; margin: 0; padding: 0; }");
    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    for (const auto& it : dl.items) {
        if (it.type == DisplayItemType::Image) {
            REQUIRE(it.bg_size_kind == 3);
            REQUIRE(it.bg_size_w == Approx(50.0f));
            REQUIRE(it.bg_size_h == Approx(30.0f));
            return;
        }
    }
    FAIL("no image");
}

TEST_CASE("LTB bg-size: percent of border-box",
    "[ltb][bg-size]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='width: 200px; height: 100px;"
        "            background-image: url(a.png);"
        "            background-size: 50%'></div>",
        "html, body, div { display: block; margin: 0; padding: 0; }");
    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    for (const auto& it : dl.items) {
        if (it.type == DisplayItemType::Image) {
            REQUIRE(it.bg_size_w == Approx(100.0f));   // 50% * 200
            REQUIRE(it.bg_size_h == Approx(0.0f));     // auto
            return;
        }
    }
    FAIL("no image");
}

TEST_CASE("LTB bg-repeat: no-repeat", "[ltb][bg-repeat]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='width: 100px; height: 100px;"
        "            background-image: url(a.png);"
        "            background-repeat: no-repeat'></div>",
        "html, body, div { display: block; margin: 0; padding: 0; }");
    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    for (const auto& it : dl.items) {
        if (it.type == DisplayItemType::Image) {
            REQUIRE(it.bg_repeat == 3);
            return;
        }
    }
    FAIL("no image");
}

TEST_CASE("LTB bg-position: center keyword",
    "[ltb][bg-position]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='width: 100px; height: 100px;"
        "            background-image: url(a.png);"
        "            background-position: center'></div>",
        "html, body, div { display: block; margin: 0; padding: 0; }");
    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    for (const auto& it : dl.items) {
        if (it.type == DisplayItemType::Image) {
            REQUIRE(it.bg_pos_x_unit == 1);
            REQUIRE(it.bg_pos_y_unit == 1);
            REQUIRE(it.bg_pos_x == Approx(50.0f));
            REQUIRE(it.bg_pos_y == Approx(50.0f));
            return;
        }
    }
    FAIL("no image");
}

TEST_CASE("LTB bg-position: right bottom",
    "[ltb][bg-position]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='width: 100px; height: 100px;"
        "            background-image: url(a.png);"
        "            background-position: right bottom'></div>",
        "html, body, div { display: block; margin: 0; padding: 0; }");
    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    for (const auto& it : dl.items) {
        if (it.type == DisplayItemType::Image) {
            REQUIRE(it.bg_pos_x == Approx(100.0f));
            REQUIRE(it.bg_pos_y == Approx(100.0f));
            return;
        }
    }
    FAIL("no image");
}

// ============================================================
//  overflow: hidden — clip
// ============================================================

TEST_CASE("LTB clip: overflow hidden sets clip on children",
    "[ltb][clip]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='width: 100px; height: 100px; overflow: hidden;"
        "            background-color: red'>"
        "  <p style='width: 200px; height: 200px; background-color: blue'></p>"
        "</div>",
        "html, body, div { display: block; margin: 0; padding: 0; }"
        "p { display: block; margin: 0; }");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);

    // Ищем синий rect (child) — у него должен быть clip.
    const DisplayItem* child = find_rect_item(dl, 0x0000FFFFu);
    REQUIRE(child != nullptr);
    REQUIRE(child->has_clip);
    REQUIRE(child->clip_w == Approx(100.0f));
    REQUIRE(child->clip_h == Approx(100.0f));
}

TEST_CASE("LTB clip: overflow visible leaves no clip",
    "[ltb][clip]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='width: 100px; height: 100px'>"
        "  <p style='width: 50px; height: 50px; background-color: blue'></p>"
        "</div>",
        "html, body, div { display: block; margin: 0; padding: 0; }"
        "p { display: block; margin: 0; }");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    const DisplayItem* child = find_rect_item(dl, 0x0000FFFFu);
    REQUIRE(child != nullptr);
    REQUIRE_FALSE(child->has_clip);
}

TEST_CASE("LTB clip: nested overflow hidden intersects clips",
    "[ltb][clip]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div style='width: 200px; height: 200px; overflow: hidden'>"
        "  <div style='width: 50px; height: 50px; overflow: hidden'>"
        "    <p style='width: 400px; height: 400px;"
        "              background-color: blue'></p>"
        "  </div>"
        "</div>",
        "html, body, div { display: block; margin: 0; padding: 0; }"
        "p { display: block; margin: 0; }");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    const DisplayItem* child = find_rect_item(dl, 0x0000FFFFu);
    REQUIRE(child != nullptr);
    REQUIRE(child->has_clip);
    REQUIRE(child->clip_w == Approx(50.0f));
    REQUIRE(child->clip_h == Approx(50.0f));
}



TEST_CASE("LTB flex column: items stack vertically", "[ltb][layout][flex][column]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'><p id='a'></p><p id='b'></p></div>",
        "#f { display: flex; flex-direction: column; width: 100px; } "
        "#a { display: block; width: 50px; height: 20px; } "
        "#b { display: block; width: 60px; height: 30px; }");
    auto* a = find_by_soa_idx(p.root.get(), 3);
    auto* b = find_by_soa_idx(p.root.get(), 4);
    REQUIRE(a->geometry.y == Approx(0.0f).epsilon(0.001));
    REQUIRE(b->geometry.y == Approx(20.0f).epsilon(0.001)); // Ниже a
    REQUIRE(a->geometry.x == Approx(0.0f).epsilon(0.001));  // По левому краю
}

TEST_CASE("LTB flex wrap: items wrap to next line when exceeding width", "[ltb][layout][flex][wrap]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'><p id='a'></p><p id='b'></p><p id='c'></p></div>",
        "#f { display: flex; flex-wrap: wrap; width: 100px; } "
        "#a { display: block; width: 60px; height: 20px; } "
        "#b { display: block; width: 60px; height: 20px; } " // Не влезает рядом с a
        "#c { display: block; width: 60px; height: 20px; }");
    auto* a = find_by_soa_idx(p.root.get(), 3);
    auto* b = find_by_soa_idx(p.root.get(), 4);
    auto* c = find_by_soa_idx(p.root.get(), 5);

    REQUIRE(a->geometry.y == Approx(0.0f).epsilon(0.001));
    REQUIRE(b->geometry.y == Approx(20.0f).epsilon(0.001)); // Ушел на 2-ю строку
    REQUIRE(c->geometry.y == Approx(40.0f).epsilon(0.001));
}

TEST_CASE("LTB flex gap: applies row and column gaps", "[ltb][layout][flex][gap]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'><p id='a'></p><p id='b'></p></div>",
        "#f { display: flex; gap: 10px 20px; width: 200px; } " // 10px row, 20px col
        "#a { display: block; width: 50px; height: 20px; } "
        "#b { display: block; width: 50px; height: 20px; }");
    auto* a = find_by_soa_idx(p.root.get(), 3);
    auto* b = find_by_soa_idx(p.root.get(), 4);
    REQUIRE(a->geometry.x == Approx(0.0f).epsilon(0.001));
    REQUIRE(b->geometry.x == Approx(70.0f).epsilon(0.001)); // 50 + 20
}

TEST_CASE("LTB flex order: reorders children", "[ltb][layout][flex][order]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena, "<div id='f'><p id='a'></p><p id='b'></p></div>",
        "#f { display: flex; } "
        "#a { display: block; width: 50px; height: 20px; order: 2; } "
        "#b { display: block; width: 50px; height: 20px; order: 1; }");
    auto* a = find_by_soa_idx(p.root.get(), 3);
    auto* b = find_by_soa_idx(p.root.get(), 4);
    // b должен быть первым (x=0), a вторым (x=50)
    REQUIRE(b->geometry.x == Approx(0.0f).epsilon(0.001));
    REQUIRE(a->geometry.x == Approx(50.0f).epsilon(0.001));
}


// ============================================================
//  Тесты для CSS-переменных (var) и calc()
// ============================================================

TEST_CASE("CSS: var() resolves simple variable", "[css][var]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div class='a'></div>",
        ":root { --my-color: red; } "
        ".a { background-color: var(--my-color); width: 10px; height: 10px; }");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    bool found_red = false;
    for (const auto& it : dl.items) {
        if (it.type == DisplayItemType::Rect && it.color == 0xFF0000FFu) found_red = true;
    }
    REQUIRE(found_red);
}

TEST_CASE("CSS: var() with fallback", "[css][var]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div class='a'></div>",
        ".a { background-color: var(--undefined-color, blue); width: 10px; height: 10px; }");

    DisplayList dl = DisplayListBuilder::build(p.root.get(), p.storage);
    bool found_blue = false;
    for (const auto& it : dl.items) {
        if (it.type == DisplayItemType::Rect && it.color == 0x0000FFFFu) found_blue = true;
    }
    REQUIRE(found_blue);
}

TEST_CASE("CSS: calc() resolves basic math", "[css][calc]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div class='a'></div>",
        ".a { display: block; width: calc(100% - 50px); height: 20px; }",
        /*viewport=*/200.0f); // Ширина вьюпорта 200px

    // Ожидаем ширину 150px
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* div = child_of_type(body, BoxType::Block);
    REQUIRE(div->geometry.width == Approx(150.0f).epsilon(0.01));
}

// ============================================================
//  Тесты для CSS Grid
// ============================================================

TEST_CASE("Grid: basic 1fr 1fr layout", "[layout][grid]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div id='grid'><p id='a'></p><p id='b'></p></div>",
        "#grid { display: grid; grid-template-columns: 1fr 1fr; width: 100px; } "
        "#a { background: red; height: 10px; } "
        "#b { background: blue; height: 10px; }");

    LayoutNode* grid = find_by_soa_idx(p.root.get(), 2);
    LayoutNode* a = find_by_soa_idx(p.root.get(), 3);
    LayoutNode* b = find_by_soa_idx(p.root.get(), 4);

    REQUIRE(grid != nullptr);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    // Две колонки по 50px
    REQUIRE(a->geometry.width == Approx(50.0f).epsilon(0.01));
    REQUIRE(b->geometry.width == Approx(50.0f).epsilon(0.01));

    // 'a' слева, 'b' справа
    REQUIRE(a->geometry.x == Approx(0.0f).epsilon(0.01));
    REQUIRE(b->geometry.x == Approx(50.0f).epsilon(0.01));
}

TEST_CASE("Grid: repeat(3, 1fr) and gap", "[layout][grid]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div id='grid'><p id='a'></p><p id='b'></p><p id='c'></p></div>",
        "#grid { display: grid; grid-template-columns: repeat(3, 1fr); column-gap: 10px; width: 130px; } "
        "#a, #b, #c { height: 10px; }");

    LayoutNode* a = find_by_soa_idx(p.root.get(), 3);
    LayoutNode* b = find_by_soa_idx(p.root.get(), 4);
    LayoutNode* c = find_by_soa_idx(p.root.get(), 5);

    // Ширина 130. Два гапа по 10px = 20px. Остается 110 / 3 = 36.66px на колонку.
    float col_w = 110.0f / 3.0f;
    REQUIRE(a->geometry.width == Approx(col_w).epsilon(0.1));
    REQUIRE(b->geometry.width == Approx(col_w).epsilon(0.1));

    REQUIRE(a->geometry.x == Approx(0.0f).epsilon(0.1));
    REQUIRE(b->geometry.x == Approx(col_w + 10.0f).epsilon(0.1));
    REQUIRE(c->geometry.x == Approx((col_w + 10.0f) * 2).epsilon(0.1));
}

TEST_CASE("Grid: span 2 columns", "[layout][grid]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div id='grid'><p id='a'></p><p id='b'></p></div>",
        "#grid { display: grid; grid-template-columns: 1fr 1fr; width: 100px; } "
        "#a { grid-column: span 2; height: 10px; } "
        "#b { height: 10px; }");

    LayoutNode* a = find_by_soa_idx(p.root.get(), 3);
    LayoutNode* b = find_by_soa_idx(p.root.get(), 4);

    // 'a' должен занять всю ширину (100px)
    REQUIRE(a->geometry.width == Approx(100.0f).epsilon(0.01));

    // 'b' переносится на вторую строку
    REQUIRE(b->geometry.y == Approx(10.0f).epsilon(0.01));
}

TEST_CASE("Grid: px and 1fr mix", "[layout][grid]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div id='grid'><p id='a'></p><p id='b'></p></div>",
        "#grid { display: grid; grid-template-columns: 50px 1fr; width: 150px; } "
        "#a, #b { height: 10px; }");

    LayoutNode* a = find_by_soa_idx(p.root.get(), 3);
    LayoutNode* b = find_by_soa_idx(p.root.get(), 4);

    REQUIRE(a->geometry.width == Approx(50.0f).epsilon(0.01));
    REQUIRE(b->geometry.width == Approx(100.0f).epsilon(0.01));
}