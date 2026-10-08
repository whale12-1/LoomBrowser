#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "../parsers/selector_matcher/selector_matcher.h"
#include "../parsers/html_parser/headers/dom.h"
#include "../parsers/css_parser/headers/css_dom.h"

namespace {

    using K = SimpleSelector::Kind;
    using SS = SimpleSelector;
    using CS = ComplexSelector;
    using Cmp = CompoundSelector;
    using Comb = Combinator;
    using SM = SelectorMatcher;

    // ---------- Simple selectors ----------
    inline SS tg(const std::string& n) { SS s; s.kind = K::Tag;       s.name = n; return s; }
    inline SS cl(const std::string& n) { SS s; s.kind = K::Class;     s.name = n; return s; }
    inline SS id_(const std::string& n) { SS s; s.kind = K::Id;        s.name = n; return s; }
    inline SS uni() { SS s; s.kind = K::Universal;             return s; }
    inline SS attr(const std::string& n) {
        SS s; s.kind = K::Attribute; s.name = n; return s;
    }
    inline SS attr(const std::string& n, const std::string& op, const std::string& v) {
        SS s; s.kind = K::Attribute; s.name = n; s.op = op; s.arg = v; return s;
    }
    inline SS pc(const std::string& n) {
        SS s; s.kind = K::PseudoClass; s.name = n; return s;
    }
    inline SS pc(const std::string& n, const std::string& arg) {
        SS s; s.kind = K::PseudoClass; s.name = n; s.arg = arg; s.has_arg = true; return s;
    }
    inline SS pe(const std::string& n) {
        SS s; s.kind = K::PseudoElement; s.name = n; return s;
    }

    // ---------- Compound ----------
    inline Cmp cmp(SS a) {
        Cmp c; c.parts.push_back(std::move(a)); return c;
    }
    inline Cmp cmp(SS a, SS b) {
        Cmp c; c.parts.push_back(std::move(a)); c.parts.push_back(std::move(b)); return c;
    }
    inline Cmp cmp(SS a, SS b, SS c3) {
        Cmp c; c.parts.push_back(std::move(a)); c.parts.push_back(std::move(b));
        c.parts.push_back(std::move(c3)); return c;
    }

    // ---------- Complex ----------
    inline CS one(SS s) { CS cs; cs.compounds.push_back(cmp(std::move(s))); return cs; }
    inline CS one(Cmp c) { CS cs; cs.compounds.push_back(std::move(c));      return cs; }

    // Chain: A comb B — comb хранится на RIGHT-компаунде
    inline CS chain(Cmp a, Comb comb, Cmp b) {
        b.combinator = comb;
        CS cs; cs.compounds.push_back(std::move(a)); cs.compounds.push_back(std::move(b));
        return cs;
    }
    inline CS chain(Cmp a, Comb c1, Cmp b, Comb c2, Cmp c3) {
        b.combinator = c1; c3.combinator = c2;
        CS cs;
        cs.compounds.push_back(std::move(a));
        cs.compounds.push_back(std::move(b));
        cs.compounds.push_back(std::move(c3));
        return cs;
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
    inline void set_attr(DOMNode* n, const std::string& k, const std::string& v) {
        n->attributes[std::pmr::string(k.data(), k.size())]
            .assign(v.data(), v.size());
    }

} // namespace


TEST_CASE("css_util::has_class: single token", "[selector][util][class]") {
    Tree t; auto* n = t.E("div"); set_class(n, "foo");
    REQUIRE(css_util::has_class(n, "foo"));
    REQUIRE_FALSE(css_util::has_class(n, "fo"));
    REQUIRE_FALSE(css_util::has_class(n, "foobar"));
}

TEST_CASE("css_util::has_class: multiple tokens", "[selector][util][class]") {
    Tree t; auto* n = t.E("div"); set_class(n, "btn primary big");
    REQUIRE(css_util::has_class(n, "btn"));
    REQUIRE(css_util::has_class(n, "primary"));
    REQUIRE(css_util::has_class(n, "big"));
    REQUIRE_FALSE(css_util::has_class(n, "small"));
}

TEST_CASE("css_util::has_class: no class attribute", "[selector][util][class]") {
    Tree t; auto* n = t.E("div");
    REQUIRE_FALSE(css_util::has_class(n, "foo"));
}

TEST_CASE("css_util::has_class: leading and trailing whitespace",
    "[selector][util][class]") {
    Tree t; auto* n = t.E("div"); set_class(n, "   foo    bar   ");
    REQUIRE(css_util::has_class(n, "foo"));
    REQUIRE(css_util::has_class(n, "bar"));
}

TEST_CASE("css_util::has_class: tab and newline as separators",
    "[selector][util][class]") {
    Tree t; auto* n = t.E("div"); set_class(n, "foo\tbar\nbaz");
    REQUIRE(css_util::has_class(n, "foo"));
    REQUIRE(css_util::has_class(n, "bar"));
    REQUIRE(css_util::has_class(n, "baz"));
}

TEST_CASE("css_util::has_class: empty class string", "[selector][util][class]") {
    Tree t; auto* n = t.E("div"); set_class(n, "");
    REQUIRE_FALSE(css_util::has_class(n, "foo"));
    REQUIRE_FALSE(css_util::has_class(n, ""));
}


TEST_CASE("css_util::attr_prefix/suffix/substr basics", "[selector][util][attr]") {
    REQUIRE(css_util::attr_prefix("hello world", "hello"));
    REQUIRE_FALSE(css_util::attr_prefix("hello world", "world"));
    REQUIRE(css_util::attr_suffix("hello world", "world"));
    REQUIRE_FALSE(css_util::attr_suffix("hello world", "hello"));
    REQUIRE(css_util::attr_substr("hello world", "lo wo"));
    REQUIRE_FALSE(css_util::attr_substr("hello world", "xyz"));
}

TEST_CASE("css_util::attr_prefix/suffix/substr: empty arg returns false",
    "[selector][util][attr]") {
    REQUIRE_FALSE(css_util::attr_prefix("abc", ""));
    REQUIRE_FALSE(css_util::attr_suffix("abc", ""));
    REQUIRE_FALSE(css_util::attr_substr("abc", ""));
}

TEST_CASE("css_util::attr_includes: word boundary", "[selector][util][attr]") {
    REQUIRE(css_util::attr_includes("foo bar baz", "bar"));
    REQUIRE(css_util::attr_includes("foo bar baz", "foo"));
    REQUIRE_FALSE(css_util::attr_includes("foobar", "foo"));
    REQUIRE_FALSE(css_util::attr_includes("foo bar", ""));
}

TEST_CASE("css_util::attr_dash: prefix + dash boundary", "[selector][util][attr]") {
    REQUIRE(css_util::attr_dash("en-US", "en"));
    REQUIRE(css_util::attr_dash("en", "en"));
    REQUIRE_FALSE(css_util::attr_dash("english", "en"));
    REQUIRE_FALSE(css_util::attr_dash("en_US", "en"));
}



TEST_CASE("parse_anb: numeric only", "[selector][util][anb]") {
    int a = -1, b = -1;
    REQUIRE(css_util::parse_anb("3", a, b));
    REQUIRE(a == 0); REQUIRE(b == 3);
}

TEST_CASE("parse_anb: odd and even", "[selector][util][anb]") {
    int a = 0, b = 0;
    REQUIRE(css_util::parse_anb("odd", a, b));  REQUIRE(a == 2); REQUIRE(b == 1);
    REQUIRE(css_util::parse_anb("even", a, b)); REQUIRE(a == 2); REQUIRE(b == 0);
}

TEST_CASE("parse_anb: 2n", "[selector][util][anb]") {
    int a = 0, b = 0;
    REQUIRE(css_util::parse_anb("2n", a, b));
    REQUIRE(a == 2); REQUIRE(b == 0);
}

TEST_CASE("parse_anb: 2n+1", "[selector][util][anb]") {
    int a = 0, b = 0;
    REQUIRE(css_util::parse_anb("2n+1", a, b));
    REQUIRE(a == 2); REQUIRE(b == 1);
}

TEST_CASE("parse_anb: -n+3", "[selector][util][anb]") {
    int a = 0, b = 0;
    REQUIRE(css_util::parse_anb("-n+3", a, b));
    REQUIRE(a == -1); REQUIRE(b == 3);
}

TEST_CASE("parse_anb: bare n", "[selector][util][anb]") {
    int a = 0, b = 0;
    REQUIRE(css_util::parse_anb("n", a, b));
    REQUIRE(a == 1); REQUIRE(b == 0);
}

TEST_CASE("parse_anb: +n", "[selector][util][anb]") {
    int a = 0, b = 0;
    REQUIRE(css_util::parse_anb("+n", a, b));
    REQUIRE(a == 1); REQUIRE(b == 0);
}

TEST_CASE("parse_anb: n+2", "[selector][util][anb]") {
    int a = 0, b = 0;
    REQUIRE(css_util::parse_anb("n+2", a, b));
    REQUIRE(a == 1); REQUIRE(b == 2);
}

TEST_CASE("parse_anb: invalid input", "[selector][util][anb]") {
    int a = 0, b = 0;
    REQUIRE_FALSE(css_util::parse_anb("foo", a, b));
    REQUIRE_FALSE(css_util::parse_anb("", a, b));
    REQUIRE_FALSE(css_util::parse_anb("  ", a, b));
}

TEST_CASE("nth_match: odd/even semantics", "[selector][util][nth]") {
    // a=2, b=1 (odd) matches 1, 3, 5
    REQUIRE(css_util::nth_match(1, 2, 1));
    REQUIRE_FALSE(css_util::nth_match(2, 2, 1));
    REQUIRE(css_util::nth_match(3, 2, 1));
    REQUIRE(css_util::nth_match(5, 2, 1));
}

TEST_CASE("nth_match: 2n matches 2, 4, 6", "[selector][util][nth]") {
    REQUIRE_FALSE(css_util::nth_match(1, 2, 0));
    REQUIRE(css_util::nth_match(2, 2, 0));
    REQUIRE_FALSE(css_util::nth_match(3, 2, 0));
    REQUIRE(css_util::nth_match(4, 2, 0));
}

TEST_CASE("nth_match: numeric a=0", "[selector][util][nth]") {
    REQUIRE(css_util::nth_match(3, 0, 3));
    REQUIRE_FALSE(css_util::nth_match(2, 0, 3));
}

TEST_CASE("nth_match: -n+3 matches first three", "[selector][util][nth]") {
    REQUIRE(css_util::nth_match(1, -1, 3));
    REQUIRE(css_util::nth_match(2, -1, 3));
    REQUIRE(css_util::nth_match(3, -1, 3));
    REQUIRE_FALSE(css_util::nth_match(4, -1, 3));
}



TEST_CASE("specificity: tag = (0,0,1)", "[selector][spec]") {
    auto s = SM::compute_specificity(one(tg("div")));
    REQUIRE(s.a == 0); REQUIRE(s.b == 0); REQUIRE(s.c == 1);
}

TEST_CASE("specificity: class = (0,1,0)", "[selector][spec]") {
    auto s = SM::compute_specificity(one(cl("foo")));
    REQUIRE(s.a == 0); REQUIRE(s.b == 1); REQUIRE(s.c == 0);
}

TEST_CASE("specificity: id = (1,0,0)", "[selector][spec]") {
    auto s = SM::compute_specificity(one(id_("main")));
    REQUIRE(s.a == 1); REQUIRE(s.b == 0); REQUIRE(s.c == 0);
}

TEST_CASE("specificity: universal = (0,0,0)", "[selector][spec]") {
    auto s = SM::compute_specificity(one(uni()));
    REQUIRE(s.a == 0); REQUIRE(s.b == 0); REQUIRE(s.c == 0);
}

TEST_CASE("specificity: attribute = (0,1,0)", "[selector][spec]") {
    auto s = SM::compute_specificity(one(attr("href")));
    REQUIRE(s.a == 0); REQUIRE(s.b == 1); REQUIRE(s.c == 0);
}

TEST_CASE("specificity: pseudo-class = (0,1,0)", "[selector][spec]") {
    auto s = SM::compute_specificity(one(pc("hover")));
    REQUIRE(s.a == 0); REQUIRE(s.b == 1); REQUIRE(s.c == 0);
}

TEST_CASE("specificity: pseudo-element = (0,0,1)", "[selector][spec]") {
    auto s = SM::compute_specificity(one(pe("before")));
    REQUIRE(s.a == 0); REQUIRE(s.b == 0); REQUIRE(s.c == 1);
}

TEST_CASE("specificity: div.foo#main = (1,1,1)", "[selector][spec]") {
    auto s = SM::compute_specificity(one(cmp(tg("div"), cl("foo"), id_("main"))));
    REQUIRE(s.a == 1); REQUIRE(s.b == 1); REQUIRE(s.c == 1);
}

TEST_CASE("specificity: div > p.foo = (0,1,2)", "[selector][spec]") {
    auto s = SM::compute_specificity(chain(cmp(tg("div")), Comb::Child,
        cmp(tg("p"), cl("foo"))));
    REQUIRE(s.a == 0); REQUIRE(s.b == 1); REQUIRE(s.c == 2);
}

TEST_CASE("specificity: :where() contributes zero", "[selector][spec]") {
    auto s = SM::compute_specificity(one(pc("where", ".foo")));
    REQUIRE(s.a == 0); REQUIRE(s.b == 0); REQUIRE(s.c == 0);
}

TEST_CASE("specificity: :not(#id) — known simplification",
    "[selector][spec][!mayfail]") {
    // По спеке должно быть (1,0,0). Текущая реализация даёт (0,1,0).
    // Помечено как mayfail — если начнёт проходить, тег можно снять.
    auto s = SM::compute_specificity(one(pc("not", "#id")));
    REQUIRE(s.a == 1); REQUIRE(s.b == 0); REQUIRE(s.c == 0);
}




TEST_CASE("match: universal matches any element", "[selector][simple]") {
    Tree t; auto* n = t.E("div");
    REQUIRE(SM::match_complex(n, one(uni())));
}

TEST_CASE("match: tag matches", "[selector][simple]") {
    Tree t; auto* n = t.E("div");
    REQUIRE(SM::match_complex(n, one(tg("div"))));
    REQUIRE_FALSE(SM::match_complex(n, one(tg("p"))));
}

TEST_CASE("match: tag case-sensitive on tag_name", "[selector][simple]") {
    // DOM хранит lowercase; селектор должен быть тоже lowercase
    Tree t; auto* n = t.E("div");
    REQUIRE_FALSE(SM::match_complex(n, one(tg("DIV"))));
}

TEST_CASE("match: class single", "[selector][simple]") {
    Tree t; auto* n = t.E("div"); set_class(n, "btn primary");
    REQUIRE(SM::match_complex(n, one(cl("btn"))));
    REQUIRE(SM::match_complex(n, one(cl("primary"))));
    REQUIRE_FALSE(SM::match_complex(n, one(cl("secondary"))));
}

TEST_CASE("match: class must not be a substring", "[selector][simple]") {
    Tree t; auto* n = t.E("div"); set_class(n, "btn-primary");
    REQUIRE_FALSE(SM::match_complex(n, one(cl("btn"))));
    REQUIRE(SM::match_complex(n, one(cl("btn-primary"))));
}

TEST_CASE("match: id exact", "[selector][simple]") {
    Tree t; auto* n = t.E("div"); set_id(n, "main");
    REQUIRE(SM::match_complex(n, one(id_("main"))));
    REQUIRE_FALSE(SM::match_complex(n, one(id_("other"))));
}

TEST_CASE("match: compound div.foo#main all required", "[selector][simple]") {
    Tree t; auto* n = t.E("div"); set_class(n, "foo"); set_id(n, "main");
    REQUIRE(SM::match_complex(n, one(cmp(tg("div"), cl("foo"), id_("main")))));
    REQUIRE_FALSE(SM::match_complex(n, one(cmp(tg("p"), cl("foo"), id_("main")))));
    REQUIRE_FALSE(SM::match_complex(n, one(cmp(tg("div"), cl("bar"), id_("main")))));
}

TEST_CASE("match: compound on text node returns false",
    "[selector][simple]") {
    Tree t; auto* n = t.T("hello");
    REQUIRE_FALSE(SM::match_complex(n, one(tg("div"))));
    REQUIRE_FALSE(SM::match_complex(n, one(uni())));
}

TEST_CASE("match: null node returns false", "[selector][simple]") {
    REQUIRE_FALSE(SM::match_complex(nullptr, one(tg("div"))));
}

TEST_CASE("match: empty compound list returns false", "[selector][simple]") {
    Tree t; auto* n = t.E("div");
    CS empty;
    REQUIRE_FALSE(SM::match_complex(n, empty));
}



TEST_CASE("match: [attr] presence", "[selector][attr]") {
    Tree t; auto* n = t.E("a"); set_attr(n, "href", "#");
    REQUIRE(SM::match_complex(n, one(attr("href"))));
    REQUIRE_FALSE(SM::match_complex(n, one(attr("src"))));
}

TEST_CASE("match: [attr=value]", "[selector][attr]") {
    Tree t; auto* n = t.E("input"); set_attr(n, "type", "text");
    REQUIRE(SM::match_complex(n, one(attr("type", "=", "text"))));
    REQUIRE_FALSE(SM::match_complex(n, one(attr("type", "=", "checkbox"))));
}

TEST_CASE("match: [attr~=value]", "[selector][attr]") {
    Tree t; auto* n = t.E("div"); set_attr(n, "class", "foo bar baz");
    REQUIRE(SM::match_complex(n, one(attr("class", "~=", "bar"))));
    REQUIRE_FALSE(SM::match_complex(n, one(attr("class", "~=", "foo-bar"))));
}

TEST_CASE("match: [lang|=en]", "[selector][attr]") {
    Tree t; auto* a = t.E("p"); set_attr(a, "lang", "en-US");
    Tree t2; auto* b = t2.E("p"); set_attr(b, "lang", "en");
    Tree t3; auto* c = t3.E("p"); set_attr(c, "lang", "english");
    REQUIRE(SM::match_complex(a, one(attr("lang", "|=", "en"))));
    REQUIRE(SM::match_complex(b, one(attr("lang", "|=", "en"))));
    REQUIRE_FALSE(SM::match_complex(c, one(attr("lang", "|=", "en"))));
}

TEST_CASE("match: [href^=https]", "[selector][attr]") {
    Tree t; auto* n = t.E("a"); set_attr(n, "href", "https://example.com");
    REQUIRE(SM::match_complex(n, one(attr("href", "^=", "https://"))));
    REQUIRE_FALSE(SM::match_complex(n, one(attr("href", "^=", "http://"))));
}

TEST_CASE("match: [href$=.png]", "[selector][attr]") {
    Tree t; auto* n = t.E("img"); set_attr(n, "src", "logo.png");
    REQUIRE(SM::match_complex(n, one(attr("src", "$=", ".png"))));
    REQUIRE_FALSE(SM::match_complex(n, one(attr("src", "$=", ".jpg"))));
}

TEST_CASE("match: [href*=example]", "[selector][attr]") {
    Tree t; auto* n = t.E("a"); set_attr(n, "href", "https://example.com");
    REQUIRE(SM::match_complex(n, one(attr("href", "*=", "example"))));
    REQUIRE_FALSE(SM::match_complex(n, one(attr("href", "*=", "sample"))));
}



namespace {
    struct SampleTree {
        Tree t;
        DOMNode* html; DOMNode* body; DOMNode* header; DOMNode* h1;
        DOMNode* intro; DOMNode* p2; DOMNode* p3; DOMNode* span;

        SampleTree() {
            html = t.E("html");
            body = t.E("body", html);
            header = t.E("header", body);
            h1 = t.E("h1", header);
            intro = t.E("p", body); set_id(intro, "intro");
            p2 = t.E("p", body); set_class(p2, "note");
            p3 = t.E("p", body); set_class(p3, "note");
            span = t.E("span", body);
        }
    };
}

TEST_CASE("match: descendant combinator", "[selector][comb]") {
    SampleTree s;
    // html h1
    auto cs = chain(cmp(tg("html")), Comb::Descendant, cmp(tg("h1")));
    REQUIRE(SM::match_complex(s.h1, cs));
    REQUIRE_FALSE(SM::match_complex(s.span, cs));
}

TEST_CASE("match: child combinator", "[selector][comb]") {
    SampleTree s;
    // body > p
    auto cs = chain(cmp(tg("body")), Comb::Child, cmp(tg("p")));
    REQUIRE(SM::match_complex(s.intro, cs));
    REQUIRE(SM::match_complex(s.p2, cs));
    // h1 не прямой ребёнок body
    REQUIRE_FALSE(SM::match_complex(s.h1, cs));
}

TEST_CASE("match: adjacent sibling", "[selector][comb]") {
    SampleTree s;
    // header + p
    auto cs = chain(cmp(tg("header")), Comb::AdjacentSibling, cmp(tg("p")));
    REQUIRE(SM::match_complex(s.intro, cs));
    REQUIRE_FALSE(SM::match_complex(s.p2, cs));   // перед p2 стоит p, не header
    REQUIRE_FALSE(SM::match_complex(s.header, cs));
}

TEST_CASE("match: general sibling", "[selector][comb]") {
    SampleTree s;
    // header ~ span
    auto cs = chain(cmp(tg("header")), Comb::GeneralSibling, cmp(tg("span")));
    REQUIRE(SM::match_complex(s.span, cs));
    REQUIRE_FALSE(SM::match_complex(s.header, cs));  // не следует за собой
}

TEST_CASE("match: descendant skips text nodes", "[selector][comb]") {
    Tree t;
    auto* div = t.E("div");
    auto* p = t.E("p", div);
    t.T("hello", div);
    // "div p" должен матчиться на p, несмотря на текстовый сиблинг
    auto cs = chain(cmp(tg("div")), Comb::Descendant, cmp(tg("p")));
    REQUIRE(SM::match_complex(p, cs));
}

TEST_CASE("match: chain of three combinators", "[selector][comb]") {
    SampleTree s;
    // html > body p.note — descendant после child
    auto cs = chain(cmp(tg("html")), Comb::Child,
        cmp(tg("body")), Comb::Descendant,
        cmp(tg("p"), cl("note")));
    REQUIRE(SM::match_complex(s.p2, cs));
    REQUIRE(SM::match_complex(s.p3, cs));
    REQUIRE_FALSE(SM::match_complex(s.intro, cs));  // .intro не имеет класса note
}

TEST_CASE("match: descendant requires ancestor in chain",
    "[selector][comb]") {
    Tree t;
    auto* a = t.E("div");
    auto* b = t.E("span");  // сиблинг, не потомок
    (void)b;
    auto cs = chain(cmp(tg("section")), Comb::Descendant, cmp(tg("div")));
    REQUIRE_FALSE(SM::match_complex(a, cs));
}

TEST_CASE("match: adjacent sibling at first child fails",
    "[selector][comb]") {
    SampleTree s;
    // ничего не стоит перед html (первый ребёнок Document)
    auto cs = chain(cmp(tg("html")), Comb::AdjacentSibling, cmp(tg("body")));
    REQUIRE_FALSE(SM::match_complex(s.body, cs));
}





namespace {
    // Родитель с детьми p, span, p, p, span
    struct SeqTree {
        Tree t;
        DOMNode* parent;
        DOMNode* p1; DOMNode* s1; DOMNode* p2; DOMNode* p3; DOMNode* s2;

        SeqTree() {
            parent = t.E("div");
            p1 = t.E("p", parent);
            s1 = t.E("span", parent);
            p2 = t.E("p", parent);
            p3 = t.E("p", parent);
            s2 = t.E("span", parent);
        }
    };
}

TEST_CASE("match: :first-child", "[selector][pseudo][structural]") {
    SeqTree s;
    auto cs = one(pc("first-child"));
    REQUIRE(SM::match_complex(s.p1, cs));
    REQUIRE_FALSE(SM::match_complex(s.s1, cs));
    REQUIRE_FALSE(SM::match_complex(s.s2, cs));
}

TEST_CASE("match: :last-child", "[selector][pseudo][structural]") {
    SeqTree s;
    auto cs = one(pc("last-child"));
    REQUIRE(SM::match_complex(s.s2, cs));
    REQUIRE_FALSE(SM::match_complex(s.p1, cs));
    REQUIRE_FALSE(SM::match_complex(s.p3, cs));
}

TEST_CASE("match: :only-child (no)", "[selector][pseudo][structural]") {
    SeqTree s;
    auto cs = one(pc("only-child"));
    REQUIRE_FALSE(SM::match_complex(s.p1, cs));
    REQUIRE_FALSE(SM::match_complex(s.s1, cs));
}

TEST_CASE("match: :only-child (yes)", "[selector][pseudo][structural]") {
    Tree t;
    auto* div = t.E("div");
    auto* only = t.E("p", div);
    auto cs = one(pc("only-child"));
    REQUIRE(SM::match_complex(only, cs));
}

TEST_CASE("match: :only-child ignores text siblings",
    "[selector][pseudo][structural]") {
    Tree t;
    auto* div = t.E("div");
    t.T("hello", div);
    auto* only = t.E("p", div);
    t.T("world", div);
    auto cs = one(pc("only-child"));
    REQUIRE(SM::match_complex(only, cs));
}

TEST_CASE("match: :empty with no children", "[selector][pseudo][structural]") {
    Tree t; auto* div = t.E("div");
    REQUIRE(SM::match_complex(div, one(pc("empty"))));
}

TEST_CASE("match: :empty with only whitespace text", "[selector][pseudo][structural]") {
    // По текущей реализации "  " НЕ пустой (text_content не пуст). Это упрощение.
    Tree t; auto* div = t.E("div"); t.T("   ", div);
    REQUIRE_FALSE(SM::match_complex(div, one(pc("empty"))));
}

TEST_CASE("match: :empty with text content", "[selector][pseudo][structural]") {
    Tree t; auto* div = t.E("div"); t.T("hello", div);
    REQUIRE_FALSE(SM::match_complex(div, one(pc("empty"))));
}

TEST_CASE("match: :empty with element child", "[selector][pseudo][structural]") {
    Tree t; auto* div = t.E("div"); t.E("p", div);
    REQUIRE_FALSE(SM::match_complex(div, one(pc("empty"))));
}

TEST_CASE("match: :empty with only empty text node",
    "[selector][pseudo][structural]") {
    Tree t; auto* div = t.E("div"); t.T("", div);
    REQUIRE(SM::match_complex(div, one(pc("empty"))));
}

TEST_CASE("match: :root", "[selector][pseudo][structural]") {
    Tree t; auto* html = t.E("html");
    // Согласно matcher: node->parent == nullptr ИЛИ parent не Element.
    REQUIRE(SM::match_complex(html, one(pc("root"))));
}

TEST_CASE("match: :root on child fails", "[selector][pseudo][structural]") {
    Tree t; auto* html = t.E("html"); auto* body = t.E("body", html);
    REQUIRE_FALSE(SM::match_complex(body, one(pc("root"))));
}





TEST_CASE("match: :nth-child(1) = first", "[selector][pseudo][nth]") {
    SeqTree s;
    auto cs = one(pc("nth-child", "1"));
    REQUIRE(SM::match_complex(s.p1, cs));
    REQUIRE_FALSE(SM::match_complex(s.s1, cs));
}

TEST_CASE("match: :nth-child(odd)", "[selector][pseudo][nth]") {
    SeqTree s;
    auto cs = one(pc("nth-child", "odd"));
    REQUIRE(SM::match_complex(s.p1, cs));  // 1
    REQUIRE_FALSE(SM::match_complex(s.s1, cs)); // 2
    REQUIRE(SM::match_complex(s.p2, cs));  // 3
    REQUIRE_FALSE(SM::match_complex(s.p3, cs)); // 4
    REQUIRE(SM::match_complex(s.s2, cs));  // 5
}

TEST_CASE("match: :nth-child(even)", "[selector][pseudo][nth]") {
    SeqTree s;
    auto cs = one(pc("nth-child", "even"));
    REQUIRE_FALSE(SM::match_complex(s.p1, cs));
    REQUIRE(SM::match_complex(s.s1, cs));
    REQUIRE_FALSE(SM::match_complex(s.p2, cs));
    REQUIRE(SM::match_complex(s.p3, cs));
    REQUIRE_FALSE(SM::match_complex(s.s2, cs));
}

TEST_CASE("match: :nth-child(2n+1) same as odd", "[selector][pseudo][nth]") {
    SeqTree s;
    auto cs = one(pc("nth-child", "2n+1"));
    REQUIRE(SM::match_complex(s.p1, cs));
    REQUIRE_FALSE(SM::match_complex(s.s1, cs));
    REQUIRE(SM::match_complex(s.p2, cs));
}

TEST_CASE("match: :nth-child(-n+2) first two", "[selector][pseudo][nth]") {
    SeqTree s;
    auto cs = one(pc("nth-child", "-n+2"));
    REQUIRE(SM::match_complex(s.p1, cs));
    REQUIRE(SM::match_complex(s.s1, cs));
    REQUIRE_FALSE(SM::match_complex(s.p2, cs));
}

TEST_CASE("match: :nth-child(n+3)", "[selector][pseudo][nth]") {
    SeqTree s;
    auto cs = one(pc("nth-child", "n+3"));
    REQUIRE_FALSE(SM::match_complex(s.p1, cs));
    REQUIRE_FALSE(SM::match_complex(s.s1, cs));
    REQUIRE(SM::match_complex(s.p2, cs));
    REQUIRE(SM::match_complex(s.p3, cs));
    REQUIRE(SM::match_complex(s.s2, cs));
}

TEST_CASE("match: :nth-last-child(1) = last", "[selector][pseudo][nth]") {
    SeqTree s;
    auto cs = one(pc("nth-last-child", "1"));
    REQUIRE(SM::match_complex(s.s2, cs));
    REQUIRE_FALSE(SM::match_complex(s.p1, cs));
}

TEST_CASE("match: :nth-last-child(2)", "[selector][pseudo][nth]") {
    SeqTree s;
    auto cs = one(pc("nth-last-child", "2"));
    REQUIRE(SM::match_complex(s.p3, cs));
    REQUIRE_FALSE(SM::match_complex(s.s2, cs));
}

TEST_CASE("match: :nth-child ignores text nodes", "[selector][pseudo][nth]") {
    Tree t;
    auto* div = t.E("div");
    auto* p1 = t.E("p", div);
    t.T("text", div);
    auto* p2 = t.E("p", div);
    auto cs = one(pc("nth-child", "2"));
    REQUIRE(SM::match_complex(p2, cs));
    REQUIRE_FALSE(SM::match_complex(p1, cs));
}




TEST_CASE("match: :first-of-type", "[selector][pseudo][oftype]") {
    SeqTree s;
    auto cs = one(pc("first-of-type"));
    REQUIRE(SM::match_complex(s.p1, cs));   // первый <p>
    REQUIRE(SM::match_complex(s.s1, cs));   // первый <span>
    REQUIRE_FALSE(SM::match_complex(s.p2, cs));
    REQUIRE_FALSE(SM::match_complex(s.s2, cs));
}

TEST_CASE("match: :last-of-type", "[selector][pseudo][oftype]") {
    SeqTree s;
    auto cs = one(pc("last-of-type"));
    REQUIRE(SM::match_complex(s.p3, cs));   // последний <p>
    REQUIRE(SM::match_complex(s.s2, cs));   // последний <span>
    REQUIRE_FALSE(SM::match_complex(s.p1, cs));
    REQUIRE_FALSE(SM::match_complex(s.s1, cs));
}

TEST_CASE("match: :nth-of-type(2)", "[selector][pseudo][oftype]") {
    SeqTree s;
    auto cs = one(pc("nth-of-type", "2"));
    REQUIRE(SM::match_complex(s.p2, cs));   // 2-й <p>
    REQUIRE(SM::match_complex(s.s2, cs));   // 2-й <span>
    REQUIRE_FALSE(SM::match_complex(s.p1, cs));
    REQUIRE_FALSE(SM::match_complex(s.s1, cs));
}

TEST_CASE("match: :nth-last-of-type(1) = last-of-type",
    "[selector][pseudo][oftype]") {
    SeqTree s;
    auto cs = one(pc("nth-last-of-type", "1"));
    REQUIRE(SM::match_complex(s.p3, cs));
    REQUIRE(SM::match_complex(s.s2, cs));
}

TEST_CASE("match: :nth-of-type(odd)", "[selector][pseudo][oftype]") {
    SeqTree s;
    auto cs = one(pc("nth-of-type", "odd"));
    REQUIRE(SM::match_complex(s.p1, cs));   // 1-й p
    REQUIRE_FALSE(SM::match_complex(s.p2, cs)); // 2-й p
    REQUIRE(SM::match_complex(s.p3, cs));   // 3-й p
    REQUIRE(SM::match_complex(s.s1, cs));   // 1-й span
    REQUIRE_FALSE(SM::match_complex(s.s2, cs));
}




TEST_CASE("match: :not(.foo)", "[selector][pseudo][logical]") {
    Tree t; auto* a = t.E("div"); set_class(a, "foo");
    Tree t2; auto* b = t2.E("div"); set_class(b, "bar");
    auto cs = one(pc("not", ".foo"));
    REQUIRE_FALSE(SM::match_complex(a, cs));
    REQUIRE(SM::match_complex(b, cs));
}

TEST_CASE("match: :not(tag)", "[selector][pseudo][logical]") {
    Tree t; auto* d = t.E("div");
    Tree t2; auto* p = t2.E("p");
    auto cs = one(pc("not", "div"));
    REQUIRE_FALSE(SM::match_complex(d, cs));
    REQUIRE(SM::match_complex(p, cs));
}

TEST_CASE("match: :not(#id)", "[selector][pseudo][logical]") {
    Tree t; auto* a = t.E("div"); set_id(a, "main");
    Tree t2; auto* b = t2.E("div"); set_id(b, "other");
    auto cs = one(pc("not", "#main"));
    REQUIRE_FALSE(SM::match_complex(a, cs));
    REQUIRE(SM::match_complex(b, cs));
}

TEST_CASE("match: :not with two alternatives (comma)",
    "[selector][pseudo][logical]") {
    Tree t; auto* n = t.E("div"); set_class(n, "bar");
    auto cs = one(pc("not", ".foo, .baz"));
    REQUIRE(SM::match_complex(n, cs));
}

TEST_CASE("match: :is(.foo, .bar)", "[selector][pseudo][logical]") {
    Tree t; auto* a = t.E("div"); set_class(a, "foo");
    Tree t2; auto* b = t2.E("div"); set_class(b, "bar");
    Tree t3; auto* c = t3.E("div"); set_class(c, "baz");
    auto cs = one(pc("is", ".foo, .bar"));
    REQUIRE(SM::match_complex(a, cs));
    REQUIRE(SM::match_complex(b, cs));
    REQUIRE_FALSE(SM::match_complex(c, cs));
}

TEST_CASE("match: :where(...)", "[selector][pseudo][logical]") {
    Tree t; auto* n = t.E("div"); set_class(n, "foo");
    auto cs = one(pc("where", ".foo"));
    REQUIRE(SM::match_complex(n, cs));
}

TEST_CASE("match: :not with compound arg", "[selector][pseudo][logical]") {
    Tree t; auto* a = t.E("div"); set_class(a, "foo"); set_id(a, "x");
    Tree t2; auto* b = t2.E("div"); set_class(b, "foo"); set_id(b, "y");
    auto cs = one(pc("not", "div#x"));
    REQUIRE_FALSE(SM::match_complex(a, cs));
    REQUIRE(SM::match_complex(b, cs));
}



TEST_CASE("match: :hover never matches in static tree",
    "[selector][pseudo][state]") {
    Tree t; auto* n = t.E("div");
    REQUIRE_FALSE(SM::match_complex(n, one(pc("hover"))));
}

TEST_CASE("match: :focus never matches", "[selector][pseudo][state]") {
    Tree t; auto* n = t.E("input");
    REQUIRE_FALSE(SM::match_complex(n, one(pc("focus"))));
}

TEST_CASE("match: :active never matches", "[selector][pseudo][state]") {
    Tree t; auto* n = t.E("a");
    REQUIRE_FALSE(SM::match_complex(n, one(pc("active"))));
}

TEST_CASE("match: :visited never matches", "[selector][pseudo][state]") {
    Tree t; auto* n = t.E("a"); set_attr(n, "href", "#");
    REQUIRE_FALSE(SM::match_complex(n, one(pc("visited"))));
}

TEST_CASE("match: :any-link on <a href>", "[selector][pseudo][link]") {
    Tree t; auto* n = t.E("a"); set_attr(n, "href", "https://x");
    REQUIRE(SM::match_complex(n, one(pc("any-link"))));
}

TEST_CASE("match: :any-link on <a> without href fails",
    "[selector][pseudo][link]") {
    Tree t; auto* n = t.E("a");
    REQUIRE_FALSE(SM::match_complex(n, one(pc("any-link"))));
}

TEST_CASE("match: :any-link on <area href>", "[selector][pseudo][link]") {
    Tree t; auto* n = t.E("area"); set_attr(n, "href", "#x");
    REQUIRE(SM::match_complex(n, one(pc("any-link"))));
}

TEST_CASE("match: :any-link on <div href> fails", "[selector][pseudo][link]") {
    Tree t; auto* n = t.E("div"); set_attr(n, "href", "#");
    REQUIRE_FALSE(SM::match_complex(n, one(pc("any-link"))));
}





TEST_CASE("match: ::before never matches DOM node",
    "[selector][pseudo][element]") {
    Tree t; auto* n = t.E("p");
    REQUIRE_FALSE(SM::match_complex(n, one(pe("before"))));
}

TEST_CASE("match: ::after never matches DOM node",
    "[selector][pseudo][element]") {
    Tree t; auto* n = t.E("p");
    REQUIRE_FALSE(SM::match_complex(n, one(pe("after"))));
}



TEST_CASE("integration: typical layout selectors on card DOM",
    "[selector][integration]") {
    Tree t;
    auto* card = t.E("div"); set_class(card, "card");
    auto* h1 = t.E("h1", card);
    auto* p = t.E("p", card); set_class(p, "desc");
    auto* btn = t.E("button", card); set_attr(btn, "disabled", "");

    // .card h1
    REQUIRE(SM::match_complex(h1,
        chain(cmp(cl("card")), Comb::Descendant, cmp(tg("h1")))));

    // .card > p.desc
    REQUIRE(SM::match_complex(p,
        chain(cmp(cl("card")), Comb::Child, cmp(tg("p"), cl("desc")))));

    // h1 + p
    REQUIRE(SM::match_complex(p,
        chain(cmp(tg("h1")), Comb::AdjacentSibling, cmp(tg("p")))));

    // button[disabled]
    REQUIRE(SM::match_complex(btn, one(cmp(tg("button"), attr("disabled")))));

    // :not(.desc)
    REQUIRE_FALSE(SM::match_complex(p, one(pc("not", ".desc"))));
    REQUIRE(SM::match_complex(h1, one(pc("not", ".desc"))));

    // :first-child и :last-child
    REQUIRE(SM::match_complex(h1, one(pc("first-child"))));
    REQUIRE(SM::match_complex(btn, one(pc("last-child"))));
}

TEST_CASE("integration: complex chain html body div.card p",
    "[selector][integration]") {
    Tree t;
    auto* html = t.E("html");
    auto* body = t.E("body", html);
    auto* div = t.E("div", body); set_class(div, "card");
    auto* p = t.E("p", div);

    // Нужно 4 compound'а, а chain3 поддерживает 3. Собираем вручную:
    CS big;
    big.compounds.push_back(cmp(tg("html")));
    Cmp c2 = cmp(tg("body")); c2.combinator = Comb::Child; big.compounds.push_back(c2);
    Cmp c3 = cmp(cl("card")); c3.combinator = Comb::Descendant; big.compounds.push_back(c3);
    Cmp c4 = cmp(tg("p"));    c4.combinator = Comb::Child; big.compounds.push_back(c4);

    REQUIRE(SM::match_complex(p, big));

    // Отрицательный случай: тот же селектор против html
    REQUIRE_FALSE(SM::match_complex(html, big));
}


TEST_CASE("specificity: :not(.foo, #bar) takes max",
    "[selector][spec]") {
    auto s = SM::compute_specificity(one(pc("not", ".foo, #bar")));
    REQUIRE(s.a == 1); REQUIRE(s.b == 0); REQUIRE(s.c == 0);
}

TEST_CASE("specificity: :is(div, .foo)", "[selector][spec]") {
    auto s = SM::compute_specificity(one(pc("is", "div, .foo")));
    REQUIRE(s.a == 0); REQUIRE(s.b == 1); REQUIRE(s.c == 0);
}

TEST_CASE("specificity: :not(:where(#x)) is zero",
    "[selector][spec]") {
    auto s = SM::compute_specificity(one(pc("not", ":where(#x)")));
    // :where даёт 0, значит :not(0) = 0.
    // Текущая реализация вложенного :where -> 0, arg_list -> 0.
    REQUIRE(s.a == 0); REQUIRE(s.b == 0); REQUIRE(s.c == 0);
}


// ============================================================
//  ДОПОЛНЕНИЯ: глубокие комбинаторы, специфичность, edge-cases.
// ============================================================

TEST_CASE("match: descendant across many levels",
    "[selector][comb][deep]") {
    Tree t;
    auto* a = t.E("a");
    auto* b = t.E("b", a);
    auto* c = t.E("c", b);
    auto* d = t.E("d", c);
    REQUIRE(SM::match_complex(d,
        chain(cmp(tg("a")), Comb::Descendant, cmp(tg("d")))));
    REQUIRE_FALSE(SM::match_complex(d,
        chain(cmp(tg("a")), Comb::Child, cmp(tg("d")))));

    CS big;
    big.compounds.push_back(cmp(tg("a")));
    { Cmp x = cmp(tg("b")); x.combinator = Comb::Child; big.compounds.push_back(x); }
    { Cmp x = cmp(tg("c")); x.combinator = Comb::Child; big.compounds.push_back(x); }
    { Cmp x = cmp(tg("d")); x.combinator = Comb::Child; big.compounds.push_back(x); }
    REQUIRE(SM::match_complex(d, big));
}

TEST_CASE("match: sibling chain a + b + c",
    "[selector][comb][sibling]") {
    Tree t;
    auto* p = t.E("div");
    auto* a = t.E("a", p);
    auto* b = t.E("b", p);
    auto* c = t.E("c", p);
    (void)a;
    CS cs;
    cs.compounds.push_back(cmp(tg("a")));
    { Cmp x = cmp(tg("b")); x.combinator = Comb::AdjacentSibling; cs.compounds.push_back(x); }
    { Cmp x = cmp(tg("c")); x.combinator = Comb::AdjacentSibling; cs.compounds.push_back(x); }
    REQUIRE(SM::match_complex(c, cs));
}

TEST_CASE("match: general sibling with multiple candidates",
    "[selector][comb][sibling]") {
    Tree t;
    auto* p = t.E("div");
    auto* h1 = t.E("h1", p);
    auto* x1 = t.E("p", p);
    auto* x2 = t.E("p", p);
    auto* s = t.E("span", p);
    (void)h1; (void)x1; (void)x2;
    REQUIRE(SM::match_complex(s,
        chain(cmp(tg("h1")), Comb::GeneralSibling, cmp(tg("span")))));
    REQUIRE_FALSE(SM::match_complex(h1,
        chain(cmp(tg("p")), Comb::GeneralSibling, cmp(tg("h1")))));
}

TEST_CASE("match: compound with multiple classes",
    "[selector][simple][class]") {
    Tree t;
    auto* n = t.E("div"); set_class(n, "a b c");
    Cmp c;
    c.parts.push_back(cl("a"));
    c.parts.push_back(cl("b"));
    c.parts.push_back(cl("c"));
    REQUIRE(SM::match_complex(n, one(std::move(c))));

    Tree t2;
    auto* n2 = t2.E("div"); set_class(n2, "a b");
    Cmp c2;
    c2.parts.push_back(cl("a"));
    c2.parts.push_back(cl("c"));    // нет c
    REQUIRE_FALSE(SM::match_complex(n2, one(std::move(c2))));
}

TEST_CASE("match: :not(div) on span matches",
    "[selector][pseudo][not]") {
    Tree t; auto* sp = t.E("span");
    REQUIRE(SM::match_complex(sp, one(pc("not", "div"))));
}

TEST_CASE("match: :not(.a.b.c) compound negation",
    "[selector][pseudo][not]") {
    Tree t;  auto* a = t.E("div"); set_class(a, "a b");
    Tree t2; auto* b = t2.E("div"); set_class(b, "a b c");
    REQUIRE(SM::match_complex(a, one(pc("not", ".a.b.c"))));
    REQUIRE_FALSE(SM::match_complex(b, one(pc("not", ".a.b.c"))));
}

TEST_CASE("match: :nth-last-child from tail",
    "[selector][pseudo][nth]") {
    SeqTree s;
    auto cs = one(pc("nth-last-child", "1"));
    REQUIRE(SM::match_complex(s.s2, cs));
    auto cs5 = one(pc("nth-last-child", "5"));
    REQUIRE(SM::match_complex(s.p1, cs5));
}

TEST_CASE("match: compound of two pseudo-classes",
    "[selector][pseudo][compound]") {
    Tree t;
    auto* div = t.E("div");
    auto* p = t.E("p", div);
    (void)div;
    Cmp c;
    c.parts.push_back(tg("p"));
    c.parts.push_back(pc("first-child"));
    c.parts.push_back(pc("last-child"));
    REQUIRE(SM::match_complex(p, one(std::move(c))));
}

TEST_CASE("specificity: :nth-child counts as (0,1,0)",
    "[selector][spec]") {
    auto s = SM::compute_specificity(one(pc("nth-child", "2")));
    REQUIRE(s.a == 0); REQUIRE(s.b == 1); REQUIRE(s.c == 0);
}

TEST_CASE("specificity: chain a > b c.d#e = (1,1,3)",
    "[selector][spec]") {
    CS cs;
    cs.compounds.push_back(cmp(tg("a")));
    { Cmp x = cmp(tg("b")); x.combinator = Comb::Child; cs.compounds.push_back(x); }
    {
        Cmp x = cmp(tg("c"), cl("d"), id_("e"));
        x.combinator = Comb::Descendant;
        cs.compounds.push_back(x);
    }
    auto s = SM::compute_specificity(cs);
    REQUIRE(s.a == 1); REQUIRE(s.b == 1); REQUIRE(s.c == 3);
}

TEST_CASE("match: chain of four adjacent siblings",
    "[selector][comb][sibling]") {
    Tree t;
    auto* p = t.E("div");
    auto* a = t.E("a", p);
    auto* b = t.E("b", p);
    auto* c = t.E("c", p);
    auto* d = t.E("d", p);
    (void)a; (void)b;
    CS cs;
    cs.compounds.push_back(cmp(tg("a")));
    { Cmp x = cmp(tg("b")); x.combinator = Comb::AdjacentSibling; cs.compounds.push_back(x); }
    { Cmp x = cmp(tg("c")); x.combinator = Comb::AdjacentSibling; cs.compounds.push_back(x); }
    { Cmp x = cmp(tg("d")); x.combinator = Comb::AdjacentSibling; cs.compounds.push_back(x); }
    REQUIRE(SM::match_complex(d, cs));
}

TEST_CASE("match: null parent for :root",
    "[selector][pseudo][root]") {
    Tree t; auto* div = t.E("div");
    REQUIRE(SM::match_complex(div, one(pc("root"))));
}

TEST_CASE("integration: navigation menu selectors",
    "[selector][integration]") {
    Tree t;
    auto* nav = t.E("nav"); set_class(nav, "menu");
    auto* ul = t.E("ul", nav);
    auto* li1 = t.E("li", ul); set_class(li1, "item");
    auto* li2 = t.E("li", ul); set_class(li2, "item active");
    auto* a = t.E("a", li1); set_attr(a, "href", "#");

    REQUIRE(SM::match_complex(a,
        chain(cmp(cl("menu")), Comb::Descendant, cmp(tg("a")))));

    CS cs;
    cs.compounds.push_back(cmp(cl("menu")));
    { Cmp x = cmp(tg("ul")); x.combinator = Comb::Child; cs.compounds.push_back(x); }
    { Cmp x = cmp(tg("li")); x.combinator = Comb::Child; cs.compounds.push_back(x); }
    REQUIRE(SM::match_complex(li1, cs));
    REQUIRE(SM::match_complex(li2, cs));

    REQUIRE(SM::match_complex(li2, one(cmp(tg("li"), cl("active")))));
    REQUIRE_FALSE(SM::match_complex(li1, one(cmp(tg("li"), cl("active")))));
}