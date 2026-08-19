#pragma once

#include "PCH.h"

namespace OStimTogether
{
    // Compatibility facade retained temporarily so existing addon code paths
    // do not need to change in the same commit. It performs NO UDP/network
    // work: all sends are delegated exclusively to STRPMTransport.
    class UdpTransport
    {
    public:
        static UdpTransport& GetSingleton();

        bool Start();
        void Stop();
        void Send(std::string_view payload);

        [[nodiscard]] bool IsRunning() const noexcept
        {
            return _running.load();
        }

    private:
        UdpTransport() = default;
        std::atomic_bool _running{ false };
    };
}
