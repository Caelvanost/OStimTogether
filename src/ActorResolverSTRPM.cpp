#include "PCH.h"
#include "ActorResolver.h"
#include "AddonBridge.h"
#include "OStimBridge.h"
#include "STRPMTransport.h"

#include <cctype>
#include <limits>

namespace OStimTogether
{
    ActorResolver& ActorResolver::GetSingleton()
    {
        static ActorResolver instance;
        return instance;
    }

    std::vector<std::string> ActorResolver::Split(
        std::string_view text,
        char delimiter)
    {
        std::vector<std::string> result;
        std::size_t start = 0;

        while (start <= text.size()) {
            const auto pos = text.find(delimiter, start);
            if (pos == std::string_view::npos) {
                result.emplace_back(text.substr(start));
                break;
            }

            result.emplace_back(text.substr(start, pos - start));
            start = pos + 1;
        }

        return result;
    }

    std::string ActorResolver::Trim(std::string value)
    {
        while (!value.empty() &&
               std::isspace(static_cast<unsigned char>(value.front()))) {
            value.erase(value.begin());
        }

        while (!value.empty() &&
               std::isspace(static_cast<unsigned char>(value.back()))) {
            value.pop_back();
        }

        return value;
    }

    std::string ActorResolver::NormalizeName(std::string_view value)
    {
        std::string result(value);
        std::transform(
            result.begin(),
            result.end(),
            result.begin(),
            [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
        return result;
    }

    bool ActorResolver::EqualsInsensitive(
        std::string_view lhs,
        std::string_view rhs)
    {
        return NormalizeName(lhs) == NormalizeName(rhs);
    }

    std::optional<std::string> ActorResolver::Field(
        std::string_view payload,
        std::string_view key)
    {
        const auto needle = fmt::format("{}=", key);
        std::size_t searchFrom = 0;

        while (searchFrom < payload.size()) {
            const auto pos = payload.find(needle, searchFrom);
            if (pos == std::string_view::npos) {
                return std::nullopt;
            }

            if (pos == 0 || payload[pos - 1] == '|') {
                const auto valueStart = pos + needle.size();
                const auto valueEnd = payload.find('|', valueStart);

                if (valueEnd == std::string_view::npos) {
                    return std::string(payload.substr(valueStart));
                }

                return std::string(
                    payload.substr(valueStart, valueEnd - valueStart));
            }

            searchFrom = pos + needle.size();
        }

        return std::nullopt;
    }

    std::optional<std::int32_t> ActorResolver::ParseThreadID(
        std::string_view payload)
    {
        const auto value = Field(payload, "thread");
        if (!value || value->empty()) {
            return std::nullopt;
        }

        try {
            return static_cast<std::int32_t>(std::stol(*value));
        } catch (...) {
            return std::nullopt;
        }
    }

    SceneCenter ActorResolver::ParseSceneCenter(
        std::string_view payload)
    {
        SceneCenter center{};
        const auto value = Field(payload, "center");

        if (!value || value->empty()) {
            return center;
        }

        const auto parts = Split(*value, ',');
        if (parts.size() != 4) {
            return center;
        }

        try {
            center.x = std::stof(parts[0]);
            center.y = std::stof(parts[1]);
            center.z = std::stof(parts[2]);
            center.r = std::stof(parts[3]);
            center.valid = true;
        } catch (...) {
            center.valid = false;
        }

        if (!center.IsFinite() ||
            std::abs(center.x) > 1000000.0F ||
            std::abs(center.y) > 1000000.0F ||
            std::abs(center.z) > 1000000.0F ||
            std::abs(center.r) > 100.0F) {
            center.valid = false;
        }

        return center;
    }

    FurnitureAnchor ActorResolver::ParseFurniture(
        std::string_view payload)
    {
        FurnitureAnchor result{};
        const auto value = Field(payload, "furniture");

        if (!value || value->empty() || *value == "none") {
            return result;
        }

        const auto fields = Split(*value, ',');
        if (fields.size() != 6) {
            return result;
        }

        try {
            result.referenceFormID =
                static_cast<std::uint32_t>(
                    std::stoul(fields[0], nullptr, 16));
            result.baseFormID =
                static_cast<std::uint32_t>(
                    std::stoul(fields[1], nullptr, 16));
            result.x = std::stof(fields[2]);
            result.y = std::stof(fields[3]);
            result.z = std::stof(fields[4]);
            result.r = std::stof(fields[5]);
            result.valid = true;
        } catch (...) {
            result.valid = false;
        }

        if (!result.IsFinite() ||
            std::abs(result.x) > 1000000.0F ||
            std::abs(result.y) > 1000000.0F ||
            std::abs(result.z) > 1000000.0F ||
            std::abs(result.r) > 100.0F) {
            result.valid = false;
        }

        return result;
    }

    RE::TESObjectREFR* ActorResolver::ResolveFurniture(
        const FurnitureAnchor& furniture,
        const std::vector<RE::Actor*>& actors)
    {
        if (!furniture.IsFinite()) {
            return nullptr;
        }

        const RE::NiPoint3 target{
            furniture.x,
            furniture.y,
            furniture.z
        };

        auto isFurniture = [](RE::TESObjectREFR* ref) {
            if (!ref) {
                return false;
            }
            auto* base = ref->GetBaseObject();
            return base && base->As<RE::TESFurniture>();
        };

        if (furniture.referenceFormID != 0) {
            auto* form = RE::TESForm::LookupByID(furniture.referenceFormID);
            auto* ref = form ? form->As<RE::TESObjectREFR>() : nullptr;

            if (isFurniture(ref)) {
                const auto pos = ref->GetPosition();
                const auto distanceSq = pos.GetSquaredDistance(target);
                constexpr float kExactMaxDistance = 24.0F;

                if (distanceSq <= kExactMaxDistance * kExactMaxDistance) {
                    SKSE::log::info(
                        "OSTNET FURNITURE receiver EXACT ref={:08X} distance={:.3f}",
                        ref->GetFormID(),
                        std::sqrt(distanceSq));
                    return ref;
                }
            }
        }

        RE::TESObjectCELL* cell = nullptr;
        for (auto* actor : actors) {
            if (!actor || !actor->GetParentCell()) {
                continue;
            }
            cell = actor->GetParentCell();
            if (actor->IsPlayerRef()) {
                break;
            }
        }

        if (!cell) {
            return nullptr;
        }

        RE::TESObjectREFR* best = nullptr;
        float bestDistanceSq = std::numeric_limits<float>::max();
        constexpr float kSearchRadius = 48.0F;

        cell->ForEachReferenceInRange(
            target,
            kSearchRadius,
            [&](RE::TESObjectREFR& ref) {
                auto* candidate = std::addressof(ref);
                if (!isFurniture(candidate)) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }

                const auto distanceSq =
                    ref.GetPosition().GetSquaredDistance(target);
                if (distanceSq < bestDistanceSq) {
                    bestDistanceSq = distanceSq;
                    best = candidate;
                }

                return RE::BSContainer::ForEachResult::kContinue;
            });

        if (!best || bestDistanceSq > kSearchRadius * kSearchRadius) {
            SKSE::log::warn(
                "OSTNET FURNITURE receiver MISS target=({:.3f},{:.3f},{:.3f})",
                furniture.x,
                furniture.y,
                furniture.z);
            return nullptr;
        }

        SKSE::log::info(
            "OSTNET FURNITURE receiver COORD ref={:08X} distance={:.3f}",
            best->GetFormID(),
            std::sqrt(bestDistanceSq));
        return best;
    }

    std::vector<ActorPose> ActorResolver::ParseActorPoses(
        std::string_view payload)
    {
        std::vector<ActorPose> result;
        const auto value = Field(payload, "poses");

        if (!value || value->empty()) {
            return result;
        }

        for (const auto& rawPose : Split(*value, ';')) {
            const auto fields = Split(rawPose, ':');
            if (fields.size() != 5) {
                continue;
            }

            try {
                const auto index =
                    static_cast<std::size_t>(std::stoul(fields[0]));
                if (index > 15) {
                    continue;
                }

                ActorPose pose{};
                pose.x = std::stof(fields[1]);
                pose.y = std::stof(fields[2]);
                pose.z = std::stof(fields[3]);
                pose.r = std::stof(fields[4]);
                pose.valid = true;

                if (!pose.IsFinite() ||
                    std::abs(pose.x) > 1000000.0F ||
                    std::abs(pose.y) > 1000000.0F ||
                    std::abs(pose.z) > 1000000.0F ||
                    std::abs(pose.r) > 100.0F) {
                    continue;
                }

                if (result.size() <= index) {
                    result.resize(index + 1);
                }
                result[index] = pose;
            } catch (...) {
                continue;
            }
        }

        return result;
    }

    std::vector<ActorResolver::Participant>
        ActorResolver::ParseParticipants(std::string_view payload)
    {
        std::vector<Participant> result;
        const auto actorsValue = Field(payload, "actors");

        if (!actorsValue || actorsValue->empty()) {
            return result;
        }

        for (const auto& rawActor : Split(*actorsValue, ',')) {
            const auto first = rawActor.find(':');
            if (first == std::string::npos) {
                continue;
            }

            const auto second = rawActor.find(':', first + 1);
            if (second == std::string::npos) {
                continue;
            }

            Participant participant{};
            try {
                participant.remoteFormID =
                    static_cast<std::uint32_t>(
                        std::stoul(rawActor.substr(0, first), nullptr, 16));
            } catch (...) {
                participant.remoteFormID = 0;
            }

            participant.role =
                Trim(rawActor.substr(first + 1, second - first - 1));
            participant.name = Trim(rawActor.substr(second + 1));
            result.push_back(std::move(participant));
        }

        return result;
    }

    void ActorResolver::CacheSTRPMProxyName(
        STRPMApi::ConnectionID connectionID,
        RE::FormID formID,
        std::string_view senderName)
    {
        if (connectionID == 0 || formID == 0) {
            return;
        }

        std::vector<std::string> names;
        if (!senderName.empty()) {
            names.push_back(NormalizeName(senderName));
        }

        if (auto* form = RE::TESForm::LookupByID(formID)) {
            if (auto* actor = form ? form->As<RE::Actor>() : nullptr) {
                const auto* actorName = actor->GetName();
                if (actorName && *actorName) {
                    names.push_back(NormalizeName(actorName));
                }
            }
        }

        std::scoped_lock lock(_mutex);
        for (const auto& name : names) {
            if (!name.empty()) {
                _strpmRemoteByName[name] = formID;
            }
        }
    }

    ActorResolver::ResolveResult ActorResolver::ResolveParticipant(
        const Participant& participant,
        STRPMApi::ConnectionID senderConnectionID)
    {
        ResolveResult result{};

        if (participant.name.empty()) {
            return result;
        }

        auto* localPlayer = RE::PlayerCharacter::GetSingleton();

        if (participant.role == "player" && senderConnectionID != 0) {
            const auto formID =
                STRPMTransport::GetSingleton().ResolveProxy(senderConnectionID);

            if (!formID) {
                SKSE::log::warn(
                    "OSTNET STRPM RESOLVE pending connection={} name=\"{}\"",
                    senderConnectionID,
                    participant.name);
                return result;
            }

            auto* form = RE::TESForm::LookupByID(*formID);
            auto* actor = form ? form->As<RE::Actor>() : nullptr;

            if (!actor || actor == localPlayer) {
                SKSE::log::warn(
                    "OSTNET STRPM RESOLVE invalid connection={} form={:08X}",
                    senderConnectionID,
                    *formID);
                return result;
            }

            result.chosen = *formID;
            result.matches.push_back(*formID);
            result.fromSTRPM = true;
            return result;
        }

        if (participant.role != "player" && localPlayer) {
            const auto* localName = localPlayer->GetName();
            if (localName && EqualsInsensitive(localName, participant.name)) {
                result.chosen = localPlayer->GetFormID();
                result.matches.push_back(result.chosen);
                result.localSelf = true;
                return result;
            }
        }

        auto* processLists = RE::ProcessLists::GetSingleton();
        if (!processLists) {
            return result;
        }

        struct Candidate
        {
            RE::FormID formID{ 0 };
            float distance{ 0.0F };
        };

        std::vector<Candidate> candidates;
        std::unordered_set<RE::FormID> seen;

        auto considerActor = [&](RE::Actor* actor) {
            if (!actor) {
                return;
            }

            const auto formID = actor->GetFormID();
            if (!seen.insert(formID).second) {
                return;
            }

            if (participant.role == "player" && actor == localPlayer) {
                return;
            }

            const auto* actorName = actor->GetName();
            if (!actorName ||
                !EqualsInsensitive(actorName, participant.name)) {
                return;
            }

            float distance = 0.0F;
            if (localPlayer) {
                distance = actor->GetPosition().GetDistance(
                    localPlayer->GetPosition());
            }

            candidates.push_back(Candidate{ formID, distance });
            result.matches.push_back(formID);
        };

        auto scanHandles = [&](const auto& handles) {
            for (const auto& handle : handles) {
                const auto actorPtr = handle.get();
                considerActor(actorPtr.get());
            }
        };

        scanHandles(processLists->highActorHandles);
        scanHandles(processLists->middleHighActorHandles);
        scanHandles(processLists->middleLowActorHandles);
        scanHandles(processLists->lowActorHandles);

        if (candidates.empty()) {
            return result;
        }

        std::sort(
            candidates.begin(),
            candidates.end(),
            [](const Candidate& lhs, const Candidate& rhs) {
                return lhs.distance < rhs.distance;
            });

        result.chosen = candidates.front().formID;
        return result;
    }

    RE::Actor* ActorResolver::ResolveRemotePlayerByName(
        std::string_view name)
    {
        const auto normalized = NormalizeName(name);

        {
            std::scoped_lock lock(_mutex);
            const auto it = _strpmRemoteByName.find(normalized);
            if (it != _strpmRemoteByName.end()) {
                auto* form = RE::TESForm::LookupByID(it->second);
                if (auto* actor = form ? form->As<RE::Actor>() : nullptr) {
                    SKSE::log::info(
                        "OSTNET STRPM ADDON RESOLVE name=\"{}\" form={:08X}",
                        name,
                        actor->GetFormID());
                    return actor;
                }
            }
        }

        Participant participant{};
        participant.role = "player";
        participant.name = Trim(std::string(name));
        const auto resolved = ResolveParticipant(participant, 0);

        if (!resolved.chosen) {
            return nullptr;
        }

        auto* form = RE::TESForm::LookupByID(resolved.chosen);
        return form ? form->As<RE::Actor>() : nullptr;
    }

    void ActorResolver::HandleStart(
        const std::string& sender,
        std::string_view payload,
        STRPMApi::ConnectionID senderConnectionID)
    {
        const auto threadID = ParseThreadID(payload);
        const auto nodeValue = Field(payload, "node");

        if (!threadID || !nodeValue || nodeValue->empty()) {
            SKSE::log::error(
                "OSTNET RESOLVE START invalid sender={} connection={}",
                sender,
                senderConnectionID);
            return;
        }

        const auto participants = ParseParticipants(payload);
        const auto authoritativeCenter = ParseSceneCenter(payload);
        const auto furnitureDescriptor = ParseFurniture(payload);
        const auto authoritativePoses = ParseActorPoses(payload);
        const bool wallScene =
            nodeValue->find("wall") != std::string::npos;
        const bool anchoredScene =
            furnitureDescriptor.IsFinite() || wallScene;

        SKSE::log::info(
            "OSTNET RESOLVE START sender={} connection={} thread={} participants={} node={} transport={} continuousMirrorAlign={}",
            sender,
            senderConnectionID,
            *threadID,
            participants.size(),
            *nodeValue,
            senderConnectionID ? "STRPM" : "UDP",
            anchoredScene ? 1 : 0);

        std::vector<RE::Actor*> resolvedActors;
        std::vector<bool> localAlignmentMask;
        resolvedActors.reserve(participants.size());
        localAlignmentMask.reserve(participants.size());

        std::int32_t localSelfIndex = -1;
        bool allResolved = true;

        for (std::size_t i = 0; i < participants.size(); ++i) {
            const auto& participant = participants[i];
            const auto resolved =
                ResolveParticipant(participant, senderConnectionID);

            if (resolved.chosen == 0) {
                allResolved = false;
                SKSE::log::warn(
                    "OSTNET RESOLVE MISS sender={} connection={} thread={} idx={} role={} remoteForm={:08X} name=\"{}\"",
                    sender,
                    senderConnectionID,
                    *threadID,
                    i,
                    participant.role,
                    participant.remoteFormID,
                    participant.name);
                resolvedActors.push_back(nullptr);
                localAlignmentMask.push_back(false);
                continue;
            }

            auto* form = RE::TESForm::LookupByID(resolved.chosen);
            auto* actor = form ? form->As<RE::Actor>() : nullptr;
            if (!actor) {
                allResolved = false;
                resolvedActors.push_back(nullptr);
                localAlignmentMask.push_back(false);
                continue;
            }

            const bool isLocalSelf =
                resolved.localSelf && actor->IsPlayerRef();
            if (isLocalSelf) {
                localSelfIndex = static_cast<std::int32_t>(i);
            }

            const bool alignLocally =
                anchoredScene && participant.role != "player";
            localAlignmentMask.push_back(alignLocally);
            resolvedActors.push_back(actor);

            SKSE::log::info(
                "OSTNET RESOLVE {} sender={} connection={} thread={} idx={} role={} name=\"{}\" -> localForm={:08X} continuousAlign={}",
                resolved.fromSTRPM ? "STRPM" :
                    (resolved.localSelf ? "SELF" : "LEGACY"),
                sender,
                senderConnectionID,
                *threadID,
                i,
                participant.role,
                participant.name,
                resolved.chosen,
                alignLocally ? 1 : 0);

            const auto key = fmt::format(
                "{}|{}|{}",
                sender,
                *threadID,
                i);
            {
                std::scoped_lock lock(_mutex);
                _resolved[key] = resolved.chosen;
            }
        }

        if (!allResolved || resolvedActors.empty()) {
            SKSE::log::warn(
                "OSTNET MIRROR not started sender={} connection={} thread={} resolved={}/{}",
                sender,
                senderConnectionID,
                *threadID,
                std::count_if(
                    resolvedActors.begin(),
                    resolvedActors.end(),
                    [](RE::Actor* actor) { return actor != nullptr; }),
                resolvedActors.size());
            return;
        }

        auto* localFurniture =
            ResolveFurniture(furnitureDescriptor, resolvedActors);

        OStimBridge::GetSingleton().StartRemoteMirror(
            sender,
            *threadID,
            resolvedActors,
            localAlignmentMask,
            localSelfIndex,
            authoritativeCenter,
            authoritativePoses,
            localFurniture,
            *nodeValue);
    }

    void ActorResolver::HandlePayload(
        const std::string& sender,
        std::string_view payload,
        STRPMApi::ConnectionID senderConnectionID)
    {
        if (payload.starts_with("ADDONOVR|") ||
            payload.starts_with("ADDONOBJ|")) {
            AddonBridge::GetSingleton().HandleRemotePacket(sender, payload);
            return;
        }

        if (payload.starts_with("START|")) {
            HandleStart(sender, payload, senderConnectionID);
            return;
        }

        if (payload.starts_with("NODE|")) {
            const auto threadID = ParseThreadID(payload);
            const auto nodeValue = Field(payload, "node");
            if (!threadID || !nodeValue || nodeValue->empty()) {
                SKSE::log::warn(
                    "OSTNET MIRROR NODE invalid packet sender={}",
                    sender);
                return;
            }

            OStimBridge::GetSingleton().NavigateRemoteMirror(
                sender,
                *threadID,
                *nodeValue,
                ParseActorPoses(payload));
            return;
        }

        if (payload.starts_with("SPEED|")) {
            const auto threadID = ParseThreadID(payload);
            const auto speedValue = Field(payload, "speed");
            if (!threadID || !speedValue) {
                return;
            }

            try {
                const auto speed =
                    static_cast<std::int32_t>(std::stol(*speedValue));
                auto& bridge = OStimBridge::GetSingleton();

                if (bridge.IsRemoteMirrorSpeedCurrent(
                        sender,
                        *threadID,
                        speed)) {
                    SKSE::log::info(
                        "OSTNET MIRROR SPEED NOOP sender={} remoteThread={} speed={} reason=already-current",
                        sender,
                        *threadID,
                        speed);
                    return;
                }

                bridge.SetRemoteMirrorSpeed(
                    sender,
                    *threadID,
                    speed);
            } catch (...) {
                SKSE::log::warn(
                    "OSTNET MIRROR SPEED invalid value sender={} value={}",
                    sender,
                    *speedValue);
            }
            return;
        }

        if (payload.starts_with("STOP|")) {
            const auto threadID = ParseThreadID(payload);
            if (threadID) {
                OStimBridge::GetSingleton().StopRemoteMirror(
                    sender,
                    *threadID);
            }
        }
    }

    void ActorResolver::HandleSTRPMPacket(
        STRPMApi::ConnectionID connectionID,
        std::string sender,
        std::string payload)
    {
        if (connectionID == 0 || payload.empty()) {
            return;
        }

        const auto proxy =
            STRPMTransport::GetSingleton().ResolveProxy(connectionID);
        if (proxy) {
            CacheSTRPMProxyName(connectionID, *proxy, sender);
        }

        HandlePayload(sender, payload, connectionID);
    }

    void ActorResolver::HandleUdpPacket(std::string packet)
    {
        constexpr std::string_view prefix = "OSTUDP|v1|from=";
        if (!packet.starts_with(prefix)) {
            return;
        }

        const auto senderStart = prefix.size();
        const auto senderEnd = packet.find('|', senderStart);
        if (senderEnd == std::string::npos) {
            return;
        }

        const std::string sender =
            packet.substr(senderStart, senderEnd - senderStart);
        const std::string_view payload{
            packet.data() + senderEnd + 1,
            packet.size() - senderEnd - 1
        };

        HandlePayload(sender, payload, 0);
    }
}
