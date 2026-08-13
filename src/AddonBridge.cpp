#include "PCH.h"
#include "AddonBridge.h"

#include "ActorResolver.h"
#include "PapyrusAnimationBridge.h"
#include "RaceMenuOverlayBridge.h"
#include "UdpTransport.h"

namespace OStimTogether
{
    namespace
    {
        constexpr std::string_view kAddonEventName =
            "ostimtogether_addon";

        bool EqualsInsensitive(
            std::string_view lhs,
            std::string_view rhs)
        {
            if (lhs.size() != rhs.size()) {
                return false;
            }

            return std::equal(
                lhs.begin(),
                lhs.end(),
                rhs.begin(),
                [](char a, char b) {
                    return std::tolower(
                               static_cast<unsigned char>(a)) ==
                           std::tolower(
                               static_cast<unsigned char>(b));
                });
        }

        bool IsSafeAddonToken(std::string_view value)
        {
            if (value.empty() || value.size() > 128) {
                return false;
            }

            return std::all_of(
                value.begin(),
                value.end(),
                [](char ch) {
                    const auto u =
                        static_cast<unsigned char>(ch);
                    return std::isalnum(u) ||
                           ch == '_' ||
                           ch == '-' ||
                           ch == '.';
                });
        }
    }

    AddonBridge& AddonBridge::GetSingleton()
    {
        static AddonBridge singleton;
        return singleton;
    }

    void AddonBridge::Register()
    {
        if (_registered.exchange(true)) {
            return;
        }

        auto* source =
            SKSE::GetModCallbackEventSource();

        if (!source) {
            _registered.store(false);
            SKSE::log::warn(
                "OSTNET ADDON bridge: ModCallbackEvent source unavailable");
            return;
        }

        source->AddEventSink(this);

        SKSE::log::info(
            "OSTNET ADDON bridge READY event={}",
            kAddonEventName);
    }

    std::vector<std::string> AddonBridge::Split(
        std::string_view text,
        char delimiter)
    {
        std::vector<std::string> result;
        std::size_t start = 0;

        while (start <= text.size()) {
            const auto pos =
                text.find(delimiter, start);

            if (pos == std::string_view::npos) {
                result.emplace_back(
                    text.substr(start));
                break;
            }

            result.emplace_back(
                text.substr(start, pos - start));
            start = pos + 1;
        }

        return result;
    }

    std::optional<std::string> AddonBridge::Field(
        std::string_view payload,
        std::string_view key)
    {
        const std::string needle =
            fmt::format("{}=", key);

        std::size_t searchFrom = 0;
        while (searchFrom < payload.size()) {
            const auto begin =
                payload.find(needle, searchFrom);

            if (begin == std::string_view::npos) {
                return std::nullopt;
            }

            if (begin == 0 ||
                payload[begin - 1] == '|') {
                const auto valueBegin =
                    begin + needle.size();
                const auto end =
                    payload.find('|', valueBegin);

                if (end == std::string_view::npos) {
                    return std::string(
                        payload.substr(valueBegin));
                }

                return std::string(
                    payload.substr(
                        valueBegin,
                        end - valueBegin));
            }

            searchFrom = begin + needle.size();
        }

        return std::nullopt;
    }

    std::string AddonBridge::HexEncode(
        std::string_view value)
    {
        static constexpr char kHex[] =
            "0123456789ABCDEF";

        std::string out;
        out.reserve(value.size() * 2);

        for (const auto ch : value) {
            const auto u =
                static_cast<unsigned char>(ch);
            out.push_back(kHex[(u >> 4) & 0x0F]);
            out.push_back(kHex[u & 0x0F]);
        }

        return out;
    }

    std::optional<std::string> AddonBridge::HexDecode(
        std::string_view value)
    {
        if ((value.size() % 2) != 0) {
            return std::nullopt;
        }

        const auto nibble = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') {
                return ch - '0';
            }
            if (ch >= 'a' && ch <= 'f') {
                return 10 + ch - 'a';
            }
            if (ch >= 'A' && ch <= 'F') {
                return 10 + ch - 'A';
            }
            return -1;
        };

        std::string out;
        out.reserve(value.size() / 2);

        for (std::size_t i = 0;
             i < value.size();
             i += 2) {
            const int hi = nibble(value[i]);
            const int lo = nibble(value[i + 1]);

            if (hi < 0 || lo < 0) {
                return std::nullopt;
            }

            out.push_back(
                static_cast<char>((hi << 4) | lo));
        }

        return out;
    }

    RE::BSEventNotifyControl AddonBridge::ProcessEvent(
        const SKSE::ModCallbackEvent* event,
        RE::BSTEventSource<SKSE::ModCallbackEvent>*)
    {
        if (!event) {
            return RE::BSEventNotifyControl::kContinue;
        }

        const char* rawEventName =
            event->eventName.c_str();

        if (!rawEventName ||
            !EqualsInsensitive(
                rawEventName,
                kAddonEventName)) {
            return RE::BSEventNotifyControl::kContinue;
        }

        auto* actor =
            event->sender ?
                event->sender->As<RE::Actor>() :
                nullptr;

        // Addons are authoritative only for this client's real player.
        // This prevents a locally simulated remote STR proxy from becoming a
        // source of addon state and creating feedback loops.
        if (!actor || !actor->IsPlayerRef()) {
            SKSE::log::info(
                "OSTNET ADDON local event ignored sender={:08X} reason=not-local-player",
                actor ? actor->GetFormID() : 0);
            return RE::BSEventNotifyControl::kContinue;
        }

        const char* rawArg =
            event->strArg.c_str();
        const std::string_view arg =
            rawArg ? std::string_view(rawArg) :
                     std::string_view{};

        const auto parts = Split(arg, '|');
        if (parts.size() != 3) {
            SKSE::log::warn(
                "OSTNET ADDON local event invalid arg=\"{}\"",
                arg);
            return RE::BSEventNotifyControl::kContinue;
        }

        const auto& command = parts[0];
        const auto& channel = parts[1];
        const auto& value = parts[2];

        if (!IsSafeAddonToken(channel)) {
            SKSE::log::warn(
                "OSTNET ADDON local event invalid channel=\"{}\"",
                channel);
            return RE::BSEventNotifyControl::kContinue;
        }

        if (EqualsInsensitive(command, "OVR")) {
            if (value.empty() || value.size() > 512) {
                SKSE::log::warn(
                    "OSTNET ADDON OVR invalid marker channel={}",
                    channel);
                return RE::BSEventNotifyControl::kContinue;
            }

            SendOverlayState(
                actor,
                channel,
                value);
            return RE::BSEventNotifyControl::kContinue;
        }

        if (EqualsInsensitive(command, "OBJ")) {
            if (!IsSafeAddonToken(value)) {
                SKSE::log::warn(
                    "OSTNET ADDON OBJ invalid type channel={} type=\"{}\"",
                    channel,
                    value);
                return RE::BSEventNotifyControl::kContinue;
            }

            SendOStimObjectState(
                actor,
                channel,
                value,
                event->numArg > 0.5F);
            return RE::BSEventNotifyControl::kContinue;
        }

        SKSE::log::warn(
            "OSTNET ADDON local event unknown command=\"{}\" channel={}",
            command,
            channel);

        return RE::BSEventNotifyControl::kContinue;
    }

    void AddonBridge::SendOverlayState(
        RE::Actor* actor,
        std::string_view channel,
        std::string_view textureMarker)
    {
        if (!actor || !actor->IsPlayerRef()) {
            return;
        }

        const char* rawName = actor->GetName();
        const std::string name =
            rawName ? rawName : "";

        if (name.empty()) {
            return;
        }

        const auto chunks =
            RaceMenuOverlayBridge::GetSingleton()
                .CaptureMarkedOverlayChunks(
                    actor,
                    textureMarker,
                    2200);

        SKSE::log::info(
            "OSTNET ADDON OVR TX channel={} actor={:08X} name=\"{}\" marker=\"{}\" chunks={}",
            channel,
            actor->GetFormID(),
            name,
            textureMarker,
            chunks.size());

        for (std::size_t i = 0;
             i < chunks.size();
             ++i) {
            UdpTransport::GetSingleton().Send(
                fmt::format(
                    "ADDONOVR|channel={}|name={}|seq={}|count={}|props={}",
                    HexEncode(channel),
                    HexEncode(name),
                    i,
                    chunks.size(),
                    chunks[i]));
        }
    }

    void AddonBridge::SendOStimObjectState(
        RE::Actor* actor,
        std::string_view channel,
        std::string_view objectType,
        bool equipped)
    {
        if (!actor || !actor->IsPlayerRef()) {
            return;
        }

        const char* rawName = actor->GetName();
        const std::string name =
            rawName ? rawName : "";

        if (name.empty()) {
            return;
        }

        UdpTransport::GetSingleton().Send(
            fmt::format(
                "ADDONOBJ|channel={}|name={}|type={}|equipped={}",
                HexEncode(channel),
                HexEncode(name),
                HexEncode(objectType),
                equipped ? 1 : 0));

        SKSE::log::info(
            "OSTNET ADDON OBJ TX channel={} actor={:08X} name=\"{}\" type={} equipped={}",
            channel,
            actor->GetFormID(),
            name,
            objectType,
            equipped ? 1 : 0);
    }

    void AddonBridge::HandleRemotePacket(
        const std::string& sender,
        std::string_view payload)
    {
        if (payload.starts_with("ADDONOVR|")) {
            const auto channelHex = Field(payload, "channel");
            const auto nameHex = Field(payload, "name");
            const auto props = Field(payload, "props");

            if (!channelHex || !nameHex || !props) {
                SKSE::log::warn(
                    "OSTNET ADDON OVR RX invalid sender={}",
                    sender);
                return;
            }

            const auto channel = HexDecode(*channelHex);
            const auto name = HexDecode(*nameHex);

            if (!channel || !name ||
                !IsSafeAddonToken(*channel) ||
                name->empty()) {
                SKSE::log::warn(
                    "OSTNET ADDON OVR RX decode failed sender={}",
                    sender);
                return;
            }

            auto* actor =
                ActorResolver::GetSingleton()
                    .ResolveRemotePlayerByName(*name);

            if (!actor) {
                SKSE::log::warn(
                    "OSTNET ADDON OVR RX unresolved sender={} channel={} name=\"{}\"",
                    sender,
                    *channel,
                    *name);
                return;
            }

            RaceMenuOverlayBridge::GetSingleton()
                .ApplyRemoteOverlayChunk(
                    actor,
                    *channel,
                    *props);

            SKSE::log::info(
                "OSTNET ADDON OVR RX sender={} channel={} name=\"{}\" actor={:08X}",
                sender,
                *channel,
                *name,
                actor->GetFormID());
            return;
        }

        if (payload.starts_with("ADDONOBJ|")) {
            const auto channelHex = Field(payload, "channel");
            const auto nameHex = Field(payload, "name");
            const auto typeHex = Field(payload, "type");
            const auto equippedValue = Field(payload, "equipped");

            if (!channelHex || !nameHex ||
                !typeHex || !equippedValue) {
                SKSE::log::warn(
                    "OSTNET ADDON OBJ RX invalid sender={}",
                    sender);
                return;
            }

            const auto channel = HexDecode(*channelHex);
            const auto name = HexDecode(*nameHex);
            const auto objectType = HexDecode(*typeHex);

            if (!channel || !name || !objectType ||
                !IsSafeAddonToken(*channel) ||
                !IsSafeAddonToken(*objectType) ||
                name->empty()) {
                SKSE::log::warn(
                    "OSTNET ADDON OBJ RX decode failed sender={}",
                    sender);
                return;
            }

            auto* actor =
                ActorResolver::GetSingleton()
                    .ResolveRemotePlayerByName(*name);

            if (!actor) {
                SKSE::log::warn(
                    "OSTNET ADDON OBJ RX unresolved sender={} channel={} name=\"{}\" type={}",
                    sender,
                    *channel,
                    *name,
                    *objectType);
                return;
            }

            const bool equipped =
                *equippedValue == "1";

            const bool dispatched =
                PapyrusAnimationBridge::GetSingleton()
                    .SetOStimObjectState(
                        actor,
                        *objectType,
                        equipped);

            SKSE::log::info(
                "OSTNET ADDON OBJ RX sender={} channel={} name=\"{}\" actor={:08X} type={} equipped={} dispatched={}",
                sender,
                *channel,
                *name,
                actor->GetFormID(),
                *objectType,
                equipped ? 1 : 0,
                dispatched ? 1 : 0);
            return;
        }
    }
}
