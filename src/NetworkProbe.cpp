#include "PCH.h"
#include "NetworkProbe.h"
#include "STRPMTransport.h"
#include "UdpTransport.h"
#include "OStimBridge.h"

#include <cctype>
#include <limits>

namespace OStimTogether
{
    namespace
    {
        void SendScenePayload(std::string_view payload)
        {
            if (STRPMTransport::GetSingleton().Send(payload)) {
                return;
            }

            UdpTransport::GetSingleton().Send(payload);
        }
    }

    NetworkProbe& NetworkProbe::GetSingleton()
    {
        static NetworkProbe instance;
        return instance;
    }

    std::string NetworkProbe::GetNodeID(OStim::Thread* thread) const
    {
        if (!thread) {
            return "";
        }

        auto* node = thread->getCurrentNode();
        if (!node) {
            return "";
        }

        const auto* id = node->getNodeID();
        return id ? id : "";
    }

    std::string NetworkProbe::BuildActorList(OStim::Thread* thread) const
    {
        if (!thread) {
            return "";
        }

        std::string result;
        const auto count = thread->getActorCount();

        for (std::uint32_t i = 0; i < count; ++i) {
            auto* ta = thread->getActor(i);
            auto* actor =
                ta ? static_cast<RE::Actor*>(ta->getGameActor()) : nullptr;

            if (i != 0) {
                result += ",";
            }

            if (!actor) {
                result += "null";
                continue;
            }

            result += fmt::format(
                "{:08X}:{}:{}",
                actor->GetFormID(),
                actor->IsPlayerRef() ? "player" : "actor",
                actor->GetName());
        }

        return result;
    }

    std::string NetworkProbe::BuildActorPoses(
        OStim::Thread* thread,
        const SceneCenter& center) const
    {
        if (!thread ||
            !center.IsFinite()) {
            return "";
        }

        std::string result;

        const auto actorCount =
            thread->getActorCount();

        for (std::uint32_t i = 0;
             i < actorCount;
             ++i) {
            ActorPose pose{};

            if (!OStimBridge::GetSingleton().
                    TryComputeActorPose(
                        thread,
                        i,
                        center,
                        pose)) {
                continue;
            }

            if (!result.empty()) {
                result += ";";
            }

            result += fmt::format(
                "{}:{:.6f}:{:.6f}:{:.6f}:{:.8f}",
                i,
                pose.x,
                pose.y,
                pose.z,
                pose.r);
        }

        return result;
    }

    FurnitureAnchor NetworkProbe::FindLockedSceneFurniture(
        OStim::Thread* thread) const
    {
        FurnitureAnchor result{};

        if (!thread) {
            return result;
        }

        RE::Actor* referenceActor = nullptr;

        const auto actorCount =
            thread->getActorCount();

        for (std::uint32_t i = 0;
             i < actorCount;
             ++i) {
            auto* threadActor =
                thread->getActor(i);

            auto* actor =
                threadActor ?
                static_cast<RE::Actor*>(
                    threadActor->getGameActor()) :
                nullptr;

            if (!actor) {
                continue;
            }

            if (actor->IsPlayerRef()) {
                referenceActor = actor;
                break;
            }

            if (!referenceActor) {
                referenceActor = actor;
            }
        }

        if (!referenceActor) {
            return result;
        }

        auto* cell =
            referenceActor->GetParentCell();

        if (!cell) {
            SKSE::log::warn(
                "OSTNET FURNITURE LOCK sender node={} no-parent-cell",
                GetNodeID(thread));
            return result;
        }

        const auto origin =
            referenceActor->GetPosition();

        auto equalsInsensitive =
            [](std::string_view lhs,
               std::string_view rhs) {
                if (lhs.size() != rhs.size()) {
                    return false;
                }

                for (std::size_t i = 0;
                     i < lhs.size();
                     ++i) {
                    const auto a =
                        static_cast<unsigned char>(
                            lhs[i]);

                    const auto b =
                        static_cast<unsigned char>(
                            rhs[i]);

                    if (std::tolower(a) !=
                        std::tolower(b)) {
                        return false;
                    }
                }

                return true;
            };

        RE::TESObjectREFR* best = nullptr;
        float bestDistanceSq =
            std::numeric_limits<float>::max();

        std::uint32_t blockedFurnitureCount = 0;
        std::uint32_t ostimOwnedCount = 0;

        constexpr float kSearchRadius =
            1024.0F;

        cell->ForEachReferenceInRange(
            origin,
            kSearchRadius,
            [&](RE::TESObjectREFR& ref) {
                auto* base =
                    ref.GetBaseObject();

                if (!base ||
                    !base->As<RE::TESFurniture>()) {
                    return RE::BSContainer::
                        ForEachResult::kContinue;
                }

                if (!ref.IsActivationBlocked()) {
                    return RE::BSContainer::
                        ForEachResult::kContinue;
                }

                ++blockedFurnitureCount;

                auto* owner =
                    ref.GetOwner();

                auto* ownerFile =
                    owner ?
                        owner->GetFile(0) :
                        nullptr;

                const auto ownerFileName =
                    ownerFile ?
                        ownerFile->GetFilename() :
                        std::string_view{};

                const bool ostimOwned =
                    ownerFile &&
                    equalsInsensitive(
                        ownerFileName,
                        "OStim.esp");

                const auto pos =
                    ref.GetPosition();

                const float dx =
                    pos.x - origin.x;
                const float dy =
                    pos.y - origin.y;
                const float dz =
                    pos.z - origin.z;

                const float distanceSq =
                    dx * dx +
                    dy * dy +
                    dz * dz;

                SKSE::log::info(
                    "OSTNET FURNITURE LOCK candidate node={} ref={:08X} base={:08X} name=\"{}\" blocked=1 owner={:08X} ownerFile=\"{}\" distance={:.3f} ostimOwned={}",
                    GetNodeID(thread),
                    ref.GetFormID(),
                    base->GetFormID(),
                    ref.GetName(),
                    owner ?
                        owner->GetFormID() :
                        0,
                    ownerFileName,
                    std::sqrt(distanceSq),
                    ostimOwned ? 1 : 0);

                if (!ostimOwned) {
                    return RE::BSContainer::
                        ForEachResult::kContinue;
                }

                ++ostimOwnedCount;

                if (distanceSq <
                    bestDistanceSq) {
                    bestDistanceSq =
                        distanceSq;
                    best =
                        std::addressof(ref);
                }

                return RE::BSContainer::
                    ForEachResult::kContinue;
            });

        if (!best) {
            SKSE::log::warn(
                "OSTNET FURNITURE LOCK sender node={} no-exact-OStim-furniture blockedFurniture={} ostimOwned={} radius={:.0f}; furniture=none",
                GetNodeID(thread),
                blockedFurnitureCount,
                ostimOwnedCount,
                kSearchRadius);

            return result;
        }

        const auto pos =
            best->GetPosition();

        auto* base =
            best->GetBaseObject();

        result.referenceFormID =
            best->GetFormID();

        result.baseFormID =
            base ?
                base->GetFormID() :
                0;

        result.x = pos.x;
        result.y = pos.y;
        result.z = pos.z;
        result.r =
            best->GetAngleZ();

        result.valid = true;

        SKSE::log::info(
            "OSTNET FURNITURE LOCK sender EXACT node={} ref={:08X} base={:08X} name=\"{}\" pos=({:.3f},{:.3f},{:.3f},{:.5f}) playerDistance={:.3f} candidates={}",
            GetNodeID(thread),
            result.referenceFormID,
            result.baseFormID,
            best->GetName(),
            result.x,
            result.y,
            result.z,
            result.r,
            std::sqrt(bestDistanceSq),
            ostimOwnedCount);

        return result;
    }

    std::string NetworkProbe::EncodeFurniture(
        const FurnitureAnchor& furniture)
    {
        if (!furniture.IsFinite()) {
            return "none";
        }

        return fmt::format(
            "{:08X},{:08X},{:.6f},{:.6f},{:.6f},{:.8f}",
            furniture.referenceFormID,
            furniture.baseFormID,
            furniture.x,
            furniture.y,
            furniture.z,
            furniture.r);
    }

    void NetworkProbe::SceneStart(OStim::Thread* thread)
    {
        if (!thread) {
            return;
        }

        const auto threadID = thread->getThreadID();

        {
            std::scoped_lock lock(_mutex);
            _startedThreads.insert(threadID);
        }

        SceneCenter center{};

        const bool haveCenter =
            OStimBridge::GetSingleton().
                TryComputeSceneCenter(
                    thread,
                    center);

        const auto poses =
            haveCenter ?
                BuildActorPoses(
                    thread,
                    center) :
                std::string{};

        const auto furniture =
            FindLockedSceneFurniture(
                thread);

        const auto payload =
            fmt::format(
                "START|thread={}|node={}|center={:.6f},{:.6f},{:.6f},{:.8f}|furniture={}|poses={}|actors={}",
                threadID,
                GetNodeID(thread),
                haveCenter ? center.x : 0.0F,
                haveCenter ? center.y : 0.0F,
                haveCenter ? center.z : 0.0F,
                haveCenter ? center.r : 0.0F,
                EncodeFurniture(
                    furniture),
                poses,
                BuildActorList(thread));

        SKSE::log::info(
            "OSTNET|v1|{} centerValid={} furnitureValid={}",
            payload,
            haveCenter ? 1 : 0,
            furniture.IsFinite() ? 1 : 0);

        SendScenePayload(payload);
    }

    void NetworkProbe::SceneNode(OStim::Thread* thread)
    {
        if (!thread) {
            return;
        }

        const auto threadID = thread->getThreadID();

        {
            std::scoped_lock lock(_mutex);

            if (!_startedThreads.contains(threadID)) {
                SKSE::log::trace(
                    "OSTNET ignoring pre-START NODE thread={}",
                    threadID);
                return;
            }
        }

        SceneCenter center{};

        const bool haveCenter =
            OStimBridge::GetSingleton().
                TryComputeSceneCenter(
                    thread,
                    center);

        const auto poses =
            haveCenter ?
                BuildActorPoses(
                    thread,
                    center) :
                std::string{};

        const auto payload =
            fmt::format(
                "NODE|thread={}|node={}|poses={}|actors={}",
                threadID,
                GetNodeID(thread),
                poses,
                BuildActorList(thread));

        SKSE::log::info(
            "OSTNET|v1|{} centerValid={}",
            payload,
            haveCenter ? 1 : 0);
        SendScenePayload(payload);
    }

    void NetworkProbe::SceneStop(OStim::Thread* thread)
    {
        if (!thread) {
            return;
        }

        const auto threadID = thread->getThreadID();

        const auto payload =
            fmt::format(
                "STOP|thread={}|node={}|actors={}",
                threadID,
                GetNodeID(thread),
                BuildActorList(thread));

        SKSE::log::info("OSTNET|v1|{}", payload);
        SendScenePayload(payload);

        std::scoped_lock lock(_mutex);
        _startedThreads.erase(threadID);
    }

    void NetworkProbe::SceneSpeed(
        OStim::Thread* thread,
        std::int32_t speed)
    {
        if (!thread || speed < 0) {
            return;
        }

        const auto threadID = thread->getThreadID();

        {
            std::scoped_lock lock(_mutex);

            if (!_startedThreads.contains(threadID)) {
                SKSE::log::trace(
                    "OSTNET ignoring pre-START SPEED thread={}",
                    threadID);
                return;
            }
        }

        const auto payload =
            fmt::format(
                "SPEED|thread={}|speed={}",
                threadID,
                speed);

        SKSE::log::info("OSTNET|v1|{}", payload);
        SendScenePayload(payload);
    }
}