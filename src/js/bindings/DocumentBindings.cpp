#include "DocumentBindings.h"
#include "ElementBindings.h"
#include "Page.h"                                    // ← было "style/Page.h"

#include "parsers/css_parser/headers/css_parser.h"
#include "parsers/selector_matcher/selector_matcher.h"
#include "parsers/arena_memory_allocator/headers/arena.h"
#include <QDebug>

#include <algorithm>
#include <string_view>
#include <memory_resource>
#include <cctype>
#include <string>
#include <vector>

namespace bindings {

    // pageFromCtx — берём из ElementBindings (экспортирован в .h).
    // Здесь его НЕ переопределяем, чтобы не было дубликата символа.

    namespace {

        // Разбираем строку-селектор через полноценный CSSParser.
        // Комплексный селектор копируется в heap — можно спокойно
        // использовать после выхода из функции.
        bool parseSelector(const std::string& sel, ComplexSelector& out) {
            ArenaAllocator local;
            CSSParser cp(local);
            StyleSheet* sheet = cp.parse(sel + "{}");
            if (!sheet || sheet->rules.empty()) return false;
            if (sheet->rules[0].selectors.empty()) return false;
            out = sheet->rules[0].selectors[0];
            return true;
        }

        // ---------- общие предикаты ----------
        struct FindByIdCtx { std::string id; };

        inline QString qs_from_pmr(const std::pmr::string& s) {
            return QString::fromUtf8(s.data(), int(s.size()));
        }

        bool matchId(const DOMNode* n, void* vctx) {
            auto* c = static_cast<FindByIdCtx*>(vctx);
            for (const auto& kv : n->attributes) {
                std::string_view k(kv.first.data(), kv.first.size());
                if (k != "id") continue;

                std::string_view v(kv.second.data(), kv.second.size());
                const bool match = (v == c->id);

                // диагностика — удалить после проверки
                qDebug() << "[matchId] looking for" << QString::fromStdString(c->id)
                    << "node" << qs_from_pmr(n->tag_name)
                    << "has id =" << QString::fromUtf8(v.data(), int(v.size()))
                    << "match =" << match;
                return match;
            }
            return false;
        }

        struct QueryCtx {
            const ComplexSelector* sel;
            std::vector<DOMNode*>* out;   // nullptr для querySelector
            DOMNode* first = nullptr;
        };

        bool matchQuery(const DOMNode* n, void* vctx) {
            auto* c = static_cast<QueryCtx*>(vctx);
            if (SelectorMatcher::match_complex(const_cast<DOMNode*>(n), *c->sel)) {
                if (c->out) c->out->push_back(const_cast<DOMNode*>(n));
                else        c->first = const_cast<DOMNode*>(n);
                return c->out == nullptr;   // для первого совпадения — стоп
            }
            return false;
        }

        bool matchBody(const DOMNode* n, void*) {
            return n->tag_name == "body";
        }

        // ---------- doc.* ----------
        JSValue doc_getElementById(JSContext* ctx, JSValueConst /*tv*/,
            int argc, JSValueConst* argv,
            JSClassID class_id)
        {
            if (argc < 1) return JS_NULL;
            Page* page = pageFromCtx(ctx);
            if (!page) return JS_NULL;

            const char* id = JS_ToCString(ctx, argv[0]);
            if (!id) return JS_NULL;
            FindByIdCtx c{ id };
            JS_FreeCString(ctx, id);

            DOMNode* found = page->findFirst(page->document(), matchId, &c);
            return wrapNode(ctx, class_id, found);
        }

        JSValue doc_querySelector(JSContext* ctx, JSValueConst /*tv*/,
            int argc, JSValueConst* argv,
            JSClassID class_id)
        {
            if (argc < 1) return JS_NULL;
            Page* page = pageFromCtx(ctx);
            if (!page) return JS_NULL;

            const char* s = JS_ToCString(ctx, argv[0]);
            if (!s) return JS_NULL;
            ComplexSelector sel;
            const bool ok = parseSelector(s, sel);
            JS_FreeCString(ctx, s);
            if (!ok) return JS_NULL;

            QueryCtx c{ &sel, nullptr };
            page->findFirst(page->document(), matchQuery, &c);
            return c.first ? wrapNode(ctx, class_id, c.first) : JS_NULL;
        }

        JSValue doc_querySelectorAll(JSContext* ctx, JSValueConst /*tv*/,
            int argc, JSValueConst* argv,
            JSClassID class_id)
        {
            JSValue arr = JS_NewArray(ctx);
            if (argc < 1) return arr;
            Page* page = pageFromCtx(ctx);
            if (!page) return arr;

            const char* s = JS_ToCString(ctx, argv[0]);
            if (!s) return arr;
            ComplexSelector sel;
            const bool ok = parseSelector(s, sel);
            JS_FreeCString(ctx, s);
            if (!ok) return arr;

            std::vector<DOMNode*> found;
            QueryCtx c{ &sel, &found };
            page->collectAll(page->document(), matchQuery, &c, found);

            for (size_t i = 0; i < found.size(); ++i)
                JS_SetPropertyUint32(ctx, arr, (uint32_t)i,
                    wrapNode(ctx, class_id, found[i]));
            return arr;
        }

        JSValue doc_createElement(JSContext* ctx, JSValueConst /*tv*/,
            int argc, JSValueConst* argv,
            JSClassID class_id)
        {
            if (argc < 1) return JS_NULL;
            Page* page = pageFromCtx(ctx);
            if (!page) return JS_NULL;

            const char* tag = JS_ToCString(ctx, argv[0]);
            if (!tag) return JS_NULL;

            DOMNode* n = page->arena().Alloc<DOMNode>();
            n->type = NodeType::Element;
            n->tag_name = tag;
            for (char& c : n->tag_name) c = (char)std::tolower((unsigned char)c);

            JS_FreeCString(ctx, tag);
            return wrapNode(ctx, class_id, n);
        }

        // getter'у не нужны argc/argv — только ctx, this_val, class_id.
        JSValue doc_get_body(JSContext* ctx, JSValueConst /*tv*/, JSClassID class_id) {
            Page* page = pageFromCtx(ctx);
            if (!page) return JS_NULL;
            DOMNode* b = page->findFirst(page->document(), matchBody, nullptr);
            return b ? wrapNode(ctx, class_id, b) : JS_NULL;
        }

        // ---------- обёртки с чтением class_id из глобала ----------
        // Trick: JS_NewCFunctionMagic принимает только int magic. Хранить
        // class_id в int можно, но лучше — в __elementClassId, чтобы не
        // пересекаться с magic для других целей.
        JSClassID readClassId(JSContext* ctx) {
            JSValue g = JS_GetGlobalObject(ctx);
            JSValue cidv = JS_GetPropertyStr(ctx, g, "__elementClassId");
            int32_t cid = 0;
            JS_ToInt32(ctx, &cid, cidv);
            JS_FreeValue(ctx, cidv);
            JS_FreeValue(ctx, g);
            return static_cast<JSClassID>(cid);
        }

        JSValue doc_body_getter(JSContext* ctx, JSValueConst tv, int /*magic*/) {
            return doc_get_body(ctx, tv, readClassId(ctx));
        }

    } // namespace


    // ============================================================
    //  installDocument
    // ============================================================
    void installDocument(JSContext* ctx, JSClassID class_id, Page* /*page*/) {
        JSValue global = JS_GetGlobalObject(ctx);

        // Кладём class_id в глобал — чтобы обёртки ниже могли его прочитать.
        JS_SetPropertyStr(ctx, global, "__elementClassId",
            JS_NewInt32(ctx, int(class_id)));

        JSValue doc = JS_NewObject(ctx);

        JS_SetPropertyStr(ctx, doc, "getElementById",
            JS_NewCFunctionMagic(ctx, [](JSContext* ctx, JSValueConst tv,
                int argc, JSValueConst* argv, int)
                {
                    return doc_getElementById(ctx, tv, argc, argv, readClassId(ctx));
                }, "getElementById", 1, JS_CFUNC_generic_magic, 0));

        JS_SetPropertyStr(ctx, doc, "querySelector",
            JS_NewCFunctionMagic(ctx, [](JSContext* ctx, JSValueConst tv,
                int argc, JSValueConst* argv, int)
                {
                    return doc_querySelector(ctx, tv, argc, argv, readClassId(ctx));
                }, "querySelector", 1, JS_CFUNC_generic_magic, 0));

        JS_SetPropertyStr(ctx, doc, "querySelectorAll",
            JS_NewCFunctionMagic(ctx, [](JSContext* ctx, JSValueConst tv,
                int argc, JSValueConst* argv, int)
                {
                    return doc_querySelectorAll(ctx, tv, argc, argv, readClassId(ctx));
                }, "querySelectorAll", 1, JS_CFUNC_generic_magic, 0));

        JS_SetPropertyStr(ctx, doc, "createElement",
            JS_NewCFunctionMagic(ctx, [](JSContext* ctx, JSValueConst tv,
                int argc, JSValueConst* argv, int)
                {
                    return doc_createElement(ctx, tv, argc, argv, readClassId(ctx));
                }, "createElement", 1, JS_CFUNC_generic_magic, 0));

        JSAtom bodyAtom = JS_NewAtom(ctx, "body");
        JS_DefinePropertyGetSet(ctx, doc, bodyAtom,
            JS_NewCFunction2(ctx, (JSCFunction*)doc_body_getter,
                "get body", 0, JS_CFUNC_getter_magic, 0),
            JS_UNDEFINED,
            JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, bodyAtom);

        JS_SetPropertyStr(ctx, global, "document", doc);
        JS_FreeValue(ctx, global);
    }

} // namespace bindings