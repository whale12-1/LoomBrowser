#pragma once
#include "quickjs.h"

class Page;

namespace bindings {
	void installDocument(JSContext* ctx, JSClassID class_id, Page* page);
}