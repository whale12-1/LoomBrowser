// net/HttpTypes.cpp
#include "HttpTypes.h"
#include <algorithm>
#include <cctype>

namespace net {

    static bool iequals(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
                return false;
        return true;
    }

    const std::string* HttpResponse::findHeader(const std::string& name) const {
        for (const auto& [k, v] : headers)
            if (iequals(k, name)) return &v;
        return nullptr;
    }

} // namespace net