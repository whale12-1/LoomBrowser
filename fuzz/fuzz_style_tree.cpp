#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "parsers/arena_memory_allocator/headers/arena.h"
#include "parsers/html_parser/headers/html_parser.h"
#include "parsers/css_parser/headers/css_parser.h"
#include "parsers/selector_matcher/style_tree_builder.h"
#include "parsers/selector_matcher/style_origin.h"

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
    // Доступ ко всем полям SoA форсирует чтение — если что-то вышло за границы,
    // ASan поймает это здесь.
    volatile float sink = 0;
    for (uint32_t i = 0; i < storage.size(); ++i)
        sink += storage.font_sizes[i];
    (void)sink;

    return 0;
}