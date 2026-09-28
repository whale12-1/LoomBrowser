// NetworkBackendFactory.cpp
#include "NetworkBackendFactory.h"

#include "qt/QtNetworkBackend.h"

namespace net {

    std::unique_ptr<NetworkBackend> makeNetworkBackend(const std::string& name) {
        if (name.empty() || name == "qt") {
            return std::make_unique<QtNetworkBackend>();
        }
        // Будущее: "curl" → make_unique<CurlNetworkBackend>();
        return std::make_unique<QtNetworkBackend>();
    }

    std::string listAvailableBackends() {
        return "qt";
    }

} // namespace net