#include <cstdint>
#include <cstddef>
#include <string>

#include "parsers/html_parser/headers/html_parser.h"
#include "parsers/html_parser/headers/dom.h"
#include "parsers/arena_memory_allocator/headers/arena.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // 1. Отсечка: слишком большие входы игнорируем — иначе фаззер съест
    //    память за секунды и упадёт от OOM раньше, чем найдёт реальный баг.
    if (size > 64 * 1024) return 0;

    // 2. Арена на стеке — обнуляется на каждом вызове. Идеально для фаззинга:
    //    никаких утечек между итерациями, всё освобождается при выходе.
    ArenaAllocator arena;

    // 3. Парсинг. Здесь НЕ должно быть исключений, exit(), throw и т.п.
    //    Если парсер бросает — это уже баг, фаззер его зафиксирует как краш.
    std::string html(reinterpret_cast<const char*>(data), size);
    HTMLParser parser(arena);
    DOMNode* doc = parser.parse(html);

    // 4. Проверка инварианта. Если он нарушен — программируем падение,
    //    чтобы фаззер понял, что вход «плохой».
    //    Непустой вход → должен быть Document-узел.
    if (!html.empty() && doc == nullptr) {
        __builtin_trap();
    }
    if (doc != nullptr && doc->type != NodeType::Document) {
        __builtin_trap();
    }

    // 5. Опционально: рекурсивный обход с проверкой инвариантов DOM.
    //    Например, у каждого узла parent должен указывать на того,
    //    кто его держит в children.
    //    Осторожно: глубокий обход на больших входах может сам по себе
    //    ловить stack overflow, который фаззер сообщит как краш.
    //    Если хотите — ограничьте глубину 100.

    return 0;
}