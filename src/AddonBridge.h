#pragma once

#include "PCH.h"

namespace OStimTogether
{
    // Generic, addon-agnostic bridge.
    //
    // Papyrus addons send the local player a standard SKSE ModEvent named
    // "ostimtogether_addon".  The event's strArg is one of:
    //   OVR|<channel>|<texture-marker>
    //   OBJ|<channel>|<ostim-object-type>
    // and numArg is used as the boolean state for OBJ.
    //
    // The core never needs to know which addon owns a channel, which texture
    // marker it uses, or which OStim equip-object names it defines.
    class AddonBridge final :
        public RE::BSTEventSink<SKSE::ModCallbackEvent>
    {
    public:
        static AddonBridge& GetSingleton();

        void Register();

        RE::BSEventNotifyControl ProcessEvent(
            const SKSE::ModCallbackEvent* event,
            RE::BSTEventSource<SKSE::ModCallbackEvent>* source) override;

        // Called by ActorResolver on Skyrim's game thread for ADDON* packets.
        void HandleRemotePacket(
            const std::string& sender,
            std::string_view payload);

    private:
        AddonBridge() = default;

        static std::vector<std::string> Split(
            std::string_view text,
            char delimiter);

        static std::optional<std::string> Field(
            std::string_view payload,
            std::string_view key);

        static std::string HexEncode(
            std::string_view value);

        static std::optional<std::string> HexDecode(
            std::string_view value);

        void SendOverlayState(
            RE::Actor* actor,
            std::string_view channel,
            std::string_view textureMarker);

        void SendOStimObjectState(
            RE::Actor* actor,
            std::string_view channel,
            std::string_view objectType,
            bool equipped);

        std::atomic_bool _registered{ false };
    };
}
