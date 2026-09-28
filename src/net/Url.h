#pragma once
#include <cstdint>
#include <string>

namespace net {

    struct Url {
        // Каноническая строка целиком: "https://host:443/path?q=1#frag".
        std::string text;

        // Разобранные поля (заполняются parse()).
        std::string scheme;   // "http", "https", "file", "about", …
        std::string host;
        uint16_t    port = 0;         // 0 = default для схемы
        std::string path;             // "/foo/bar"
        std::string query;            // без '?'
        std::string fragment;         // без '#'

        bool valid()   const { return !text.empty() && !scheme.empty(); }
        bool isHttp()  const { return scheme == "http" || scheme == "https"; }
        bool isFile()  const { return scheme == "file"; }

        static Url normalize(const std::string& input);

        // Строгий разбор без добавления схемы. Пустое scheme = relative.
        static Url parse(const std::string& s);

        static constexpr uint16_t defaultPort(const std::string& scheme) {
            return scheme == "https" ? 443
                : scheme == "http" ? 80 : 0;
        }
    };

} // namespace net