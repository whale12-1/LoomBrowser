# MiniBrowser: Собственный браузерный движок на C++

**MiniBrowser** (LoomBrowser) — это легковесный движок рендеринга веб-страниц и UI для браузера, написанный с нуля на C++20 и Qt 6. В отличие от проектов, которые просто оборачивают Chromium или WebKit, этот движок реализует собственный HTML-токенизатор, CSS-каскад, дерево макета (layout tree) и пайплайн рендеринга.

Цель проекта — исследовать архитектуру браузеров, современное управление памятью в C++ и сложные алгоритмы верстки.


> 🇬🇧 [Read in English](README.md)


## ✨ Особенности

### Парсинг и стили
*   **Собственный HTML5-парсер:** Токенизатор на основе конечного автомата с поддержкой `RCDATA`, `RAWTEXT`, `ScriptData`, неявного закрытия тегов и символьных ссылок.
*   **CSS-парсер:** Поддержка сложных селекторов, медиа-запросов `@media`, специфичности, `!important` и CSS-переменных (`var()`).
*   **Каскад и наследование:** Полная реализация каскада CSS (User-Agent, Author, Inline) и поддержка 70+ свойств.

### Движок верстки (Layout)
*   **Block & Inline контексты (BFC/IFC):** Корректная обработка блочной верстки, строчных боксов (line boxes), выравнивания по базовой линии (baseline), обтекания (float) и схлопывания марджинов (margin collapsing).
*   **Flexbox и Grid:** Собственная реализация CSS Flexbox (grow, shrink, wrap) и CSS Grid (`grid-template-columns`, единицы `fr`, `minmax`).
*   **Таблицы:** Алгоритм верстки таблиц с поддержкой `colspan`/`rowspan` и распределением ширины колонок.

### Архитектура и производительность
*   **Arena Memory Allocator:** DOM-узлы и CSS-структуры аллоцируются в кастомном bump-аллокаторе для минимизации фрагментации памяти и ускорения парсинга.
*   **Structure of Arrays (SoA):** Вычисленные стили хранятся в кэш-дружелюбном формате SoA (`StyleStorageSoA`) вместо тяжелых объектов.
*   **Display List:** Layout генерирует плоский список команд отрисовки, который рендерится в кэшированный `QPixmap` (с поддержкой HiDPI/Retina) для плавного скроллинга.

### Инструменты и надежность
*   **Интеграция QuickJS:** Встроенный движок QuickJS для выполнения инлайн-скриптов.
*   **Headless CLI-рендерер (`render_cli`):** Полностью отвязан от GUI. Рендерит любой HTML в PNG с помощью `QT_QPA_PLATFORM=offscreen`. Идеально для автотестов (visual regression).
*   **Фаззинг:** Встроенные таргеты libFuzzer для проверки парсеров на краши и утечки памяти.
*   **CI/CD:** Настроен GitHub Actions (сборка, 184+ юнит-теста Catch2, запуск фаззеров).

## 💻 Использование

### 1. GUI Браузер
Соберите и запустите основное приложение для серфинга по локальным файлам и интернету.

### 2. Headless CLI-рендерер (Скриншотер)
Вы можете рендерить любой HTML-файл в PNG без открытия окна.

**Важно для Windows (Шрифты):**
По умолчанию платформа `offscreen` в Qt может не найти системные шрифты. Чтобы текст отрисовывался корректно, укажите путь к папке со шрифтами Windows.

powershell
# В Windows PowerShell:
$env:QT_QPA_PLATFORM="offscreen"
$env:QT_QPA_FONTDIR="C:\Windows\Fonts"

# Запуск
.\render_cli --width 1024 --height 768 -o output.png your_file.html
🖼️ Примеры для рендеринга
Вот несколько страниц, которые отлично отрисовываются текущим движком:

Hacker News (news.ycombinator.com)
tests/screenshots/

Локальные тесты
Загляните в папку tests/screenshots/, там лежат простые HTML-файлы для проверки конкретных фич (Flexbox, Grid).

🚀 Сборка
Требования
Компилятор с поддержкой C++20 (MSVC 2022, GCC 11+ или Clang 14+)

CMake 3.20+

Qt 6.5+ (модули Widgets, Network, Svg)

Шаги сборки
bash
git clone --recursive https://github.com/whale12-1/LoomBrowser.git
cd LoomBrowser
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
🧪 Тестирование и фаззинг
Запуск юнит-тестов (Catch2):

bash
cd build
ctest --output-on-failure
Запуск фаззеров (требуется Clang):

bash
cd fuzz
cmake -B build-fuzz -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -G Ninja
cmake --build build-fuzz
./build-fuzz/fuzz_html corpus/html
🚧 Ограничения и планы
Продвинутый CSS: transform, box-shadow, clip-path, многоколоночная верстка (multi-column) и анимации пока не реализованы.

Псевдоэлементы: ::before и ::after парсятся, но не рендерятся.

Z-Index: Базовый z-index работает, но сложные контексты наложения с прозрачностью могут отрисовываться неверно.

