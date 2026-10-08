# ============================================================
#  ядро дл€ фаззеров Ч только парсеры, без Qt.
#  ¬се парсеры header-only, поэтому достаточно INTERFACE-библиотеки,
#  котора€ прокидывает include-пути вниз, к самим фаззерам.
# ============================================================

# ≈сли какие-то из этих .cpp вдруг по€в€тс€ в будущем Ч они попадут сюда.
set(FUZZ_COMMON_SOURCES "")
foreach(_cand
        ${CMAKE_SOURCE_DIR}/src/parsers/html_parser/html_parser.cpp
        ${CMAKE_SOURCE_DIR}/src/parsers/css_parser/css_parser.cpp
        ${CMAKE_SOURCE_DIR}/src/parsers/selector_matcher/selector_matcher.cpp)
    if(EXISTS "${_cand}")
        list(APPEND FUZZ_COMMON_SOURCES "${_cand}")
    endif()
endforeach()

if(FUZZ_COMMON_SOURCES)
    add_library(tb_fuzz_core STATIC ${FUZZ_COMMON_SOURCES})
    message(STATUS "Fuzz: tb_fuzz_core Ч STATIC, sources: ${FUZZ_COMMON_SOURCES}")
else()
    add_library(tb_fuzz_core INTERFACE)
    message(STATUS "Fuzz: tb_fuzz_core Ч INTERFACE (header-only parsers)")
endif()

# Include-пути. ѕор€док не важен, но важно, чтобы все headers-подкаталоги
# были перечислены Ч иначе компил€тор не найдЄт .h-файлы из #include.
target_include_directories(tb_fuzz_core INTERFACE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/parsers

    ${CMAKE_SOURCE_DIR}/src/parsers/arena_memory_allocator/headers

    ${CMAKE_SOURCE_DIR}/src/parsers/html_parser
    ${CMAKE_SOURCE_DIR}/src/parsers/html_parser/headers

    ${CMAKE_SOURCE_DIR}/src/parsers/css_parser
    ${CMAKE_SOURCE_DIR}/src/parsers/css_parser/headers

    ${CMAKE_SOURCE_DIR}/src/parsers/selector_matcher
    ${CMAKE_SOURCE_DIR}/src/parsers/selector_matcher/headers
)

# ============================================================
#  Helper: создать fuzz-таргет из одного .cpp
# ============================================================
function(add_fuzz_target name source)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
        message(WARNING "Fuzz target '${name}': source '${source}' not found, skipping.")
        return()
    endif()

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