cmake_minimum_required(VERSION 3.20)
project(ToyBrowserEngine LANGUAGES CXX C)

option(TOYBROWSER_FUZZING "Build libFuzzer targets" OFF)

if(TOYBROWSER_FUZZING)
    # Fuzzing требует Clang (или clang-cl на Windows)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
            "Fuzzing requires Clang. Use clang++ or clang-cl.")
    endif()

    # На Linux/macOS включаем фаззер как линковку main
    # На Windows фаззер в clang-cl работает через -fsanitize=fuzzer
    add_compile_options(-fsanitize=fuzzer-no-link,address,undefined
                        -fno-omit-frame-pointer -g -O1)
endif()

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
# --- ПОДКЛЮЧЕНИЕ QUICKJS ---
set(QUICKJS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/quickjs")

if(EXISTS "${QUICKJS_DIR}/CMakeLists.txt")
    message(STATUS "QuickJS: found CMakeLists at ${QUICKJS_DIR}")
    add_subdirectory(third_party/quickjs EXCLUDE_FROM_ALL)

    # Ищем любой из известных target-имён
    if(TARGET quickjs)
        message(STATUS "QuickJS: target 'quickjs' available")
        set(_qjs_target quickjs)
    elseif(TARGET quickjs-ng)
        message(STATUS "QuickJS: target 'quickjs-ng' available")
        set(_qjs_target quickjs-ng)
    elseif(TARGET qjs)
        message(STATUS "QuickJS: target 'qjs' available")
        set(_qjs_target qjs)
    else()
        message(WARNING "QuickJS: no known target found. "
                        "Check third_party/quickjs/CMakeLists.txt for add_library name.")
        set(_qjs_target "")
    endif()
else()
    message(WARNING "QuickJS: ${QUICKJS_DIR}/CMakeLists.txt NOT FOUND")
    set(_qjs_target "")
endif()

# Где лежит quickjs.h
file(GLOB_RECURSE QUICKJS_HEADER "${QUICKJS_DIR}/quickjs.h")
if(QUICKJS_HEADER)
    list(GET QUICKJS_HEADER 0 _qjs_hdr)
    get_filename_component(_qjs_include_dir "${_qjs_hdr}" DIRECTORY)
    message(STATUS "QuickJS: header found at ${_qjs_hdr}")
else()
    message(WARNING "QuickJS: quickjs.h NOT FOUND anywhere in ${QUICKJS_DIR}")
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

# --- Гарантированный проброс include-пути ---
if(QUICKJS_HEADER)
    # _qjs_include_dir уже вычислен выше
    target_include_directories(ToyBrowserCore PUBLIC "${_qjs_include_dir}")
endif()

# --- Гарантированная линковка ---
if(_qjs_target)
    target_link_libraries(ToyBrowserCore PUBLIC ${_qjs_target})
endif()


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


if(TOYBROWSER_FUZZING)
    add_subdirectory(fuzz)
endif()