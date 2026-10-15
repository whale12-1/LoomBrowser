#include <QGuiApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QString>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "parsers/arena_memory_allocator/headers/arena.h"
#include "parsers/html_parser/headers/html_parser.h"
#include "parsers/html_parser/headers/dom.h"
#include "parsers/css_parser/headers/css_parser.h"
#include "parsers/selector_matcher/style_tree_builder.h"
#include "parsers/selector_matcher/user_agent_stylesheet.h"
#include "parsers/selector_matcher/style_origin.h"
#include "parsers/selector_matcher/display_list_builder.h"
#include "layout/headers/layout_tree_builder.h"
#include "./headers/display_list_renderer.h"
#include "./headers/tree_dumper.h"

namespace {

    // -------- Файловые утилиты --------

    std::string read_file(const QString& path) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return {};
        const QByteArray data = f.readAll();
        return std::string(data.constData(), static_cast<size_t>(data.size()));
    }

    // Собрать текст всех <style> тегов
    void collect_style_text(const DOMNode* n, std::string& out) {
        if (!n) return;
        if (n->type == NodeType::Element) {
            const std::string_view tag(n->tag_name.data(), n->tag_name.size());
            if (tag == "style") {
                for (const DOMNode* c : n->children) {
                    if (c->type == NodeType::Text) {
                        out.append(c->text_content.data(),
                            c->text_content.size());
                    }
                }
                return;   // внутрь <style> не заходим
            }
        }
        for (const DOMNode* c : n->children) collect_style_text(c, out);
    }

    DOMNode* first_element(DOMNode* n) {
        if (!n) return nullptr;
        if (n->type == NodeType::Element) return n;
        for (DOMNode* c : n->children)
            if (auto* e = first_element(c)) return e;
        return nullptr;
    }

    // -------- Qt-метрики для layout'а --------

    LayoutTreeBuilder::TextMetrics make_qt_metrics() {
        LayoutTreeBuilder::TextMetrics tm;

        tm.measure_width = [](const std::string& text, float fs,
            bool bold, bool italic) {
                QFont f;
                f.setPixelSize(std::max(1, qRound(fs)));
                f.setBold(bold);
                f.setItalic(italic);
                QFontMetricsF fm(f);
                const QString s = QString::fromUtf8(text.data(),
                    static_cast<int>(text.size()));
                return static_cast<float>(fm.horizontalAdvance(s));
            };

        tm.line_spacing = [](float fs, bool bold) {
            QFont f;
            f.setPixelSize(std::max(1, qRound(fs)));
            f.setBold(bold);
            QFontMetricsF fm(f);
            return static_cast<float>(fm.lineSpacing());
            };

        return tm;
    }
    std::string resolve_path(const std::string& base_dir,
        const std::string& src)
    {
        if (src.empty()) return {};
        if (src.size() >= 2 && src[1] == ':') return src;
        if (src[0] == '/' || src[0] == '\\') return src;
        if (base_dir.empty()) return src;
        std::string out = base_dir;
        if (!out.empty() && out.back() != '/' && out.back() != '\\')
            out += '/';
        out += src;
        return out;
    }

    LayoutTreeBuilder::ImageMetrics make_image_metrics(
        const std::string& base_dir)
    {
        LayoutTreeBuilder::ImageMetrics m;
        if (base_dir.empty()) return m;
        m.intrinsic_size = [base_dir](const std::string& src)
            -> std::pair<float, float>
            {
                const std::string path = resolve_path(base_dir, src);
                QImage img;
                if (!img.load(QString::fromStdString(path)))
                    return { 0.0f, 0.0f };
                return { float(img.width()), float(img.height()) };
            };
        return m;
    }

} // namespace


int main(int argc, char** argv) {
    // Для CI и headless-окружений: offscreen плагин.
    // Ставим ДО создания QGuiApplication.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    QGuiApplication app(argc, argv);

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "loom-render — headless HTML→PNG renderer for ToyBrowserEngine");
    parser.addHelpOption();

    QCommandLineOption outputOpt({ "o", "output" },
        "Output PNG file path", "file", "output.png");
    QCommandLineOption widthOpt("width",
        "Viewport width in pixels", "px", "1024");
    QCommandLineOption heightOpt("height",
        "Viewport height in pixels", "px", "768");
    QCommandLineOption treeOpt("dump-tree",
        "Also dump layout tree to this text file", "file");

    parser.addOption(outputOpt);
    parser.addOption(widthOpt);
    parser.addOption(heightOpt);
    parser.addOption(treeOpt);
    parser.addPositionalArgument("input", "Input HTML file");

    parser.process(app);

    const QStringList pos = parser.positionalArguments();
    if (pos.isEmpty()) {
        std::cerr << "error: no input HTML file\n";
        parser.showHelp(1);
    }

    const QString input_path = pos[0];
    const int vw = parser.value(widthOpt).toInt();
    const int vh = parser.value(heightOpt).toInt();

    const std::string input_std = input_path.toStdString();
    const size_t slash = input_std.find_last_of("/\\");
    const std::string base_dir =
        (slash == std::string::npos) ? "" : input_std.substr(0, slash);

    std::cerr << "[diag] input_path = " << input_std << "\n";
    std::cerr << "[diag] base_dir   = " << base_dir << "\n";

    if (vw <= 0 || vh <= 0) {
        std::cerr << "error: width and height must be positive\n";
        return 1;
    }

    const std::string html = read_file(input_path);
    if (html.empty()) {
        std::cerr << "error: cannot read input file '"
            << input_path.toStdString() << "'\n";
        return 1;
    }

    std::cout << "[1/5] parsing HTML (" << html.size() << " bytes)...\n";

    // 1. HTML parse
    ArenaAllocator arena;
    HTMLParser hp(arena);
    DOMNode* doc = hp.parse(html);
    if (!doc) {
        std::cerr << "error: html parse failed\n";
        return 1;
    }

    DOMNode* root_elem = first_element(doc);
    if (!root_elem) {
        std::cerr << "error: no root element in document\n";
        return 1;
    }

    // 2. CSS parse: UA + author
    std::string author_css;
    collect_style_text(root_elem, author_css);

    CSSParser cp(arena);
    StyleSheet* ua = cp.parse(ua_css::kSource);
    StyleSheet* author = cp.parse(author_css);

    std::vector<StyleRuleIndex::SheetRef> sources = {
        { ua,     Origin::UserAgent },
        { author, Origin::Author    },
    };

    std::cout << "[2/5] cascade (" << author_css.size()
        << " bytes of author CSS)...\n";

    // 3. Каскад
    StyleStorageSoA storage = StyleTreeBuilder::build(root_elem, sources);


    // 4. Layout
    std::cout << "[3/5] layout...\n";
    auto layout = LayoutTreeBuilder::build(storage,
        make_image_metrics(base_dir));
    if (!layout) {
        std::cerr << "error: layout build failed\n";
        return 1;
    }

    LayoutTreeBuilder::Viewport vp{ float(vw), float(vh) };
    LayoutTreeBuilder::compute_layout(layout.get(), vp, storage,
        make_qt_metrics());



    // 5. Display list
    std::cout << "[4/5] display list...\n";
    DisplayList dl = DisplayListBuilder::build(layout.get(), storage);
    std::cout << "      " << dl.items.size() << " items\n";

    // 6. PNG
    std::cout << "[5/5] rendering PNG...\n";
    const QImage img = DisplayListRenderer::render(dl, vw, vh);
    const QString out_path = parser.value(outputOpt);
    if (!img.save(out_path, "PNG")) {
        std::cerr << "error: cannot save PNG to '"
            << out_path.toStdString() << "'\n";
        return 1;
    }
    std::cout << "rendered: " << out_path.toStdString() << "\n";

    // 7. Опционально — dump tree
    if (parser.isSet(treeOpt)) {
        const QString tree_path = parser.value(treeOpt);
        std::ofstream out(tree_path.toStdString());
        if (!out) {
            std::cerr << "error: cannot open tree output '"
                << tree_path.toStdString() << "'\n";
            return 1;
        }
        TreeDumper::dump(layout.get(), storage, out);
        std::cout << "tree dumped: " << tree_path.toStdString() << "\n";
    }

    return 0;
}