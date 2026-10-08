#include <cstdint>
#include <cstddef>
#include <string>

#include "parsers/css_parser/headers/css_parser.h"
#include "parsers/selector_matcher/selector_matcher.h"
#include "parsers/html_parser/headers/dom.h"
#include "parsers/arena_memory_allocator/headers/arena.h"

namespace {
    // Собираем фиксированный DOM: <div class="a"><p id="x"><span/></p></div>
    DOMNode* buildFixedDom(ArenaAllocator& arena) {
        auto mk = [&](NodeType t, const char* tag) {
            DOMNode* n = arena.Alloc<DOMNode>();
            n->type = t;
            if (tag) n->tag_name = tag;
            return n;
            };
        DOMNode* div = mk(NodeType::Element, "div");
        div->attributes["class"] = "a b";
        DOMNode* p = mk(NodeType::Element, "p");
        p->attributes["id"] = "x";
        div->add_child(p);
        DOMNode* span = mk(NodeType::Element, "span");
        p->add_child(span);
        return div;
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 4 * 1024) return 0;

    ArenaAllocator arena;
    DOMNode* dom = buildFixedDom(arena);

    // Парсим селектор из входа
    std::string sel(reinterpret_cast<const char*>(data), size);
    CSSParser css(arena);
    StyleSheet* sheet = css.parse(sel + " {}");   // фиктивное правило

    if (!sheet || sheet->rules.empty()) return 0;
    if (sheet->rules[0].selectors.empty()) return 0;

    const ComplexSelector& cs = sheet->rules[0].selectors[0];

    // Прогоняем матчер по всем узлам DOM. Здесь чаще всего и падает.
    DOMNode* stack[16];
    int sp = 0;
    stack[sp++] = dom;
    while (sp > 0) {
        DOMNode* n = stack[--sp];
        if (n->type == NodeType::Element) {
            SelectorMatcher::match_complex(n, cs);
        }
        for (DOMNode* c : n->children) {
            if (sp < 16) stack[sp++] = c;
        }
    }
    return 0;
}