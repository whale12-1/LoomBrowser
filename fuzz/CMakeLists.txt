# Ядро парсеров как отдельная библиотека БЕЗ Qt-зависимостей.
# Если парсеры тянут Qt — придётся либо собирать под Qt, либо вырезать
# Qt-зависимости. В вашем случае Arena/HTML/CSS/Selector от Qt не зависят.

set(FUZZ_COMMON_SOURCES
    ${CMAKE_SOURCE_DIR}/src/parsers/html_parser/html_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/parsers/css_parser/css_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/parsers/selector_matcher/selector_matcher.cpp
)

add_library(tb_fuzz_core STATIC ${FUZZ_COMMON_SOURCES})
target_include_directories(tb_fuzz_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/parsers
    ${CMAKE_SOURCE_DIR}/src/parsers/html_parser
    ${CMAKE_SOURCE_DIR}/src/parsers/css_parser
    ${CMAKE_SOURCE_DIR}/src/parsers/selector_matcher
    ${CMAKE_SOURCE_DIR}/src/parsers/arena_memory_allocator/headers
)

# -----------------------------------------------------------------
#  Helper: создаёт fuzz-таргет из одного .cpp
# -----------------------------------------------------------------
function(add_fuzz_target name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE tb_fuzz_core)
    target_compile_options(${name} PRIVATE
        -fsanitize=fuzzer,address,undefined
        -fno-omit-frame-pointer -g -O1)
    target_link_options(${name} PRIVATE
        -fsanitize=fuzzer,address,undefined)
endfunction()

add_fuzz_target(fuzz_html     fuzz_html.cpp)
add_fuzz_target(fuzz_css      fuzz_css.cpp)
add_fuzz_target(fuzz_selector fuzz_selector.cpp)