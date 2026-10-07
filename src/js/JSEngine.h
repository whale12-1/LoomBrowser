#pragma once

#include <cstdint>
#include <string>

// QuickJS. Путь зависит от того, как подключён third_party/quickjs.
// Если CMake пробрасывает include-каталог — можно писать <quickjs.h>.
// См. раздел про CMake ниже.
extern "C" {
#include "quickjs.h"
}

// Forward — понадобится, когда будем ставить DOM-bindings.
class Page;
struct DOMNode;

// ============================================================
//  JsEngine — RAII-обёртка над QuickJS runtime + context.
//
//  Что делает:
//    * создаёт JSRuntime и JSContext в конструкторе
//    * освобождает их в деструкторе (порядок гарантирован)
//    * регистрирует console.log и (позже) DOM-глобалы
//    * запускает скрипты через run()
//    * предоставляет ctx() для внешних bindings
//
//  Что НЕ делает:
//    * не владеет DOM и ареной — это ответственность Page
//    * не реализует setTimeout, fetch, Promise jobs
//      (для них нужен свой event loop — отдельная задача)
// ============================================================
class JsEngine {
public:
    JsEngine();
    ~JsEngine();

    JsEngine(const JsEngine&) = delete;
    JsEngine& operator=(const JsEngine&) = delete;

    // Установка глобальных объектов.
    //   document_root — корень DOM-дерева (обычно #document),
    //                   используется Document/Element bindings.
    //   page          — владелец арены; bindings аллоцируют новые узлы
    //                   через Page::arena_.
    // Сейчас реализует только console.*; Document/Element подключаются
    // отдельным вызовом позже.
    void installGlobals(Page* page, DOMNode* document_root);

    // Выполнить скрипт. filename — для сообщений об ошибках
    // (показывается в консоли Qt при синтаксической/рантайм ошибке).
    // Возвращает false при ошибке; подробности пишет в qWarning.
    bool run(const std::string& source, const std::string& filename);

    // Есть ли необработанные Promise jobs.
    // Пока не используется — оставлено для будущего event loop.
    bool hasPendingJobs() const;

    // Управление лимитами (вызывать до первого run, иначе)
    // setMemoryLimit/setMaxStackSize не имеют эффекта.
    void setMemoryLimit(std::size_t bytes);
    void setMaxStackSize(std::size_t bytes);

    // Доступ для внешних bindings
    // Вызвать все listener'ы на узле (и на его предках) для события type.
// Пока реализация — в Page, но интерфейс удобно держать здесь.
    void dispatchEvent(DOMNode* /*target*/, const std::string& /*type*/);
    JSContext* ctx()        const { return ctx_; }
    JSRuntime* runtime()    const { return rt_; }
    JSClassID  elementClassId() const { return element_class_id_; }

private:
    void registerConsole();
    void registerElementClass();

    Page* page_ = nullptr;
    JSRuntime* rt_ = nullptr;
    JSContext* ctx_ = nullptr;

    JSClassID element_class_id_ = 0;
    JSClassID page_class_id_ = 0;    // ← добавить
};