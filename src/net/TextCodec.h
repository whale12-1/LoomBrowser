// TextCodec.h
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace net {

    // ƒекодирует сырые байты ответа в UTF-8. charset Ч из Content-Type.
    // ≈сли charset неизвестен или не поддерживаетс€ Ч fallback на UTF-8.
    std::string decodeToUtf8(const std::vector<uint8_t>& bytes,
        const std::string& charset);

} // namespace net