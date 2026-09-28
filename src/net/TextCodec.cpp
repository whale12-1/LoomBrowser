// TextCodec.cpp
#include "TextCodec.h"

#include <QStringDecoder>

#include <algorithm>
#include <cctype>

namespace net {

    std::string decodeToUtf8(const std::vector<uint8_t>& bytes,
        const std::string& charset_in)
    {
        if (bytes.empty()) return {};

        std::string cs = charset_in;
        for (char& c : cs) c = (char)std::tolower((unsigned char)c);

        if (cs.empty() || cs == "utf-8" || cs == "utf8") {
            return std::string(reinterpret_cast<const char*>(bytes.data()),
                bytes.size());
        }

        auto decoder = QStringDecoder(cs.c_str());
        if (!decoder.isValid()) {
            // Не смогли — попробуем UTF-8, что бы там ни было.
            return std::string(reinterpret_cast<const char*>(bytes.data()),
                bytes.size());
        }

        const QByteArray ba(reinterpret_cast<const char*>(bytes.data()),
            qsizetype(bytes.size()));
        const QString s = decoder.decode(ba);
        const QByteArray utf8 = s.toUtf8();
        return std::string(utf8.constData(), size_t(utf8.size()));
    }

} // namespace net