#pragma once
#include "quickjs.h"

class Page;
struct DOMNode;

namespace bindings {

    // Регистрирует прототип класса "Element" со всеми его методами.
    void installElement(JSContext* ctx, JSClassID class_id, Page* page);

    // Создать JS-обёртку вокруг DOMNode.
    JSValue wrapNode(JSContext* ctx, JSClassID class_id, DOMNode* node);

    // Извлечь DOMNode* из JS-объекта (nullptr если это не Element).
    DOMNode* unwrapNode(JSContext* ctx, JSClassID class_id, JSValueConst obj);

    // Достать Page* из globalThis.__page (общий helper).
    Page* pageFromCtx(JSContext* ctx);

} // namespace bindings