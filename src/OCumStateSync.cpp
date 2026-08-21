#include "PCH.h"
#include "OCumStateSync.h"

#include "RaceMenuOverlayBridge.h"
#include "STRPMTransport.h"

namespace OStimTogether
{
    namespace
    {
        constexpr std::string_view kChannel = "ocum";
        constexpr std::string_view kMarker = "CumOverlays";
        constexpr std::string_view kVaginalObject = "ocumvagmesh";
        constexpr std::string_view kAnalObject = "ocumanmesh";

        bool IsArmorWorn(RE::Actor* actor, RE::TESObjectARMO* armor)
        {
            if (!actor || !armor) {
                return false;
            }

            const auto inventory = actor->GetInventory();
            const auto it = inventory.find(armor);
            return it != inventory.end() &&
                   it->second.second &&
                   it->second.second->IsWorn();
        }
    }

    OCumStateSync& OCumStateSync::GetSingleton()
    {
        static OCumStateSync instance;
        return instance;
    }

    std::string OCumStateSync::HexEncode(std::string_view value)
    {
        static constexpr char kHex[] = "0123456789ABCDEF";
        std::string out;
        out.reserve(value.size() * 2);
        for (const auto ch : value) {
            const auto u = static_cast<unsigned char>(ch);
            out.push_back(kHex[(u >> 4) & 0x0F]);
            out.push_back(kHex[u & 0x0F]);
        }
        return out;
    }

    void OCumStateSync::SendLocalSnapshot(std::string_view reason)
    {
        auto* data = RE::TESDataHandler::GetSingleton();
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!data || !player || !data->LookupModByName("OCum.esp")) {
            return;
        }

        const char* rawName = player->GetName();
        const std::string name = rawName ? rawName : "";
        if (name.empty()) {
            return;
        }

        const auto chunks = RaceMenuOverlayBridge::GetSingleton()
            .CaptureMarkedOverlayChunks(player, kMarker, 2200);

        for (std::size_t i = 0; i < chunks.size(); ++i) {
            STRPMTransport::GetSingleton().Send(
                fmt::format(
                    "ADDONOVR|channel={}|name={}|seq={}|count={}|props={}",
                    HexEncode(kChannel),
                    HexEncode(name),
                    i,
                    chunks.size(),
                    chunks[i]));
        }

        auto* vaginal = data->LookupForm<RE::TESObjectARMO>(0x00000F37, "OCum.esp");
        auto* anal = data->LookupForm<RE::TESObjectARMO>(0x00000F3B, "OCum.esp");

        const bool vaginalEquipped = IsArmorWorn(player, vaginal);
        const bool analEquipped = IsArmorWorn(player, anal);

        const auto sendObject = [&](std::string_view type, bool equipped) {
            STRPMTransport::GetSingleton().Send(
                fmt::format(
                    "ADDONOBJ|channel={}|name={}|type={}|equipped={}",
                    HexEncode(kChannel),
                    HexEncode(name),
                    HexEncode(type),
                    equipped ? 1 : 0));
        };

        sendObject(kVaginalObject, vaginalEquipped);
        sendObject(kAnalObject, analEquipped);

        SKSE::log::info(
            "OSTNET OCUM SNAPSHOT TX reason={} actor={:08X} name=\"{}\" overlayChunks={} vagMesh={} analMesh={}",
            reason,
            player->GetFormID(),
            name,
            chunks.size(),
            vaginalEquipped ? 1 : 0,
            analEquipped ? 1 : 0);
    }
}
