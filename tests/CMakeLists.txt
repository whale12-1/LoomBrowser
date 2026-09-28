find_package(Catch2 3 REQUIRED)
include(Catch)

# --- Тест CSS-парсера ---
add_executable(css_parser_tests
    main.cpp
    css_parser_tests.cpp
)
target_link_libraries(css_parser_tests PRIVATE ToyBrowserCore Catch2::Catch2)
catch_discover_tests(css_parser_tests)

# --- Тест HTML-парсера ---
add_executable(html_parser_tests
    main.cpp
    html_parser_tests.cpp
)
target_link_libraries(html_parser_tests PRIVATE ToyBrowserCore Catch2::Catch2)
catch_discover_tests(html_parser_tests)

# --- Тест Selector-Matcher-механизма ---

add_executable(selector_matcher_tests
    main.cpp
    selector_matcher_tests.cpp
)
target_link_libraries(selector_matcher_tests PRIVATE ToyBrowserCore Catch2::Catch2)
catch_discover_tests(selector_matcher_tests)

# --- Тест Style Tree Builder -механизма ---
add_executable(style_tree_builder_tests
    main.cpp
    style_tree_builder_tests.cpp
)
target_link_libraries(style_tree_builder_tests PRIVATE ToyBrowserCore Catch2::Catch2)
catch_discover_tests(style_tree_builder_tests)

# --- Тест Layout Tree Builder -механизма ---

add_executable(layout_tree_builder_tests
    main.cpp
    layout_tree_builder_tests.cpp
)
target_link_libraries(layout_tree_builder_tests PRIVATE ToyBrowserCore Catch2::Catch2)
catch_discover_tests(layout_tree_builder_tests)