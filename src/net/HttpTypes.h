#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Url.h"

namespace net {

    using Headers = std::vector<std::pair<std::string, std::string>>;

    enum class HttpMethod : uint8_t { Get, Post, Put, Delete, Head, Patch };

    inline const char* methodName(HttpMethod m) {
        switch (m) {
        case HttpMethod::Get:    return "GET";
        case HttpMethod::Post:   return "POST";
        case HttpMethod::Put:    return "PUT";
        case HttpMethod::Delete: return "DELETE";
        case HttpMethod::Head:   return "HEAD";
        case HttpMethod::Patch:  return "PATCH";
        }
        return "GET";
    }

    struct HttpRequest {
        Url          url;
        HttpMethod   method = HttpMethod::Get;
        Headers      headers;                 // поверх дефолтных (UA, Accept)
        std::vector<uint8_t> body;            // дл€ POST/PUT/PATCH
        uint32_t     timeout_ms = 30'000;
        uint32_t     max_redirects = 10;
    };

    struct HttpResponse {
        int          status_code = 0;         // 200, 404, 500Е
        std::string  status_text;             // "OK"
        Url          final_url;               // с учЄтом redirects
        Headers      headers;                 // Set-Cookie, Content-Type и т.д.
        std::string  content_type;            // "text/html" (без charset)
        std::string  charset;                 // "utf-8" (разобранный из Content-Type)
        std::vector<uint8_t> body;            // сырые байты

        // ”тилита: первое значение заголовка по имени (case-insensitive).
        const std::string* findHeader(const std::string& name) const;
    };

} // namespace net