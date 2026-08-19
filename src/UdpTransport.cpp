#include "PCH.h"
#include "UdpTransport.h"

#include "STRPMTransport.h"

namespace OStimTogether
{
    UdpTransport& UdpTransport::GetSingleton()
    {
        static UdpTransport instance;
        return instance;
    }

    bool UdpTransport::Start()
    {
        _running.store(true);
        SKSE::log::info(
            "OSTNET legacy UdpTransport facade enabled; backend=STRPM-only udp=disabled");
        return true;
    }

    void UdpTransport::Stop()
    {
        _running.store(false);
    }

    void UdpTransport::Send(std::string_view payload)
    {
        if (payload.empty()) {
            return;
        }

        if (!STRPMTransport::GetSingleton().Send(payload)) {
            SKSE::log::warn(
                "OSTNET STRPM-only TX dropped: STRPM unavailable/not connected bytes={}",
                payload.size());
        }
    }
}
