#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "parsers/arena_memory_allocator/headers/arena.h"
#include "parsers/html_parser/headers/html_parser.h"
#include "parsers/css_parser/headers/css_parser.h"
#include "parsers/selector_matcher/style_tree_builder.h"
#include "parsers/selector_matcher/style_origin.h"
#include "parsers/selector_matcher/display_list_builder.h"
#include "layout/headers/layout_tree_builder.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0 || size > 1 << 20) return 0;

    const void* sep = std::memchr(data, '\0', size);
    if (!sep) return 0;
    const size_t css_len = static_cast<const uint8_t*>(sep) - data;
    const size_t html_off = css_len + 1;
    if (html_off >= size) return 0;

    std::string css(reinterpret_cast<const char*>(data), css_len);
    std::string html(reinterpret_cast<const char*>(data + html_off), size - html_off);

    ArenaAllocator arena;

    HTMLParser hp(arena);
    DOMNode* doc = hp.parse(html);
    if (!doc) return 0;

    CSSParser cp(arena);
    StyleSheet* sheet = cp.parse(css);

    std::vector<StyleRuleIndex::SheetRef> sources = {
        { sheet, Origin::Author }
    };

    StyleStorageSoA storage = StyleTreeBuilder::build(doc, sources);
    if (storage.size() == 0) return 0;

    auto layout = LayoutTreeBuilder::build(storage);
    if (!layout) return 0;

    LayoutTreeBuilder::Viewport vp{ /*width*/ 1024.0f, /*height*/ 768.0f };
    LayoutTreeBuilder::compute_layout(layout.get(), vp, storage, {});

    auto dl = DisplayListBuilder::build(layout.get(), storage);
    (void)dl;

    return 0;
}