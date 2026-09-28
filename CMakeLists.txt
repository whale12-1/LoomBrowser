cmake_minimum_required(VERSION 3.20)
project(ToyBrowserEngine LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- НАСТРОЙКИ КОМПИЛЯТОРА ---
if(MSVC)
    add_compile_options(/utf-8)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -Wpedantic)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address,undefined -fno-omit-frame-pointer")
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -fsanitize=address,undefined")
endif()

# --- АВТОМАТИЧЕСКАЯ ГЕНЕРАЦИЯ МЕТА-ОБЪЕКТОВ QT ---
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# --- ПОИСК ПАКЕТОВ QT6 И СИСТЕМНЫХ БИБЛИОТЕК ---
find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGLWidgets Network WebSockets)
find_package(OpenGL REQUIRED)
find_package(Freetype REQUIRED)

# --- OPENSSL 3.x (для TLS в Qt Network) ---
if(WIN32)
    if(NOT DEFINED OPENSSL_ROOT_DIR)
        set(OPENSSL_ROOT_DIR "C:/Program Files/OpenSSL-Win64")
    endif()
endif()

find_package(OpenSSL REQUIRED)

message(STATUS "OpenSSL version: ${OPENSSL_VERSION}")
message(STATUS "OpenSSL root:    ${OPENSSL_ROOT_DIR}")

# --- ПОДКЛЮЧЕНИЕ СТОРОННИХ ЗАВИСИМОСТЕЙ ---
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/quickjs/CMakeLists.txt")
    add_subdirectory(third_party/quickjs EXCLUDE_FROM_ALL)
endif()

# --- СБОР СУЩЕСТВУЮЩИХ ИСХОДНИКОВ ДВИЖКА ---
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.qrc"
)

# Исключаем служебные файлы CMake и папку билда
list(FILTER CORE_SOURCES EXCLUDE REGEX "${CMAKE_CURRENT_SOURCE_DIR}/out/.*")
list(FILTER CORE_SOURCES EXCLUDE REGEX "${CMAKE_CURRENT_SOURCE_DIR}/build/.*")
list(FILTER CORE_SOURCES EXCLUDE REGEX "CMakeCXXCompilerId\\.cpp$")

# --- 1. ЯДРО ДВИЖКА (СТАТИЧЕСКАЯ БИБЛИОТЕКА) ---
add_library(ToyBrowserCore STATIC ${CORE_SOURCES})

target_include_directories(ToyBrowserCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parsers
    ${CMAKE_CURRENT_SOURCE_DIR}/src/style
    ${CMAKE_CURRENT_SOURCE_DIR}/src/layout
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render
    ${CMAKE_CURRENT_SOURCE_DIR}/src/js
    ${CMAKE_CURRENT_SOURCE_DIR}/src/widgets
)

target_link_libraries(ToyBrowserCore PUBLIC 
    Qt6::Widgets 
    Qt6::OpenGLWidgets
    Qt6::Network 
    Qt6::WebSockets
    OpenGL::GL
    Freetype::Freetype
    OpenSSL::SSL
    OpenSSL::Crypto
)

if(TARGET quickjs)
    target_link_libraries(ToyBrowserCore PUBLIC quickjs)
endif()

# --- 2. ГЛАВНЫЙ ИСПОЛНЯЕМЫЙ ФАЙЛ ПРИЛОЖЕНИЯ ---
add_executable(ToyBrowserEngine apps/main.cpp)
target_link_libraries(ToyBrowserEngine PRIVATE ToyBrowserCore)

set_target_properties(ToyBrowserEngine PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
)

# --- АВТОКОПИРОВАНИЕ ПЛАГИНОВ QT (platforms и tls) ---
if(DEFINED VCPKG_INSTALLED_DIR)
    set(QT_PLUGINS_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/qt6/plugins")
    if(NOT EXISTS "${QT_PLUGINS_DIR}/platforms")
        set(QT_PLUGINS_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/qt6/plugins")
    endif()

    add_custom_command(TARGET ToyBrowserEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${QT_PLUGINS_DIR}/platforms"
            "$<TARGET_FILE_DIR:ToyBrowserEngine>/platforms"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${QT_PLUGINS_DIR}/tls"
            "$<TARGET_FILE_DIR:ToyBrowserEngine>/tls"
        COMMENT "Copying Qt platform & TLS plugins to output directory..."
    )
endif()

# Автоматическое копирование зависимых DLL
if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.21")
    add_custom_command(TARGET ToyBrowserEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:ToyBrowserEngine>
            $<TARGET_FILE_DIR:ToyBrowserEngine>
        COMMAND_EXPAND_LISTS
        COMMENT "Copying runtime DLLs to target directory..."
    )
endif()

if(WIN32)
    set_target_properties(ToyBrowserEngine PROPERTIES
        WIN32_EXECUTABLE OFF
    )
endif()

# --- 3. ПОДКЛЮЧЕНИЕ CATCH2 ТЕСТОВ ---
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# --- ЯВНОЕ КОПИРОВАНИЕ OpenSSL DLL ИЗ ПАПКИ УСТАНОВКИ ---
if(WIN32 AND TARGET ToyBrowserEngine)
    # Поиск DLL в корне OpenSSL и в папе bin/
    find_file(OPENSSL_SSL_DLL 
        NAMES libssl-3-x64.dll libssl-3.dll
        PATHS "C:/Program Files/OpenSSL-Win64" "C:/Program Files/OpenSSL-Win64/bin"
        NO_DEFAULT_PATH
    )
    find_file(OPENSSL_CRYPTO_DLL 
        NAMES libcrypto-3-x64.dll libcrypto-3.dll
        PATHS "C:/Program Files/OpenSSL-Win64" "C:/Program Files/OpenSSL-Win64/bin"
        NO_DEFAULT_PATH
    )

    if(OPENSSL_SSL_DLL AND OPENSSL_CRYPTO_DLL)
        add_custom_command(TARGET ToyBrowserEngine POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${OPENSSL_SSL_DLL}"
                "${OPENSSL_CRYPTO_DLL}"
                "$<TARGET_FILE_DIR:ToyBrowserEngine>"
            COMMENT "Copying OpenSSL DLLs (${OPENSSL_SSL_DLL}) → $<TARGET_FILE_DIR:ToyBrowserEngine>"
        )
    else()
        message(WARNING "Could not find OpenSSL DLLs to copy!")
    endif()
endif()