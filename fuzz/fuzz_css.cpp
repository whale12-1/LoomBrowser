#include <cstdint>
#include <cstddef>
#include <string>

#include "parsers/css_parser/headers/css_parser.h"
#include "parsers/css_parser/headers/css_dom.h"
#include "parsers/arena_memory_allocator/headers/arena.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 32 * 1024) return 0;

    ArenaAllocator arena;
    std::string css(reinterpret_cast<const char*>(data), size);

    CSSParser parser(arena);
    StyleSheet* sheet = parser.parse(css);

    // Инвариант: парсер не должен возвращать nullptr даже на мусоре
    if (sheet == nullptr) __builtin_trap();

    // Проверим, что все селекторы имеют хотя бы один compound
    for (const auto& rule : sheet->rules) {
        for (const auto& sel : rule.selectors) {
            if (sel.compounds.empty()) __builtin_trap();
        }
    }

    return 0;
}