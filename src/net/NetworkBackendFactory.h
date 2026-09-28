// NetworkBackendFactory.h
#pragma once
#include <memory>
#include <string>
#include "NetworkBackend.h"

namespace net {

    // Выбор бэкенда по имени: "qt" (default). Можно расширять: "curl", "dummy".
    std::unique_ptr<NetworkBackend> makeNetworkBackend(
        const std::string& name = "qt");

    // Список доступных бэкендов (для CLI-флага --net-backend=list).
    std::string listAvailableBackends();

} // namespace net