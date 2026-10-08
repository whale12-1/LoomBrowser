#include "JSEngine.h"

#include <QDebug>
#include <QString>
#include "bindings/DocumentBindings.h"
#include "bindings/ElementBindings.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>

// ============================================================
//  Вспомогательное — извлечь строку из JS-значения, безопасно
// ============================================================
namespace {

    std::string jsToString(JSContext* ctx, JSValueConst v) {
        const char* s = JS_ToCString(ctx, v);
        if (!s) return "<unprintable>";
        std::string out(s);
        JS_FreeCString(ctx, s);
        return out;
    }

    // ------------------------------------------------------------
    //  console.log / console.warn / console.error
    //  Реализованы как одна функция с аргументом level.
    // ------------------------------------------------------------
    JSValue js_console_log(JSContext* ctx, JSValueConst /*this_val*/,
        int argc, JSValueConst* argv,
        int magic)
    {
        const char* level =
            (magic == 1) ? "warn" :
            (magic == 2) ? "error" : "log";

        std::string line = "[js:";
        line += level;
        line += "] ";

        for (int i = 0; i < argc; ++i) {
            if (i > 0) line += ' ';
            line += jsToString(ctx, argv[i]);
        }

        // Пишем в тот же поток, что и qDebug — так проще смотреть вывод.
        qDebug().noquote() << QString::fromStdString(line);
        return JS_UNDEFINED;
    }

} // namespace

// ============================================================
//  Жизненный цикл
// ============================================================

JsEngine::JsEngine() {
    rt_ = JS_NewRuntime();
    if (!rt_) throw std::runtime_error("JS_NewRuntime failed");

    // Разумные дефолты для toy browser.
    // 32 МБ heap — с запасом хватит на небольшие страницы.
    // 512 КБ stack — на рекурсивные обходы DOM.
    JS_SetMemoryLimit(rt_, 32u * 1024u * 1024u);
    JS_SetMaxStackSize(rt_, 512u * 1024u);

    ctx_ = JS_NewContext(rt_);
    if (!ctx_) {
        JS_FreeRuntime(rt_);
        rt_ = nullptr;
        throw std::runtime_error("JS_NewContext failed");
    }

    registerConsole();
    registerElementClass();
}

JsEngine::~JsEngine() {
    // Порядок обязателен: сначала context, потом runtime.
    if (ctx_) { JS_FreeContext(ctx_); ctx_ = nullptr; }
    if (rt_) { JS_FreeRuntime(rt_);  rt_ = nullptr; }
}

void JsEngine::setMemoryLimit(std::size_t bytes) {
    if (rt_) JS_SetMemoryLimit(rt_, bytes);
}

void JsEngine::setMaxStackSize(std::size_t bytes) {
    if (rt_) JS_SetMaxStackSize(rt_, bytes);
}

bool JsEngine::hasPendingJobs() const {
    return rt_ && JS_IsJobPending(rt_);
}

// ============================================================
//  console.*
// ============================================================

void JsEngine::registerConsole() {
    JSValue console = JS_NewObject(ctx_);

    // magic — маленькое целое, передаётся в C-функцию как константа.
    // Используем его, чтобы не плодить три похожие функции.
    JS_SetPropertyStr(ctx_, console, "log",
        JS_NewCFunctionMagic(ctx_, js_console_log, "log", 0,
            JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx_, console, "warn",
        JS_NewCFunctionMagic(ctx_, js_console_log, "warn", 0,
            JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx_, console, "error",
        JS_NewCFunctionMagic(ctx_, js_console_log, "error", 0,
            JS_CFUNC_generic_magic, 2));

    JSValue global = JS_GetGlobalObject(ctx_);
    JS_SetPropertyStr(ctx_, global, "console", console);
    JS_FreeValue(ctx_, global);
}

// ============================================================
//  Element class (заготовка под DOM bindings)
// ============================================================

void JsEngine::registerElementClass() {
    JS_NewClassID(rt_, &element_class_id_);

    JSClassDef def{};
    def.class_name = "Element";
    def.finalizer = [](JSRuntime*, JSValue) {
        // no-op. DOMNode владеет арена страницы, а не QuickJS.
        // Нужен только чтобы quickjs-ng создал opaque_class —
        // без него JS_SetClassProto не работает.
        };

    int rc = JS_NewClass(rt_, element_class_id_, &def);
    qDebug() << "[JsEngine] JS_NewClass rc =" << rc
        << "element_class_id_ =" << element_class_id_;
}

// ============================================================
//  installGlobals — точка входа для bindings.
//  Пока регистрирует только то, что доступно «из коробки».
//  Document/Element подключаются отдельным файлом позже.
// ============================================================

void JsEngine::installGlobals(Page* page, DOMNode* document_root) {
    page_ = page;

    // ВАЖНО: JS_NewClassID читает исходное значение *pclass_id,
    // чтобы продолжить нумерацию. Передавать неинициализированную
    // переменную = UB (получите мусорный ID и краш при JS_NewObjectClass).
    // page_class_id_ инициализирован 0 в заголовке — этого достаточно.
    JS_NewClassID(rt_, &page_class_id_);

    JSClassDef page_def{};
    page_def.class_name = "Page";
    JS_NewClass(rt_, page_class_id_, &page_def);

    JSValue p = JS_NewObjectClass(ctx_, page_class_id_);
    JS_SetOpaque(p, page);

    JSValue global = JS_GetGlobalObject(ctx_);
    JS_SetPropertyStr(ctx_, global, "__page", p);
    JS_FreeValue(ctx_, global);

    // Регистрируем Element прототип и Document.
    bindings::installElement(ctx_, element_class_id_, page);
    bindings::installDocument(ctx_, element_class_id_, page);

    (void)document_root;
}
// ============================================================
//  run
// ============================================================

bool JsEngine::run(const std::string& source, const std::string& filename) {
    if (source.empty()) return true;

    JSValue result = JS_Eval(ctx_,
        source.data(), source.size(),
        filename.c_str(),
        JS_EVAL_TYPE_GLOBAL);

    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx_);

        std::string msg;
        JSValue stack = JS_GetPropertyStr(ctx_, exc, "stack");
        if (!JS_IsUndefined(stack) && !JS_IsNull(stack)) {
            msg = jsToString(ctx_, stack);
        }
        else {
            msg = jsToString(ctx_, exc);
        }
        JS_FreeValue(ctx_, stack);

        qWarning().noquote()
            << "[js] error in" << QString::fromStdString(filename)
            << ":\n" << QString::fromStdString(msg);

        JS_FreeValue(ctx_, exc);
        JS_FreeValue(ctx_, result);
        return false;
    }

    JS_FreeValue(ctx_, result);
    return true;
}