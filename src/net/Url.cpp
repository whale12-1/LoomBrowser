// net/Url.cpp
#include "Url.h"
#include <algorithm>
#include <cctype>

namespace net {

    static std::string toLowerCopy(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    static std::string trim(const std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace((unsigned char)s[a])) ++a;
        while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
        return s.substr(a, b - a);
    }

    static bool looksLikeHost(const std::string& s) {
        return !s.empty()
            && s.find(' ') == std::string::npos
            && s.find('.') != std::string::npos;
    }

    static std::string percentEncodeQuery(const std::string& s) {
        static const char* hex = "0123456789ABCDEF";
        std::string out;
        out.reserve(s.size() * 3);
        for (unsigned char c : s) {
            const bool safe =
                std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
            if (safe) out += (char)c;
            else {
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 0xF];
            }
        }
        return out;
    }

    Url Url::parse(const std::string& s) {
        Url u;
        u.text = s;

        const size_t scheme_end = s.find("://");
        if (scheme_end == std::string::npos) {
            // ќтносительный путь или голый host.
            u.path = s;
            return u;
        }
        u.scheme = toLowerCopy(s.substr(0, scheme_end));

        const size_t rest = scheme_end + 3;   // после "://"
        const size_t auth_end = s.find_first_of("/?#", rest);
        const std::string authority = (auth_end == std::string::npos)
            ? s.substr(rest)
            : s.substr(rest, auth_end - rest);

        // host[:port]
        const size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            u.host = authority.substr(0, colon);
            try { u.port = (uint16_t)std::stoi(authority.substr(colon + 1)); }
            catch (...) { u.port = 0; }
        }
        else {
            u.host = authority;
        }

        // path?query#fragment
        if (auth_end != std::string::npos) {
            const size_t path_end = s.find_first_of("?#", auth_end);
            u.path = (path_end == std::string::npos)
                ? s.substr(auth_end)
                : s.substr(auth_end, path_end - auth_end);

            if (path_end != std::string::npos) {
                if (s[path_end] == '?') {
                    const size_t frag = s.find('#', path_end);
                    u.query = (frag == std::string::npos)
                        ? s.substr(path_end + 1)
                        : s.substr(path_end + 1, frag - path_end - 1);
                    if (frag != std::string::npos) u.fragment = s.substr(frag + 1);
                }
                else {
                    u.fragment = s.substr(path_end + 1);
                }
            }
        }

        if (u.path.empty()) u.path = "/";
        if (u.port == 0)     u.port = Url::defaultPort(u.scheme);
        return u;
    }

    Url Url::normalize(const std::string& input) {
        const std::string s = trim(input);
        if (s.empty()) return {};

        // ”же с €вной схемой?
        if (s.find("://") != std::string::npos) return Url::parse(s);

        // Ћокальный файл
        if (!s.empty() && (s[0] == '/' ||
            s.rfind("./", 0) == 0 ||
            s.rfind("../", 0) == 0))
        {
            Url u;
            u.scheme = "file";
            u.path = s;
            u.text = "file://" + s;
            return u;
        }

        // ѕохоже на хост Ч префиксуем https://
        if (looksLikeHost(s)) return Url::parse("https://" + s);

        // »наче Ч поисковый запрос
        return Url::parse("https://duckduckgo.com/?q=" + percentEncodeQuery(s));
    }

} // namespace net