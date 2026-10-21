# MiniBrowser: Custom C++ Browser Engine

**MiniBrowser** (LoomBrowser) is a lightweight, from-scratch web rendering engine and browser UI built with C++20 and Qt 6. Unlike projects that wrap Chromium or WebKit, this engine implements its own HTML tokenizer, CSS cascade, layout tree, and rendering pipeline.

The goal of this project is to explore browser architecture, modern C++ memory management, and complex layout algorithms.


> 🇷🇺 [Читать на русском](README.ru.md)



## ✨ Features

### Parsing & Styling
*   **Custom HTML5 Parser:** State-machine based tokenizer supporting `RCDATA`, `RAWTEXT`, `ScriptData`, implicit tag closing, and character references.
*   **CSS Parser:** Parses complex selectors, handles `@media` queries, specificity, `!important`, and CSS variables (`var()`).
*   **Cascade & Inheritance:** Full CSS cascade implementation (User-Agent, Author, Inline styles) with support for 70+ CSS properties.

### Layout Engine
*   **Block & Inline Formatting Contexts (BFC/IFC):** Correct handling of block formatting, line boxes, baseline alignment, float wrapping, and margin collapsing.
*   **Flexbox & Grid:** Custom implementations of CSS Flexbox (grow, shrink, wrap, justify/align) and CSS Grid (`grid-template-columns`, `fr` units, `minmax`).
*   **Tables:** Table layout algorithm with `colspan`/`rowspan` and column width distribution.

### Architecture & Performance
*   **Arena Memory Allocator:** DOM nodes and CSS structures are allocated in a custom bump allocator to minimize heap fragmentation and drastically improve parsing speed.
*   **Structure of Arrays (SoA):** Computed styles are stored in a cache-friendly SoA format (`StyleStorageSoA`) instead of heavy linked objects.
*   **Display List & Backing Store:** Layout generates a flat display list, which is rendered to a cached `QPixmap` (HiDPI/Retina ready) for buttery-smooth scrolling.

### Tooling & Robustness
*   **QuickJS Integration:** Embedded QuickJS engine for executing inline scripts.
*   **Headless CLI Renderer (`render_cli`):** Fully decoupled from the GUI. Renders any HTML file to a PNG using `QT_QPA_PLATFORM=offscreen`. Perfect for automated visual regression testing.
*   **Fuzzing Ready:** Built-in libFuzzer targets for HTML tokenizer, CSS parser, and display list builder to catch edge-case crashes.
*   **CI/CD Pipeline:** Automated GitHub Actions workflow that builds the project, runs 184+ unit tests (Catch2), and executes fuzzers.

## 💻 Usage

### 1. GUI Browser
Build and run the main application to browse local HTML files and the web.

### 2. Headless CLI Renderer (Screenshot Tool)
You can render any HTML file into a PNG completely headlessly.

**Important for Windows Users (Fonts):**
By default, Qt's `offscreen` platform might not find system fonts. To render text correctly, you must point Qt to the Windows Fonts directory.

powershell
# In Windows PowerShell:
$env:QT_QPA_PLATFORM="offscreen"
$env:QT_QPA_FONTDIR="C:\Windows\Fonts"

# Run the renderer
.\render_cli --width 1024 --height 768 -o output.png your_file.html
On Linux/macOS:

bash
QT_QPA_PLATFORM=offscreen ./render_cli --width 1024 --height 768 -o output.png your_file.html
🖼️ Examples
Here are some pages that render well with the current engine state:

Hacker News (news.ycombinator.com)
tests/screenshots/

Local HTML Tests
tests/screenshots/out.png
tests/screenshots/tree.txt

🚀 How to Build
Prerequisites
C++20 compatible compiler (MSVC 2022, GCC 11+, or Clang 14+)

CMake 3.20+

Qt 6.5+ (Widgets, Network, Svg modules)

Build Steps
bash
git clone --recursive https://github.com/whale12-1/LoomBrowser.git
cd LoomBrowser
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
🧪 Testing & Fuzzing
Run the unit tests (Catch2):

bash
cd build
ctest --output-on-failure
Run the fuzzers locally (requires Clang):

bash
cd fuzz
cmake -B build-fuzz -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -G Ninja
cmake --build build-fuzz
./build-fuzz/fuzz_html corpus/html
🚧 Limitations & Roadmap
Advanced CSS: transform, box-shadow, clip-path, CSS Multi-column, and animations are not implemented.

Pseudo-elements: ::before and ::after are parsed but not fully rendered.

Z-Index Stacking Contexts: Basic z-index is supported, but complex stacking contexts with opacity may render incorrectly.