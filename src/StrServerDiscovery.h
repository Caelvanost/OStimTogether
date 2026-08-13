#pragma once

#include "PCH.h"
#include "Config.h"

namespace OStimTogether::StrServerDiscovery
{
    struct ClientState
    {
        std::optional<Config::RemotePeer> remotePeer;
        std::optional<std::string> password;
        std::string rawAddress;
    };

    ClientState ReadClientState(
        std::uint16_t ostimTogetherPort);

    std::optional<std::string>
        ReadServerPasswordFromConfig();
}
