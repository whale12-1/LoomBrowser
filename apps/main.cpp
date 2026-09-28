#include <iostream>
#include <QApplication>
#include <QSslSocket>
#include <QCoreApplication>
#include <QLibraryInfo>
#include <QDebug>

#include "../parsers/html_parser/headers/html_parser.h"
#include "../parsers/css_parser/headers/css_parser.h"
#include "../parsers/selector_matcher/style_tree_builder.h"
#include "../layout/headers/layout_tree_builder.h"
#include "../parsers/selector_matcher/display_list_builder.h"
#include "../parsers/arena_memory_allocator/headers/arena.h"
#include "../widgets/headers/BrowserWindow.h"

static const char* kHTML = R"HTML(
<html>
  <body>
    <div class="card">
      <h1>Hello, MiniBrowser</h1>
      <p>This is a tiny test page rendered by our own pipeline.</p>
      <button>Click me</button>
    </div>
  </body>
</html>
)HTML";

static const char* kCSS = R"CSS(
html, body { display: block; margin: 0; padding: 0; }
body { background-color: #f0f0f0; font-size: 16px; color: #222222; }

.card {
    display: block;
    margin: 24px;
    padding: 24px;
    background-color: #ffffff;
    border-top-width: 4px;
    border-left-width: 4px;
    width: 640px;
}

h1 {
    display: block;
    font-size: 32px;
    color: #1a1a1a;
    margin-bottom: 12px;
}

p {
    display: block;
    font-size: 16px;
    color: #444444;
    margin-bottom: 16px;
}

button {
    display: inline-block;
    padding: 8px 16px;
    background-color: #3498db;
    color: #ffffff;
    font-size: 14px;
}
)CSS";

// HTMLParser возвращает Document-узел; StyleTreeBuilder/LayoutTreeBuilder
// работают только с Element. Берём первый Element-ребёнок.
static DOMNode* first_element_child(DOMNode* node) {
    if (!node) return nullptr;
    if (node->type == NodeType::Element) return node;
    for (DOMNode* c : node->children) {
        if (c && c->type == NodeType::Element) return c;
    }
    return nullptr;
}

int main(int argc, char** argv) {
    ArenaAllocator arena;

    HTMLParser html_parser(arena);
    DOMNode* dom = html_parser.parse(kHTML);
    DOMNode* root_elem = first_element_child(dom);
    if (!root_elem) { std::cerr << "No element root\n"; return 1; }

    CSSParser css_parser(arena);
    StyleSheet* sheet = css_parser.parse(kCSS);
    if (!sheet) { std::cerr << "CSS parse failed\n"; return 1; }

    StyleStorageSoA storage = StyleTreeBuilder::build(root_elem, *sheet);
    std::cout << "[style]   nodes: " << storage.size() << "\n";

    auto layout = LayoutTreeBuilder::build(storage);
    if (!layout) { std::cerr << "layout build failed\n"; return 1; }

    LayoutTreeBuilder::Viewport viewport;
    viewport.width = 1024.0f;
    viewport.height = 768.0f;

    LayoutTreeBuilder::compute_layout(layout.get(), viewport, storage);
    std::cout << "[layout]  root "
        << layout->geometry.width << " x "
        << layout->geometry.height << "\n";

    DisplayList list = DisplayListBuilder::build(layout.get(), storage);
    std::cout << "[display] items: " << list.size() << "\n";

    QApplication app(argc, argv);

    qDebug() << "[diag] exe path:"
        << QCoreApplication::applicationDirPath();
    qDebug() << "[diag] Qt plugins path:"
        << QLibraryInfo::path(QLibraryInfo::PluginsPath);
    qDebug() << "[diag] SSL supported:"
        << QSslSocket::supportsSsl();
    qDebug() << "[diag] SSL build version:"
        << QSslSocket::sslLibraryBuildVersionString();
    qDebug() << "[diag] SSL runtime version:"
        << QSslSocket::sslLibraryVersionString();
    BrowserWindow window;
    window.setContent(list, "about:blank");
    window.show();
    return app.exec();

}