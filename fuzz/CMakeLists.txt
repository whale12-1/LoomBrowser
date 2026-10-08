cmake_minimum_required(VERSION 3.20)

# ============================================================
#  Принудительно используем Clang: libFuzzer есть только у него.
#  ВАЖНО: устанавливается ДО project(), иначе CMake возьмёт GCC.
# ============================================================
if(NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang CACHE STRING "" FORCE)
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER clang++ CACHE STRING "" FORCE)
endif()

project(ToyBrowserFuzz LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "Fuzzing requires Clang. Got: ${CMAKE_CXX_COMPILER_ID} "
        "(${CMAKE_CXX_COMPILER})")
endif()

# ============================================================
#  Корень репозитория — на уровень выше папки fuzz/.
# ============================================================
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# ============================================================
#  Ядро для фаззеров — парсеры + style + layout, без Qt.
# ============================================================
set(FUZZ_COMMON_SOURCES "")
foreach(_cand
        ${REPO_ROOT}/src/parsers/html_parser/html_parser.cpp
        ${REPO_ROOT}/src/parsers/css_parser/css_parser.cpp
        ${REPO_ROOT}/src/parsers/selector_matcher/selector_matcher.cpp
        ${REPO_ROOT}/src/layout/layout_tree_builder.cpp
        ${REPO_ROOT}/src/parsers/selector_matcher/display_list_builder.cpp)
    if(EXISTS "${_cand}")
        list(APPEND FUZZ_COMMON_SOURCES "${_cand}")
    endif()
endforeach()

if(FUZZ_COMMON_SOURCES)
    add_library(tb_fuzz_core STATIC ${FUZZ_COMMON_SOURCES})
else()
    add_library(tb_fuzz_core INTERFACE)
endif()

target_include_directories(tb_fuzz_core INTERFACE
    ${REPO_ROOT}/src
    ${REPO_ROOT}/src/parsers
    ${REPO_ROOT}/src/parsers/arena_memory_allocator/headers
    ${REPO_ROOT}/src/parsers/html_parser
    ${REPO_ROOT}/src/parsers/html_parser/headers
    ${REPO_ROOT}/src/parsers/css_parser
    ${REPO_ROOT}/src/parsers/css_parser/headers
    ${REPO_ROOT}/src/parsers/selector_matcher
    ${REPO_ROOT}/src/parsers/selector_matcher/headers
    ${REPO_ROOT}/src/style
    ${REPO_ROOT}/src/layout
    ${REPO_ROOT}/src/layout/headers
    ${REPO_ROOT}/src/render
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

add_fuzz_target(fuzz_html         fuzz_html.cpp)
add_fuzz_target(fuzz_css          fuzz_css.cpp)
add_fuzz_target(fuzz_selector     fuzz_selector.cpp)
add_fuzz_target(fuzz_style_tree   fuzz_style_tree.cpp)
add_fuzz_target(fuzz_layout       fuzz_layout.cpp)
add_fuzz_target(fuzz_display_list fuzz_display_list.cpp)