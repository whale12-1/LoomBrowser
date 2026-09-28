#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "HttpTypes.h"
#include "Url.h"

namespace net {

    // Callbacks, которые бэкенд дёргает по ходу загрузки.
    // Все — опциональные. Вызываются в «главном» потоке бэкенда.
    struct NetworkCallbacks {
        std::function<void(const Url& url)>                 onStarted;
        std::function<void(uint64_t received, uint64_t total)> onProgress;
        std::function<void(const HttpResponse& resp)>       onResponse;
        std::function<void(const Url& url, int status, const std::string& error)> onFailure;
    };

    // Абстрактный сетевой бэкенд. Один запрос в единицу времени —
    // каждый новый вызов get/request отменяет предыдущий.
    //
    // Жизненный цикл: создать → setCallbacks → get/request → callbacks.
    // Не потокобезопасен; предполагает использование из одного потока (UI).
    class NetworkBackend {
    public:
        virtual ~NetworkBackend() = default;

        // Установить callbacks (можно вызывать один раз или перед каждым запросом).
        virtual void setCallbacks(NetworkCallbacks cbs) = 0;

        // Асинхронный GET. Отменяет предыдущий незавершённый запрос.
        virtual void get(const Url& url) = 0;

        // Общий случай (метод, headers, body).
        virtual void request(const HttpRequest& req) = 0;

        // Отменить текущий запрос, если он в процессе.
        virtual void abort() = 0;

        // Управление поведением по умолчанию.
        virtual void setUserAgent(const std::string& ua) = 0;
        virtual void setCacheSize(uint64_t bytes) = 0;

        // Идентификатор бэкенда для логов: "qt", "curl", "dummy"…
        virtual const char* backendName() const = 0;
    };

} // namespace net