#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

#include "../parsers/html_parser/headers/html_parser.h"
#include "../parsers/html_parser/headers/dom.h"
#include "../parsers/arena_memory_allocator/headers/arena.h"

// ============================================================
//  Вспомогательные функции.
//  ВАЖНО: DOM хранит строки как std::pmr::string, поэтому все
//  сравнения делаем через std::string_view — без временных
//  std::string и без обращений к attributes.at("ключ").
// ============================================================
namespace {

    // Универсальный взгляд на любую basic_string (std::string, pmr::string, ...).
    template <class Traits, class Alloc>
    inline std::string_view sv(const std::basic_string<char, Traits, Alloc>& s) {
        return { s.data(), s.size() };
    }

    DOMNode* first_element(const DOMNode* root) {
        if (!root) return nullptr;
        for (DOMNode* c : root->children)
            if (c->type == NodeType::Element) return c;
        return nullptr;
    }

    DOMNode* find_tag(const DOMNode* root, std::string_view tag) {
        if (!root) return nullptr;
        for (DOMNode* c : root->children)
            if (c->type == NodeType::Element && sv(c->tag_name) == tag)
                return c;
        return nullptr;
    }

    size_t count_type(const DOMNode* node, NodeType t) {
        size_t n = 0;
        for (const DOMNode* c : node->children) if (c->type == t) ++n;
        return n;
    }

    void collect_text(const DOMNode* node, std::string& out) {
        if (!node) return;
        if (node->type == NodeType::Text)
            out.append(node->text_content.data(), node->text_content.size());
        for (const DOMNode* c : node->children) collect_text(c, out);
    }
    std::string all_text(const DOMNode* root) {
        std::string s; collect_text(root, s); return s;
    }

    DOMNode* first_text(const DOMNode* root) {
        if (!root) return nullptr;
        for (DOMNode* c : root->children)
            if (c->type == NodeType::Text) return c;
        return nullptr;
    }

    // --- attribute lookup без аллокаций и без .at("key") ---
    std::string_view attr(const DOMNode* n, std::string_view name) {
        for (const auto& kv : n->attributes)
            if (sv(kv.first) == name) return sv(kv.second);
        return {};
    }
    bool has_attr(const DOMNode* n, std::string_view name) {
        for (const auto& kv : n->attributes)
            if (sv(kv.first) == name) return true;
        return false;
    }

    struct Parsed { DOMNode* root = nullptr; };

    inline Parsed parse(ArenaAllocator& arena, const std::string& html) {
        HTMLParser p(arena);
        return { p.parse(html) };
    }

} // namespace


// ============================================================
//  Базовые
// ============================================================

TEST_CASE("HTMLParser: empty input yields empty document",
    "[html_parser][basic]") {
    ArenaAllocator arena;
    auto r = parse(arena, "");
    REQUIRE(r.root != nullptr);
    REQUIRE(r.root->type == NodeType::Document);
    REQUIRE(r.root->children.empty());
}

TEST_CASE("HTMLParser: whitespace-only input becomes a text child",
    "[html_parser][basic]") {
    ArenaAllocator arena;
    auto r = parse(arena, "   \n\t  ");
    REQUIRE(r.root->children.size() == 1);
    REQUIRE(r.root->children[0]->type == NodeType::Text);
}

TEST_CASE("HTMLParser: simple element",
    "[html_parser][basic]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<div></div>");
    REQUIRE(r.root->children.size() == 1);
    DOMNode* div = r.root->children[0];
    REQUIRE(div->type == NodeType::Element);
    REQUIRE(div->tag_name == "div");
    REQUIRE(div->children.empty());
    REQUIRE(div->parent == r.root);
}

TEST_CASE("HTMLParser: root node is Document",
    "[html_parser][basic]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<html></html>");
    REQUIRE(r.root->type == NodeType::Document);
    REQUIRE(r.root->tag_name == "#document");
    REQUIRE(r.root->parent == nullptr);
}

TEST_CASE("HTMLParser: nested elements",
    "[html_parser][basic]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<div><p><span></span></p></div>");
    DOMNode* div = first_element(r.root);
    REQUIRE(div != nullptr);
    DOMNode* p = first_element(div);
    REQUIRE(p != nullptr);
    REQUIRE(p->tag_name == "p");
    DOMNode* span = first_element(p);
    REQUIRE(span != nullptr);
    REQUIRE(span->tag_name == "span");
}

TEST_CASE("HTMLParser: multiple siblings",
    "[html_parser][basic]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<a></a><b></b><c></c>");
    REQUIRE(r.root->children.size() == 3);
    REQUIRE(r.root->children[0]->tag_name == "a");
    REQUIRE(r.root->children[1]->tag_name == "b");
    REQUIRE(r.root->children[2]->tag_name == "c");
}

TEST_CASE("HTMLParser: text before, between, after elements",
    "[html_parser][basic]") {
    ArenaAllocator arena;
    auto r = parse(arena, "before<p>mid</p>after");
    REQUIRE(r.root->children.size() == 3);
    REQUIRE(r.root->children[0]->type == NodeType::Text);
    REQUIRE(r.root->children[0]->text_content == "before");
    REQUIRE(r.root->children[1]->type == NodeType::Element);
    REQUIRE(r.root->children[2]->type == NodeType::Text);
    REQUIRE(r.root->children[2]->text_content == "after");
}

TEST_CASE("HTMLParser: parser is reusable across calls",
    "[html_parser][basic]") {
    ArenaAllocator arena;
    HTMLParser p(arena);
    DOMNode* a = p.parse("<div></div>");
    DOMNode* b = p.parse("<span></span>");
    REQUIRE(a != b);
    REQUIRE(a->children.size() == 1);
    REQUIRE(a->children[0]->tag_name == "div");
    REQUIRE(b->children.size() == 1);
    REQUIRE(b->children[0]->tag_name == "span");
}


// ============================================================
//  Теги
// ============================================================

TEST_CASE("HTMLParser: uppercase tag lowercased",
    "[html_parser][tag]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<DIV></DIV>");
    REQUIRE(r.root->children.size() == 1);
    REQUIRE(r.root->children[0]->tag_name == "div");
}

TEST_CASE("HTMLParser: mixed-case tag lowercased",
    "[html_parser][tag]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<DiV></dIv>");
    REQUIRE(r.root->children[0]->tag_name == "div");
}

TEST_CASE("HTMLParser: tag with dash", "[html_parser][tag]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<my-element></my-element>");
    REQUIRE(r.root->children[0]->tag_name == "my-element");
}

TEST_CASE("HTMLParser: nested same-name tags close correctly",
    "[html_parser][tag]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<div><div></div></div>");
    DOMNode* outer = first_element(r.root);
    REQUIRE(outer->tag_name == "div");
    REQUIRE(outer->children.size() == 1);
    DOMNode* inner = outer->children[0];
    REQUIRE(inner->tag_name == "div");
    REQUIRE(inner->children.empty());
}


// ============================================================
//  Атрибуты
// ============================================================

TEST_CASE("HTMLParser: attribute with double quotes",
    "[html_parser][attr]") {
    ArenaAllocator arena;
    auto r = parse(arena, R"(<div class="foo"></div>)");
    DOMNode* div = first_element(r.root);
    REQUIRE(div->attributes.size() == 1);
    REQUIRE(attr(div, "class") == "foo");
}

TEST_CASE("HTMLParser: attribute with single quotes",
    "[html_parser][attr]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<div class='foo'></div>");
    REQUIRE(attr(first_element(r.root), "class") == "foo");
}

TEST_CASE("HTMLParser: attribute unquoted",
    "[html_parser][attr]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<div id=main></div>");
    REQUIRE(attr(first_element(r.root), "id") == "main");
}

TEST_CASE("HTMLParser: multiple attributes",
    "[html_parser][attr]") {
    ArenaAllocator arena;
    auto r = parse(arena,
        R"(<input type="text" name="q" value="hello world" disabled>)");
    DOMNode* in = first_element(r.root);
    REQUIRE(attr(in, "type") == "text");
    REQUIRE(attr(in, "name") == "q");
    REQUIRE(attr(in, "value") == "hello world");
    REQUIRE(has_attr(in, "disabled"));
    REQUIRE(attr(in, "disabled").empty());
}

TEST_CASE("HTMLParser: boolean attribute has empty value",
    "[html_parser][attr]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<input disabled>");
    DOMNode* in = first_element(r.root);
    REQUIRE(has_attr(in, "disabled"));
    REQUIRE(attr(in, "disabled") == "");
}

TEST_CASE("HTMLParser: empty attribute value with equals",
    "[html_parser][attr]") {
    ArenaAllocator arena;
    auto r = parse(arena, R"(<div class=""></div>)");
    REQUIRE(attr(first_element(r.root), "class").empty());
}

TEST_CASE("HTMLParser: attribute name is lowercased",
    "[html_parser][attr]") {
    ArenaAllocator arena;
    auto r = parse(arena, R"(<div CLASS="Foo"></div>)");
    DOMNode* div = first_element(r.root);
    REQUIRE(has_attr(div, "class"));
    // Значение сохраняет регистр
    REQUIRE(attr(div, "class") == "Foo");
}

TEST_CASE("HTMLParser: whitespace around equals",
    "[html_parser][attr]") {
    ArenaAllocator arena;
    auto r = parse(arena, R"(<div class = "foo" ></div>)");
    REQUIRE(attr(first_element(r.root), "class") == "foo");
}

TEST_CASE("HTMLParser: attribute value with spaces inside quotes",
    "[html_parser][attr]") {
    ArenaAllocator arena;
    auto r = parse(arena, R"(<div title="a b c"></div>)");
    REQUIRE(attr(first_element(r.root), "title") == "a b c");
}

TEST_CASE("HTMLParser: duplicate attribute last wins",
    "[html_parser][attr]") {
    ArenaAllocator arena;
    auto r = parse(arena, R"(<div class="a" class="b"></div>)");
    REQUIRE(attr(first_element(r.root), "class") == "b");
}


// ============================================================
//  Void-элементы и self-closing
// ============================================================

TEST_CASE("HTMLParser: <br> is void",
    "[html_parser][void]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<div><br></div>");
    DOMNode* div = first_element(r.root);
    REQUIRE(div->children.size() == 1);
    REQUIRE(div->children[0]->tag_name == "br");
    REQUIRE(div->children[0]->children.empty());
}

TEST_CASE("HTMLParser: void does not swallow siblings",
    "[html_parser][void]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<br><p></p>");
    REQUIRE(r.root->children.size() == 2);
    REQUIRE(r.root->children[0]->tag_name == "br");
    REQUIRE(r.root->children[1]->tag_name == "p");
}

TEST_CASE("HTMLParser: common void elements",
    "[html_parser][void]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<img src=\"x\"><hr><input><meta><link>");
    REQUIRE(r.root->children.size() == 5);
    REQUIRE(r.root->children[0]->tag_name == "img");
    REQUIRE(r.root->children[1]->tag_name == "hr");
    REQUIRE(r.root->children[2]->tag_name == "input");
    REQUIRE(r.root->children[3]->tag_name == "meta");
    REQUIRE(r.root->children[4]->tag_name == "link");
}

// Парсер трактует "/>" в конце тега как самозакрытие (XML-стиль),
// даже для не-void элементов. Тест фиксирует именно это поведение.
// Если позже парсер перейдёт на HTML5-семантику ("<div/>" == открытый <div>),
// тест нужно будет инвертировать (см. комментарий в конце).
TEST_CASE("HTMLParser: non-void self-closing closes the element",
    "[html_parser][void]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<div/><span></span>");

    // <div/> — самозакрытый, <span> — его сосед на верхнем уровне.
    REQUIRE(r.root->children.size() == 2);
    REQUIRE(r.root->children[0]->tag_name == "div");
    REQUIRE(r.root->children[0]->children.empty());
    REQUIRE(r.root->children[1]->tag_name == "span");
    REQUIRE(r.root->children[1]->children.empty());

    // HTML5-вариант (если переключите парсер):
    //   REQUIRE(r.root->children.size() == 1);
    //   REQUIRE(r.root->children[0]->tag_name == "div");
    //   REQUIRE(r.root->children[0]->children.size() == 1);
    //   REQUIRE(r.root->children[0]->children[0]->tag_name == "span");
}

TEST_CASE("HTMLParser: void self-closing with slash closes",
    "[html_parser][void]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<br/><p></p>");
    REQUIRE(r.root->children.size() == 2);
    REQUIRE(r.root->children[0]->tag_name == "br");
    REQUIRE(r.root->children[0]->children.empty());
    REQUIRE(r.root->children[1]->tag_name == "p");
}

TEST_CASE("HTMLParser: void with space before slash",
    "[html_parser][void]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<br /><img src=\"x\" /><p></p>");
    REQUIRE(r.root->children.size() == 3);
    REQUIRE(r.root->children[0]->tag_name == "br");
    REQUIRE(r.root->children[1]->tag_name == "img");
    REQUIRE(r.root->children[2]->tag_name == "p");
}


// ============================================================
//  Текст
// ============================================================

TEST_CASE("HTMLParser: plain text becomes Text node",
    "[html_parser][text]") {
    ArenaAllocator arena;
    auto r = parse(arena, "hello world");
    REQUIRE(r.root->children.size() == 1);
    REQUIRE(r.root->children[0]->type == NodeType::Text);
    REQUIRE(r.root->children[0]->text_content == "hello world");
}

TEST_CASE("HTMLParser: text inside element",
    "[html_parser][text]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<p>Hello</p>");
    DOMNode* p = first_element(r.root);
    REQUIRE(p->children.size() == 1);
    REQUIRE(p->children[0]->type == NodeType::Text);
    REQUIRE(p->children[0]->text_content == "Hello");
}

TEST_CASE("HTMLParser: text with inner structure",
    "[html_parser][text]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<p>Hello <b>world</b>!</p>");
    DOMNode* p = first_element(r.root);
    REQUIRE(p->children.size() == 3);
    REQUIRE(p->children[0]->type == NodeType::Text);
    REQUIRE(p->children[0]->text_content == "Hello ");
    REQUIRE(p->children[1]->tag_name == "b");
    REQUIRE(p->children[2]->type == NodeType::Text);
    REQUIRE(p->children[2]->text_content == "!");
}


// ============================================================
//  Комментарии
// ============================================================

TEST_CASE("HTMLParser: simple comment",
    "[html_parser][comment]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<!-- hello -->");
    REQUIRE(r.root->children.size() == 1);
    REQUIRE(r.root->children[0]->type == NodeType::Comment);
    REQUIRE(r.root->children[0]->text_content == " hello ");
}

TEST_CASE("HTMLParser: comment before and after element",
    "[html_parser][comment]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<!--a--><div></div><!--b-->");
    REQUIRE(r.root->children.size() == 3);
    REQUIRE(r.root->children[0]->type == NodeType::Comment);
    REQUIRE(r.root->children[1]->type == NodeType::Element);
    REQUIRE(r.root->children[2]->type == NodeType::Comment);
}

TEST_CASE("HTMLParser: empty comment",
    "[html_parser][comment]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<!---->");
    REQUIRE(r.root->children.size() == 1);
    REQUIRE(r.root->children[0]->type == NodeType::Comment);
    REQUIRE(r.root->children[0]->text_content.empty());
}

TEST_CASE("HTMLParser: comment inside element",
    "[html_parser][comment]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<div><!--inner--></div>");
    DOMNode* div = first_element(r.root);
    REQUIRE(div->children.size() == 1);
    REQUIRE(div->children[0]->type == NodeType::Comment);
    REQUIRE(div->children[0]->text_content == "inner");
}

TEST_CASE("HTMLParser: comment with dash inside",
    "[html_parser][comment]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<!--a-b-c-->");
    REQUIRE(r.root->children[0]->text_content == "a-b-c");
}


// ============================================================
//  DOCTYPE
// ============================================================

TEST_CASE("HTMLParser: simple DOCTYPE",
    "[html_parser][doctype]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<!DOCTYPE html>");
    REQUIRE(r.root->children.size() == 1);
    REQUIRE(r.root->children[0]->type == NodeType::Doctype);
    REQUIRE(r.root->children[0]->text_content == "html");
}

TEST_CASE("HTMLParser: DOCTYPE case-insensitive",
    "[html_parser][doctype]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<!doctype html>");
    REQUIRE(r.root->children[0]->type == NodeType::Doctype);
    REQUIRE(r.root->children[0]->text_content == "html");
}

TEST_CASE("HTMLParser: DOCTYPE with PUBLIC is skipped to bogus",
    "[html_parser][doctype]") {
    ArenaAllocator arena;
    auto r = parse(arena,
        "<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01//EN\" \"http://x\">");
    REQUIRE(r.root->children.size() >= 1);
}


// ============================================================
//  Entity
// ============================================================

TEST_CASE("HTMLParser: &amp; in text",
    "[html_parser][entity]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<p>a &amp; b</p>");
    REQUIRE(all_text(r.root) == "a & b");
}

TEST_CASE("HTMLParser: &lt; &gt; &quot; &apos;",
    "[html_parser][entity]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<p>&lt;&gt;&quot;&apos;</p>");
    REQUIRE(all_text(r.root) == "<>\"'");
}

TEST_CASE("HTMLParser: numeric decimal &#65;",
    "[html_parser][entity]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<p>&#65;&#66;&#67;</p>");
    REQUIRE(all_text(r.root) == "ABC");
}

TEST_CASE("HTMLParser: numeric hex &#x41;",
    "[html_parser][entity]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<p>&#x41;&#X42;</p>");
    REQUIRE(all_text(r.root) == "AB");
}

TEST_CASE("HTMLParser: unknown entity left mostly intact",
    "[html_parser][entity]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<p>&unknown;</p>");
    // Главное — не падать и не терять контент целиком.
    REQUIRE(all_text(r.root).find('&') != std::string::npos);
}

TEST_CASE("HTMLParser: entity in attribute value",
    "[html_parser][entity]") {
    ArenaAllocator arena;
    auto r = parse(arena, R"(<div title="a &amp; b"></div>)");
    REQUIRE(attr(first_element(r.root), "title") == "a & b");
}

TEST_CASE("HTMLParser: &nbsp; produces non-breaking space codepoint",
    "[html_parser][entity]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<p>a&nbsp;b</p>");
    std::string s = all_text(r.root);
    // U+00A0 в UTF-8: 0xC2 0xA0
    REQUIRE(s.find("\xC2\xA0") != std::string::npos);
}


// ============================================================
//  RCDATA (title, textarea)
// ============================================================

TEST_CASE("HTMLParser: <title> preserves text",
    "[html_parser][rcdata]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<title>Hello Page</title>");
    DOMNode* t = first_element(r.root);
    REQUIRE(t->tag_name == "title");
    REQUIRE(all_text(t) == "Hello Page");
}

TEST_CASE("HTMLParser: <title> decodes entities",
    "[html_parser][rcdata]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<title>a &amp; b</title>");
    REQUIRE(all_text(r.root) == "a & b");
}

TEST_CASE("HTMLParser: <textarea> preserves text",
    "[html_parser][rcdata]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<textarea>hello</textarea>");
    DOMNode* t = first_element(r.root);
    REQUIRE(t->tag_name == "textarea");
    REQUIRE(all_text(t) == "hello");
}

TEST_CASE("HTMLParser: </title> case-insensitive",
    "[html_parser][rcdata]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<TITLE>x</TITLE><p>after</p>");
    REQUIRE(r.root->children.size() == 2);
    REQUIRE(r.root->children[0]->tag_name == "title");
    REQUIRE(r.root->children[1]->tag_name == "p");
}


// ============================================================
//  RAWTEXT (style, script)
// ============================================================

TEST_CASE("HTMLParser: <style> content kept literally",
    "[html_parser][rawtext]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<style>body { color: red; }</style>");
    DOMNode* st = first_element(r.root);
    REQUIRE(st->tag_name == "style");
    REQUIRE(all_text(st) == "body { color: red; }");
}

TEST_CASE("HTMLParser: <script> content kept literally",
    "[html_parser][rawtext]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<script>var x = 1 < 2;</script>");
    DOMNode* sc = first_element(r.root);
    REQUIRE(sc->tag_name == "script");
    REQUIRE(all_text(sc) == "var x = 1 < 2;");
}

TEST_CASE("HTMLParser: </script> closing tag detected",
    "[html_parser][rawtext]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<script>x</script><p></p>");
    REQUIRE(r.root->children.size() == 2);
    REQUIRE(r.root->children[0]->tag_name == "script");
    REQUIRE(r.root->children[1]->tag_name == "p");
}


// ============================================================
//  Malformed
// ============================================================

TEST_CASE("HTMLParser: stray '</>' ignored",
    "[html_parser][malformed]") {
    ArenaAllocator arena;
    auto r = parse(arena, "</>");
    REQUIRE(r.root->children.empty());
}

TEST_CASE("HTMLParser: '< ' becomes text",
    "[html_parser][malformed]") {
    ArenaAllocator arena;
    auto r = parse(arena, "< not a tag");
    REQUIRE(r.root->children.size() >= 1);
    std::string t = all_text(r.root);
    REQUIRE(t.find('<') != std::string::npos);
}

TEST_CASE("HTMLParser: closing tag without opener ignored",
    "[html_parser][malformed]") {
    ArenaAllocator arena;
    auto r = parse(arena, "</div><p></p>");
    REQUIRE(r.root->children.size() == 1);
    REQUIRE(r.root->children[0]->tag_name == "p");
}

TEST_CASE("HTMLParser: unclosed tag leaves element in tree",
    "[html_parser][malformed]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<div>");
    REQUIRE(r.root->children.size() == 1);
    REQUIRE(r.root->children[0]->tag_name == "div");
}

TEST_CASE("HTMLParser: mismatched closing tag ignored",
    "[html_parser][malformed]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<div></span></div>");
    DOMNode* div = first_element(r.root);
    REQUIRE(div->tag_name == "div");
    REQUIRE(div->children.empty());
}

TEST_CASE("HTMLParser: stray '>' as text",
    "[html_parser][malformed]") {
    ArenaAllocator arena;
    auto r = parse(arena, "a > b");
    REQUIRE(all_text(r.root) == "a > b");
}

TEST_CASE("HTMLParser: '<' inside text as raw char",
    "[html_parser][malformed]") {
    ArenaAllocator arena;
    auto r = parse(arena, "<p>1 &lt; 2</p>");
    REQUIRE(all_text(r.root) == "1 < 2");
}


// ============================================================
//  Интеграция
// ============================================================

TEST_CASE("HTMLParser: minimal full document",
    "[html_parser][integration]") {
    ArenaAllocator arena;
    auto r = parse(arena,
        "<!DOCTYPE html>"
        "<html>"
        "<head><title>Hi</title></head>"
        "<body><p>Hello</p></body>"
        "</html>");
    REQUIRE(r.root->children.size() == 2);  // doctype + html
    REQUIRE(r.root->children[0]->type == NodeType::Doctype);

    DOMNode* html = find_tag(r.root, "html");
    REQUIRE(html != nullptr);
    REQUIRE(html->children.size() == 2);

    DOMNode* head = html->children[0];
    REQUIRE(head->tag_name == "head");
    DOMNode* title = first_element(head);
    REQUIRE(title->tag_name == "title");

    DOMNode* body = html->children[1];
    REQUIRE(body->tag_name == "body");
    DOMNode* p = first_element(body);
    REQUIRE(p->tag_name == "p");
    REQUIRE(all_text(p) == "Hello");
}

TEST_CASE("HTMLParser: card with mixed content",
    "[html_parser][integration]") {
    ArenaAllocator arena;
    auto r = parse(arena,
        "<div class=\"card\">"
        "  <h1>Title</h1>"
        "  <p>Some <b>bold</b> text.</p>"
        "  <button disabled>Click</button>"
        "</div>");
    DOMNode* card = first_element(r.root);
    REQUIRE(card->tag_name == "div");
    REQUIRE(attr(card, "class") == "card");

    // Считаем только элементы — не зависим от того,
    // схлопывает ли парсер whitespace-only текстовые узлы.
    REQUIRE(count_type(card, NodeType::Element) == 3);

    DOMNode* h1 = find_tag(card, "h1");
    REQUIRE(h1 != nullptr);
    REQUIRE(all_text(h1) == "Title");

    DOMNode* btn = find_tag(card, "button");
    REQUIRE(btn != nullptr);
    REQUIRE(has_attr(btn, "disabled"));
    REQUIRE(all_text(btn) == "Click");
}

TEST_CASE("HTMLParser: comment + doctype + element",
    "[html_parser][integration]") {
    ArenaAllocator arena;
    auto r = parse(arena,
        "<!-- header -->"
        "<!DOCTYPE html>"
        "<html><body></body></html>");
    REQUIRE(r.root->children.size() == 3);
    REQUIRE(r.root->children[0]->type == NodeType::Comment);
    REQUIRE(r.root->children[1]->type == NodeType::Doctype);
    REQUIRE(r.root->children[2]->tag_name == "html");
}

TEST_CASE("HTMLParser: numeric entity in attribute",
    "[html_parser][attr][entity]") {
    ArenaAllocator arena;
    auto r = parse(arena, R"(<div title="&#65;&#x42;"></div>)");
    REQUIRE(attr(first_element(r.root), "title") == "AB");
}

TEST_CASE("HTMLParser: entity at start of attribute value",
    "[html_parser][attr][entity]") {
    ArenaAllocator arena;
    auto r = parse(arena, R"(<div title="&amp;start"></div>)");
    REQUIRE(attr(first_element(r.root), "title") == "&start");
}

TEST_CASE("HTMLParser: entity at end of attribute value",
    "[html_parser][attr][entity]") {
    ArenaAllocator arena;
    auto r = parse(arena, R"(<div title="end&amp;"></div>)");
    REQUIRE(attr(first_element(r.root), "title") == "end&");
}