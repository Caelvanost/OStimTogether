#pragma once

#include "PCH.h"
#include "STRPMApi.h"

namespace OStimTogether
{
    class STRPMTransport
    {
    public:
        static STRPMTransport& GetSingleton();

        bool Start();
        void Stop();

        // Returns true only when STRPM accepted the payload. Callers may use
        // the legacy UDP transport as a fallback when this returns false.
        bool Send(std::string_view payload);

        [[nodiscard]] bool IsRunning() const noexcept
        {
            return _running.load();
        }

        [[nodiscard]] std::optional<RE::FormID> ResolveProxy(
            STRPMApi::ConnectionID connectionID) const;

    private:
        STRPMTransport() = default;
        ~STRPMTransport();

        STRPMTransport(const STRPMTransport&) = delete;
        STRPMTransport& operator=(const STRPMTransport&) = delete;

        static void __cdecl OnMessage(
            const STRPMApi::Message* message,
            void* userData);

        static void __cdecl OnProxyMapping(
            const STRPMApi::ProxyMappingEvent* event,
            void* userData);

        void HandleMessage(const STRPMApi::Message& message);
        void HandleProxyMapping(const STRPMApi::ProxyMappingEvent& event);

        static const char* ResultName(STRPMApi::Result result) noexcept;
        static const char* MappingEventName(
            STRPMApi::ProxyMappingEventType type) noexcept;

        const STRPMApi::Interface* _api{ nullptr };
        const STRPMApi::ProxyResolverInterface* _resolver{ nullptr };
        STRPMApi::ListenerHandle _listener{};
        std::atomic_bool _running{ false };
        bool _resolverListenerRegistered{ false };
    };
}