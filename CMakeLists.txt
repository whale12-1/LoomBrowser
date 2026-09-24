cmake_minimum_required(VERSION 3.20)
project(ToyBrowserEngine LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- НАСТРОЙКИ КОМПИЛЯТОРА ---
# Обработка кириллицы в коде для MSVC
if(MSVC)
    add_compile_options(/utf-8)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -Wpedantic)
    # AddressSanitizer для отладки работы с памятью в Debug
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address,undefined -fno-omit-frame-pointer")
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -fsanitize=address,undefined")
endif()

# --- ВКУЧАЕМ АВТОМАТИЧЕСКУЮ ГЕНЕРАЦИЮ МЕТА-ОБЪЕКТОВ QT ---
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# --- ПОИСК ПАКЕТОВ QT6 И СИСТЕМНЫХ БИБЛИОТЕК ---
find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGLWidgets Network WebSockets)
find_package(OpenGL REQUIRED)
find_package(Freetype REQUIRED)

# --- ПОДКЛЮЧЕНИЕ СТОРOННИХ ЗАВИСИМОСТЕЙ ---
# QuickJS подключается как C-подпроект в папке third_party
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/quickjs/CMakeLists.txt")
    add_subdirectory(third_party/quickjs EXCLUDE_FROM_ALL)
endif()

# --- ПУТИ ПОИСКА ЗАГОЛОВОЧНЫХ ФАЙЛОВ ---
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parsers
    ${CMAKE_CURRENT_SOURCE_DIR}/src/style
    ${CMAKE_CURRENT_SOURCE_DIR}/src/layout
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render
    ${CMAKE_CURRENT_SOURCE_DIR}/src/js
    ${CMAKE_CURRENT_SOURCE_DIR}/src/widgets
)

# --- АВТОМАТИЧЕСКИЙ СБОР ИСХОДНИКОВ ---
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.qrc"
)

# Исключаем служебные файлы CMake и папку билда
list(FILTER SOURCES EXCLUDE REGEX "${CMAKE_CURRENT_SOURCE_DIR}/out/.*")
list(FILTER SOURCES EXCLUDE REGEX "${CMAKE_CURRENT_SOURCE_DIR}/build/.*")
list(FILTER SOURCES EXCLUDE REGEX "CMakeCXXCompilerId\\.cpp$")

# --- СБОРКА ИСПОЛНЯЕМОГО ФАЙЛА ---
add_executable(ToyBrowserEngine ${SOURCES} apps/main.cpp)

# Ссылаемся на модули Qt6, OpenGL, FreeType и QuickJS
target_link_libraries(ToyBrowserEngine PRIVATE 
    Qt6::Widgets 
    Qt6::OpenGLWidgets
    Qt6::Network 
    Qt6::WebSockets
    OpenGL::GL
    Freetype::Freetype
)

if(TARGET quickjs)
    target_link_libraries(ToyBrowserEngine PRIVATE quickjs)
endif()

# --- НАСТРОЙКА ПУТЕЙ ВЫВОДА ---
set_target_properties(ToyBrowserEngine PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
)

# --- АВТОМАТИЧЕСКОЕ КОПИРОВАНИЕ ПЛАГИНОВ ПЛАТФОРМЫ QT (VCPKG) ---
if(DEFINED VCPKG_INSTALLED_DIR)
    set(QT_PLUGINS_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/qt6/plugins")
    if(NOT EXISTS "${QT_PLUGINS_DIR}/platforms")
        set(QT_PLUGINS_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/qt6/plugins")
    endif()

    add_custom_command(TARGET ToyBrowserEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${QT_PLUGINS_DIR}/platforms"
        "$<TARGET_FILE_DIR:ToyBrowserEngine>/platforms"
        COMMENT "Copying Qt platform plugins to output directory..."
    )
endif()

# Окно без консоли для Windows в Release-сборке
if(WIN32)
    set_target_properties(ToyBrowserEngine PROPERTIES
        WIN32_EXECUTABLE OFF
    )
endif()