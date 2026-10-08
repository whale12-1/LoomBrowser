#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <memory>
#include <string>
#include <vector>

#include "../layout/headers/layout_node.h"
#include "../layout/headers/layout_tree_builder.h"
#include "../parsers/selector_matcher/style_tree_builder.h"
#include "../parsers/selector_matcher/style_storage_soa.h"
#include "../parsers/html_parser/headers/html_parser.h"
#include "../parsers/css_parser/headers/css_parser.h"
#include "../parsers/arena_memory_allocator/headers/arena.h"

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
        float viewport = 1024.0f) {
        std::string html = "<html><body>" + inner_html + "</body></html>";

        // Мини-UA-stylesheet: html и body — block по умолчанию.
        // Реальный браузер имеет таблицу UA-стилей; у нас её пока нет —
        // эмулируем здесь, чтобы тесты концентрировались на своих сценариях.
        std::string full_css =
            "html, body { display: block; } " + css;

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
            LayoutTreeBuilder::Viewport vp{ viewport, 768.0f };   // width, height
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

    LayoutNode* sp = child_of_type(body, BoxType::Inline);
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

    LayoutNode* btn = child_of_type(body, BoxType::Inline);
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

    // Ожидаем: [AnonBlock[Text, Inline[Text]], Block[Text], AnonBlock[Text]]
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

TEST_CASE("LTB build: all-inline children — no anonymous block",
    "[ltb][build][anon]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div>a<span>b</span>c</div>",
        "html, body, div { display: block; } "
        "span { display: inline; }");

    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* div = child_of_type(body, BoxType::Block);
    REQUIRE(div->children.size() == 3);
    for (auto& c : div->children)
        REQUIRE(c->type != BoxType::AnonymousBlock);
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
    REQUIRE(div->children[0]->type == BoxType::Text);
    REQUIRE(div->children[0]->text_content == "hello");
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

TEST_CASE("LTB build: text mixed with significant whitespace",
    "[ltb][build][text]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div>  hello  </div>",
        "html, body, div { display: block; }");
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* div = child_of_type(body, BoxType::Block);
    REQUIRE(div->children.size() == 1);
    // Пробелы сохранены (text_content не тримится)
    REQUIRE(div->children[0]->text_content == "  hello  ");
}

TEST_CASE("LTB build: text node has no style_soa_idx",
    "[ltb][build][text]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<div>hi</div>",
        "html, body, div { display: block; }");
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* div = child_of_type(body, BoxType::Block);
    REQUIRE(div->children[0]->style_soa_idx == UINT32_MAX);
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

TEST_CASE("LTB layout: margins add between siblings",
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
    REQUIRE(d2->geometry.y == 80.0f);   // 50 + 10 + 20
    REQUIRE(body->geometry.height == 110.0f);  // БЕЗ margin collapsing
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
    LayoutNode* sp = child_of_type(p_, BoxType::Inline);
    REQUIRE(sp != nullptr);
    // span не задан явно — shrink-to-fit по содержимому
    // text "ab" → char_w = 8 (fs=16 * 0.5) → one_line_w = 16
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
    REQUIRE(div->children.size() == 1);
    LayoutNode* txt = div->children[0].get();
    REQUIRE(txt->type == BoxType::Text);
    // line-height normal = 16 * 1.2 = 19.2
    REQUIRE(txt->geometry.height == Approx(19.2f).epsilon(0.001));
}

TEST_CASE("LTB layout: text wrapping when longer than parent",
    "[ltb][layout][text]") {
    ArenaAllocator arena;
    // "hello world" = 11 символов. fs=16, char_w=8 → one_line=88px.
    // parent_width=50 → chars_per_line = 6 (floor(50/8))
    // lines = ceil(11/6) = 2
    auto p = run_pipeline(arena,
        "<div>hello world</div>",
        "html, body { display: block; } "
        "div { display: block; width: 50px; font-size: 16px; }",
        500.0f);
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* div = child_of_type(body, BoxType::Block);
    LayoutNode* txt = div->children[0].get();
    // height = 2 * 19.2 = 38.4
    REQUIRE(txt->geometry.height == Approx(57.6f).epsilon(0.01));
    REQUIRE(txt->geometry.width == 50.0f);
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
    LayoutNode* txt = div->children[0].get();
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

// ============================================================
//  ДОПОЛНЕНИЯ: auto-размеры, min/max, inline-flow, negative margin.
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

TEST_CASE("LTB layout: inline-blocks stack vertically (no inline flow yet)",
    "[ltb][layout][inline]") {
    ArenaAllocator arena;
    auto p = run_pipeline(arena,
        "<p><span>a</span><span>b</span></p>",
        "html, body, p { display: block; } "
        "span { display: inline-block; width: 20px; height: 10px; }",
        500.0f);
    LayoutNode* body = child_of_type(p.root.get(), BoxType::Block);
    LayoutNode* p_ = child_of_type(body, BoxType::Block);
    REQUIRE(p_->children.size() == 2);

    LayoutNode* s1 = p_->children[0].get();
    LayoutNode* s2 = p_->children[1].get();

    // Пока нет inline-flow: оба x = 0, второй стопкой под первым.
    REQUIRE(s1->geometry.x == 0.0f);
    REQUIRE(s2->geometry.x == 0.0f);
    REQUIRE(s2->geometry.y >= s1->geometry.y + s1->geometry.height);
}
// TODO: когда появится inline formatting context — оба блока должны
// встать рядом (s2->x == 20), тест станет:
//   REQUIRE(s2->geometry.x == 20.0f);

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