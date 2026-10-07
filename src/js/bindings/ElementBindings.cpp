#include "ElementBindings.h"
#include "Page.h"
#include "parsers/html_parser/headers/dom.h"

#include <QDebug>
#include <algorithm>
#include <cctype>
#include <functional>
#include <string>
#include <vector>

namespace bindings {

    // ============================================================
    //  wrap / unwrap / pageFromCtx — экспортируемые хелперы
    // ============================================================

    DOMNode* unwrapNode(JSContext* ctx, JSClassID class_id, JSValueConst obj) {
        if (!JS_IsObject(obj)) return nullptr;
        if (JS_GetClassID(obj) != class_id) return nullptr;
        return static_cast<DOMNode*>(JS_GetOpaque(obj, class_id));
    }

    JSValue wrapNode(JSContext* ctx, JSClassID class_id, DOMNode* node) {
        if (!node) return JS_NULL;
        JSValue obj = JS_NewObjectClass(ctx, class_id);
        if (JS_IsException(obj)) return obj;
        JS_SetOpaque(obj, node);
        return obj;
    }

    Page* pageFromCtx(JSContext* ctx) {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue p = JS_GetPropertyStr(ctx, global, "__page");
        Page* page = nullptr;
        if (JS_IsObject(p)) {
            page = static_cast<Page*>(JS_GetOpaque(p, JS_GetClassID(p)));
        }
        JS_FreeValue(ctx, p);
        JS_FreeValue(ctx, global);
        return page;
    }

    // ============================================================
    //  Локальные helper'ы
    // ============================================================
    namespace {

        std::string collectText(const DOMNode* n) {
            std::string out;
            std::function<void(const DOMNode*)> walk = [&](const DOMNode* x) {
                if (!x) return;
                if (x->type == NodeType::Text) out += x->text_content;
                for (const DOMNode* c : x->children) walk(c);
                };
            walk(n);
            return out;
        }

        bool thisIsElement(JSContext* ctx, JSValueConst this_val,
            DOMNode*& n, Page*& page) {
            n = unwrapNode(ctx, JS_GetClassID(this_val), this_val);
            if (!n) return false;
            page = pageFromCtx(ctx);
            return page != nullptr;
        }

        // ---------- textContent ----------
        JSValue get_textContent(JSContext* ctx, JSValueConst this_val) {
            DOMNode* n = nullptr; Page* page = nullptr;
            if (!thisIsElement(ctx, this_val, n, page)) return JS_NULL;
            const std::string s = collectText(n);
            return JS_NewStringLen(ctx, s.data(), s.size());
        }

        JSValue set_textContent(JSContext* ctx, JSValueConst this_val,
            JSValueConst val) {
            DOMNode* n = nullptr; Page* page = nullptr;
            if (!thisIsElement(ctx, this_val, n, page)) return JS_UNDEFINED;

            const char* s = JS_ToCString(ctx, val);
            const std::string text = s ? s : "";
            if (s) JS_FreeCString(ctx, s);

            n->children.clear();
            n->text_content.clear();
            DOMNode* t = page->arena().Alloc<DOMNode>();
            t->type = NodeType::Text;
            t->text_content = text;
            n->add_child(t);

            page->markDirty();
            return JS_UNDEFINED;
        }

        // ---------- id / tagName ----------
        JSValue get_id(JSContext* ctx, JSValueConst this_val) {
            DOMNode* n = nullptr; Page* page = nullptr;
            if (!thisIsElement(ctx, this_val, n, page)) return JS_NULL;
            auto it = n->attributes.find("id");
            if (it == n->attributes.end()) return JS_NewString(ctx, "");
            return JS_NewStringLen(ctx, it->second.data(), it->second.size());
        }

        JSValue get_tagName(JSContext* ctx, JSValueConst this_val) {
            DOMNode* n = nullptr; Page* page = nullptr;
            if (!thisIsElement(ctx, this_val, n, page)) return JS_NULL;
            std::string t = n->tag_name;
            for (char& c : t) c = (char)std::toupper((unsigned char)c);
            return JS_NewStringLen(ctx, t.data(), t.size());
        }

        // ---------- attributes ----------
        JSValue method_setAttribute(JSContext* ctx, JSValueConst this_val,
            int argc, JSValueConst* argv, int /*magic*/) {
            DOMNode* n = nullptr; Page* page = nullptr;
            if (!thisIsElement(ctx, this_val, n, page)) return JS_UNDEFINED;
            if (argc < 2) return JS_UNDEFINED;

            const char* k = JS_ToCString(ctx, argv[0]);
            const char* v = JS_ToCString(ctx, argv[1]);
            if (k && v) n->attributes[k] = v;
            if (k) JS_FreeCString(ctx, k);
            if (v) JS_FreeCString(ctx, v);

            page->markDirty();
            return JS_UNDEFINED;
        }

        JSValue method_getAttribute(JSContext* ctx, JSValueConst this_val,
            int argc, JSValueConst* argv, int /*magic*/) {
            DOMNode* n = nullptr; Page* page = nullptr;
            if (!thisIsElement(ctx, this_val, n, page)) return JS_NULL;
            if (argc < 1) return JS_NULL;

            const char* k = JS_ToCString(ctx, argv[0]);
            if (!k) return JS_NULL;
            auto it = n->attributes.find(k);
            JS_FreeCString(ctx, k);
            if (it == n->attributes.end()) return JS_NULL;
            return JS_NewStringLen(ctx, it->second.data(), it->second.size());
        }

        JSValue method_hasAttribute(JSContext* ctx, JSValueConst this_val,
            int argc, JSValueConst* argv, int /*magic*/) {
            DOMNode* n = nullptr; Page* page = nullptr;
            if (!thisIsElement(ctx, this_val, n, page)) return JS_FALSE;
            if (argc < 1) return JS_FALSE;
            const char* k = JS_ToCString(ctx, argv[0]);
            if (!k) return JS_FALSE;
            const bool has = n->attributes.find(k) != n->attributes.end();
            JS_FreeCString(ctx, k);
            return has ? JS_TRUE : JS_FALSE;
        }

        JSValue method_removeAttribute(JSContext* ctx, JSValueConst this_val,
            int argc, JSValueConst* argv, int /*magic*/) {
            DOMNode* n = nullptr; Page* page = nullptr;
            if (!thisIsElement(ctx, this_val, n, page)) return JS_UNDEFINED;
            if (argc < 1) return JS_UNDEFINED;
            const char* k = JS_ToCString(ctx, argv[0]);
            if (k) { n->attributes.erase(k); JS_FreeCString(ctx, k); }
            page->markDirty();
            return JS_UNDEFINED;
        }

        // ---------- classList ----------
        JSValue method_classList_op(JSContext* ctx, JSValueConst this_val,
            int argc, JSValueConst* argv, int magic) {
            DOMNode* n = nullptr; Page* page = nullptr;
            if (!thisIsElement(ctx, this_val, n, page)) return JS_UNDEFINED;
            if (argc < 1) return JS_UNDEFINED;

            const char* cls = JS_ToCString(ctx, argv[0]);
            if (!cls) return JS_UNDEFINED;
            const std::string name(cls);
            JS_FreeCString(ctx, cls);

            std::string& cl = n->attributes["class"];
            std::vector<std::string> parts;
            size_t i = 0;
            while (i < cl.size()) {
                while (i < cl.size() && std::isspace((unsigned char)cl[i])) ++i;
                size_t s = i;
                while (i < cl.size() && !std::isspace((unsigned char)cl[i])) ++i;
                if (i > s) parts.push_back(cl.substr(s, i - s));
            }

            auto findIt = std::find(parts.begin(), parts.end(), name);
            bool result = false;
            switch (magic) {
            case 0: if (findIt == parts.end()) parts.push_back(name); result = true; break;
            case 1: if (findIt != parts.end()) parts.erase(findIt); break;
            case 2:
                if (findIt != parts.end()) { parts.erase(findIt); result = false; }
                else { parts.push_back(name); result = true; }
                break;
            case 3: result = (findIt != parts.end()); break;
            }

            cl.clear();
            for (size_t k = 0; k < parts.size(); ++k) {
                if (k) cl += ' ';
                cl += parts[k];
            }
            if (magic != 3) page->markDirty();
            return JS_NewBool(ctx, result);
        }

        // ---------- style ----------
        JSValue method_style_setProperty(JSContext* ctx, JSValueConst this_val,
            int argc, JSValueConst* argv, int /*magic*/) {
            JSValue owner = JS_GetPropertyStr(ctx, this_val, "__owner");
            DOMNode* n = nullptr; Page* page = nullptr;
            const bool ok = thisIsElement(ctx, owner, n, page);
            JS_FreeValue(ctx, owner);
            if (!ok) return JS_UNDEFINED;
            if (argc < 2) return JS_UNDEFINED;

            const char* p = JS_ToCString(ctx, argv[0]);
            const char* v = JS_ToCString(ctx, argv[1]);
            if (p && v) {
                std::string& style = n->attributes["style"];
                if (!style.empty() && style.back() != ';') style += ';';
                style += ' '; style += p; style += ": "; style += v; style += ';';
            }
            if (p) JS_FreeCString(ctx, p);
            if (v) JS_FreeCString(ctx, v);
            page->markDirty();
            return JS_UNDEFINED;
        }

        JSValue get_style(JSContext* ctx, JSValueConst this_val) {
            JSValue obj = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, obj, "__owner", JS_DupValue(ctx, this_val));
            JS_SetPropertyStr(ctx, obj, "setProperty",
                JS_NewCFunctionMagic(ctx, method_style_setProperty,
                    "setProperty", 2,
                    JS_CFUNC_generic_magic, 0));
            return obj;
        }

        // ---------- addEventListener ----------
        JSValue method_addEventListener(JSContext* ctx, JSValueConst this_val,
            int argc, JSValueConst* argv, int /*magic*/) {
            if (argc < 2) return JS_UNDEFINED;

            DOMNode* n = nullptr; Page* page = nullptr;
            if (!thisIsElement(ctx, this_val, n, page)) return JS_UNDEFINED;

            const char* t = JS_ToCString(ctx, argv[0]);
            if (!t) return JS_UNDEFINED;
            const std::string type(t);
            JS_FreeCString(ctx, t);

            if (!JS_IsFunction(ctx, argv[1])) return JS_UNDEFINED;
            page->addListener(n, type, argv[1]);
            return JS_UNDEFINED;
        }

    } // namespace

    // ============================================================
    //  installElement
    // ============================================================
    void installElement(JSContext* ctx, JSClassID class_id, Page* /*page*/) {
        JSValue proto = JS_NewObject(ctx);

        // getters
        JS_SetPropertyStr(ctx, proto, "id",
            JS_NewCFunction2(ctx, (JSCFunction*)get_id, "id", 0,
                JS_CFUNC_getter, 0));
        JS_SetPropertyStr(ctx, proto, "tagName",
            JS_NewCFunction2(ctx, (JSCFunction*)get_tagName, "tagName", 0,
                JS_CFUNC_getter, 0));
        JS_SetPropertyStr(ctx, proto, "style",
            JS_NewCFunction2(ctx, (JSCFunction*)get_style, "style", 0,
                JS_CFUNC_getter, 0));

        // методы — все через JS_NewCFunctionMagic + JS_CFUNC_generic_magic
        JS_SetPropertyStr(ctx, proto, "setAttribute",
            JS_NewCFunctionMagic(ctx, method_setAttribute, "setAttribute", 2,
                JS_CFUNC_generic_magic, 0));
        JS_SetPropertyStr(ctx, proto, "getAttribute",
            JS_NewCFunctionMagic(ctx, method_getAttribute, "getAttribute", 1,
                JS_CFUNC_generic_magic, 0));
        JS_SetPropertyStr(ctx, proto, "hasAttribute",
            JS_NewCFunctionMagic(ctx, method_hasAttribute, "hasAttribute", 1,
                JS_CFUNC_generic_magic, 0));
        JS_SetPropertyStr(ctx, proto, "removeAttribute",
            JS_NewCFunctionMagic(ctx, method_removeAttribute, "removeAttribute", 1,
                JS_CFUNC_generic_magic, 0));
        JS_SetPropertyStr(ctx, proto, "addEventListener",
            JS_NewCFunctionMagic(ctx, method_addEventListener, "addEventListener", 2,
                JS_CFUNC_generic_magic, 0));

        // classList
        JSValue classList = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, classList, "add",
            JS_NewCFunctionMagic(ctx, method_classList_op, "add", 1,
                JS_CFUNC_generic_magic, 0));
        JS_SetPropertyStr(ctx, classList, "remove",
            JS_NewCFunctionMagic(ctx, method_classList_op, "remove", 1,
                JS_CFUNC_generic_magic, 1));
        JS_SetPropertyStr(ctx, classList, "toggle",
            JS_NewCFunctionMagic(ctx, method_classList_op, "toggle", 1,
                JS_CFUNC_generic_magic, 2));
        JS_SetPropertyStr(ctx, classList, "contains",
            JS_NewCFunctionMagic(ctx, method_classList_op, "contains", 1,
                JS_CFUNC_generic_magic, 3));
        JS_SetPropertyStr(ctx, proto, "classList", classList);

        // textContent: getter/setter одним вызовом.
        // ВАЖНО: quickjs-ng не читает {get,set} из объекта — getter и setter
        // идут отдельными параметрами, а флаг JS_PROP_GETSET + HAS_GET/HAS_SET
        // говорит "используй эти два указателя как accessor".
        JSAtom tc = JS_NewAtom(ctx, "textContent");

        JSValue getter = JS_NewCFunction2(ctx, (JSCFunction*)get_textContent,
            "get textContent", 0,
            JS_CFUNC_getter, 0);
        JSValue setter = JS_NewCFunction2(ctx, (JSCFunction*)set_textContent,
            "set textContent", 1,
            JS_CFUNC_setter, 0);

        int rc = JS_DefineProperty(ctx, proto, tc,
            JS_UNDEFINED,          // val — игнорируется при GETSET
            getter,                // getter
            setter,                // setter
            JS_PROP_GETSET
            | JS_PROP_HAS_GET
            | JS_PROP_HAS_SET
            | JS_PROP_CONFIGURABLE
            | JS_PROP_HAS_CONFIGURABLE);

        qDebug() << "[installElement] textContent define rc =" << rc;
        JS_FreeValue(ctx, getter);
        JS_FreeValue(ctx, setter);
        JS_FreeAtom(ctx, tc);

        JS_SetClassProto(ctx, class_id, proto);
    }

} // namespace bindings