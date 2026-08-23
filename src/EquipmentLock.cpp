#include "PCH.h"
#include "EquipmentLock.h"
#include "DefaultOutfitGuard.h"

namespace OStimTogether
{
    namespace
    {
        bool IsLikelySTRRemotePlayerProxy(RE::Actor* actor)
        {
            if (!actor || actor->IsPlayerRef()) {
                return false;
            }

            auto* base = actor->GetActorBase();
            if (!base) {
                return false;
            }

            constexpr RE::FormID kDynamicMask = 0xFF000000;
            return (actor->GetFormID() & kDynamicMask) == kDynamicMask &&
                   (base->GetFormID() & kDynamicMask) == kDynamicMask;
        }

        bool IsOCumRuntimeArmor(RE::TESObjectARMO* armor)
        {
            if (!armor) {
                return false;
            }

            auto* data = RE::TESDataHandler::GetSingleton();
            auto* ocum = data ? data->LookupModByName("OCum.esp") : nullptr;
            return ocum && armor->GetFile(0) == ocum;
        }
    }

    EquipmentLock& EquipmentLock::GetSingleton()
    {
        static EquipmentLock instance;
        return instance;
    }

    EquipmentLock::~EquipmentLock()
    {
        Stop();
    }

    void EquipmentLock::Start()
    {
        if (_running.exchange(true)) {
            return;
        }

        _config = Config::Load();

        SKSE::log::info(
            "Equipment lock started: interval={}ms slotMask=0x{:08X} ocumArmorExempt=1",
            _config.intervalMs,
            _config.slotMask);

        _worker = std::thread([this]() { WorkerLoop(); });
    }

    void EquipmentLock::Stop()
    {
        if (!_running.exchange(false)) {
            return;
        }

        {
            std::scoped_lock lock(_targetMutex);
            _manualTarget = 0;
            _threadTargets.clear();
            _ostimRefCounts.clear();
            _primaryThreadByActor.clear();
        }

        if (_worker.joinable()) {
            _worker.join();
        }
    }

    bool EquipmentLock::HasAnyTarget() const
    {
        std::scoped_lock lock(_targetMutex);
        return _manualTarget != 0 || !_ostimRefCounts.empty();
    }

    std::vector<RE::FormID> EquipmentLock::SnapshotTargets() const
    {
        std::vector<RE::FormID> out;
        std::scoped_lock lock(_targetMutex);

        if (_manualTarget != 0) {
            out.push_back(_manualTarget);
        }

        for (const auto& [id, count] : _ostimRefCounts) {
            if (count > 0 && id != _manualTarget) {
                out.push_back(id);
            }
        }

        return out;
    }

    void EquipmentLock::ToggleCrosshairActor()
    {
        auto* pick = RE::CrosshairPickData::GetSingleton();
        if (!pick) {
            return;
        }

        auto ref = pick->targetActor.get();
        if (!ref) {
            if (_config.debugNotifications) {
                RE::DebugNotification(
                    "OStim Together: aucun NPC vise");
            }
            return;
        }

        auto* actor = ref->As<RE::Actor>();
        if (!actor || actor->IsPlayerRef()) {
            if (_config.debugNotifications) {
                RE::DebugNotification(
                    "OStim Together: cible invalide");
            }
            return;
        }

        const auto id = actor->GetFormID();

        {
            std::scoped_lock lock(_targetMutex);

            if (_manualTarget == id) {
                _manualTarget = 0;

                SKSE::log::info(
                    "Manual equipment lock OFF {:08X}", id);

                if (_config.debugNotifications) {
                    RE::DebugNotification(
                        "OStim Together: verrou manuel OFF");
                }
                return;
            }

            _manualTarget = id;
        }

        SKSE::log::info(
            "Manual equipment lock ON {:08X} ({})",
            id,
            actor->GetDisplayFullName());

        if (_config.debugNotifications) {
            RE::DebugNotification(
                "OStim Together: verrou manuel ON");
        }

        ApplyLockMainThread(id);
    }

    void EquipmentLock::ClearManual()
    {
        {
            std::scoped_lock lock(_targetMutex);
            _manualTarget = 0;
        }

        if (_config.debugNotifications) {
            RE::DebugNotification(
                "OStim Together: verrou manuel efface");
        }
    }

    void EquipmentLock::AddOStimTarget(
        RE::Actor* actor,
        std::int32_t threadID)
    {
        if (!actor || actor->IsPlayerRef()) {
            return;
        }

        if (IsLikelySTRRemotePlayerProxy(actor)) {
            auto* base = actor->GetActorBase();
            SKSE::log::info(
                "EquipmentLock SKIP STR proxy thread={} actor={:08X} base={:08X}",
                threadID,
                actor->GetFormID(),
                base ? base->GetFormID() : 0);
            return;
        }

        const auto id = actor->GetFormID();
        std::optional<std::int32_t> previousPrimary;

        {
            std::scoped_lock lock(_targetMutex);

            auto& ids = _threadTargets[threadID];
            if (!ids.insert(id).second) {
                return;
            }

            if (const auto it = _primaryThreadByActor.find(id);
                it != _primaryThreadByActor.end()) {
                previousPrimary = it->second;
            }

            _primaryThreadByActor[id] = threadID;
            ++_ostimRefCounts[id];
        }

        SKSE::log::info(
            "OStim auto-lock ON thread={} actor={:08X} ({}) primaryPrevious={}",
            threadID,
            id,
            actor->GetDisplayFullName(),
            previousPrimary ? std::to_string(*previousPrimary) : "none");

        if (_config.debugNotifications) {
            RE::DebugNotification(
                "OStim Together: NPC OStim verrouille");
        }
    }

    void EquipmentLock::RemoveOStimThread(
        std::int32_t threadID)
    {
        std::vector<RE::FormID> released;
        std::vector<RE::FormID> forceReleased;

        {
            std::scoped_lock lock(_targetMutex);

            const auto it = _threadTargets.find(threadID);
            if (it == _threadTargets.end()) {
                return;
            }

            const auto actorsInStoppedThread = it->second;

            for (const auto id : actorsInStoppedThread) {
                const auto primaryIt = _primaryThreadByActor.find(id);
                const bool primaryStop =
                    primaryIt != _primaryThreadByActor.end() &&
                    primaryIt->second == threadID;

                if (primaryStop) {
                    // The newest thread for this NPC is the actual scene in
                    // the observed OStim startup sequence. If it ends while
                    // an older setup thread remains, purge all of those stale
                    // references so equipment/outfit cleanup cannot be held
                    // hostage by a missing auxiliary STOP callback.
                    for (auto threadIt = _threadTargets.begin();
                         threadIt != _threadTargets.end();) {
                        threadIt->second.erase(id);
                        if (threadIt->second.empty()) {
                            threadIt = _threadTargets.erase(threadIt);
                        } else {
                            ++threadIt;
                        }
                    }

                    _ostimRefCounts.erase(id);
                    _primaryThreadByActor.erase(id);
                    forceReleased.push_back(id);
                    continue;
                }

                auto countIt = _ostimRefCounts.find(id);
                if (countIt != _ostimRefCounts.end()) {
                    if (countIt->second > 1) {
                        --countIt->second;
                    } else {
                        _ostimRefCounts.erase(countIt);
                        released.push_back(id);
                        _primaryThreadByActor.erase(id);
                    }
                }
            }

            // The entry may already have been erased by primary cleanup.
            _threadTargets.erase(threadID);
        }

        for (const auto id : released) {
            SKSE::log::info(
                "OStim auto-lock OFF thread={} actor={:08X}",
                threadID,
                id);
        }

        for (const auto id : forceReleased) {
            auto* actor = ResolveActor(id);

            SKSE::log::info(
                "OStim auto-lock PRIMARY STOP actor={:08X} thread={} cleanup=all-threads",
                id,
                threadID);

            if (actor) {
                // CommonLibSSE-NG does not expose Actor::StopTranslation().
                // Purging every stale OStim lock/reference is the safe
                // ownership handoff here; avoid guessing at animation or
                // movement APIs that could disturb the NPC state.
                DefaultOutfitGuard::GetSingleton()
                    .ForceReleaseActor(actor);

                SKSE::log::info(
                    "OStim NPC POST-STOP RELEASE actor={:08X} staleThreadsPurged=1 outfitForce=1",
                    id);
            }
        }

        if ((!released.empty() || !forceReleased.empty()) &&
            _config.debugNotifications) {
            RE::DebugNotification(
                "OStim Together: verrou OStim relache");
        }
    }

    void EquipmentLock::RemoveOStimActor(RE::FormID actorID)
    {
        if (actorID == 0) {
            return;
        }

        std::vector<std::int32_t> removedThreads;

        {
            std::scoped_lock lock(_targetMutex);

            for (auto it = _threadTargets.begin();
                 it != _threadTargets.end();) {
                if (it->second.erase(actorID) != 0) {
                    removedThreads.push_back(it->first);
                }

                if (it->second.empty()) {
                    it = _threadTargets.erase(it);
                } else {
                    ++it;
                }
            }

            _ostimRefCounts.erase(actorID);
            _primaryThreadByActor.erase(actorID);
        }

        if (!removedThreads.empty()) {
            std::string threadList;
            for (std::size_t i = 0; i < removedThreads.size(); ++i) {
                if (i != 0) {
                    threadList += ",";
                }
                threadList += std::to_string(removedThreads[i]);
            }

            SKSE::log::info(
                "OStim auto-lock FORCE OFF actor={:08X} threads=[{}]",
                actorID,
                threadList);
        }
    }

    void EquipmentLock::ClearAllOStimTargets()
    {
        std::scoped_lock lock(_targetMutex);
        _threadTargets.clear();
        _ostimRefCounts.clear();
        _primaryThreadByActor.clear();

        SKSE::log::info(
            "All automatic OStim targets cleared");
    }

    void EquipmentLock::WorkerLoop()
    {
        while (_running.load()) {
            if (HasAnyTarget()) {
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask([]() {
                        EquipmentLock::GetSingleton()
                            .ApplyAllLocksMainThread();
                    });
                }
            }

            std::this_thread::sleep_for(
                std::chrono::milliseconds(_config.intervalMs));
        }
    }

    void EquipmentLock::ApplyAllLocksMainThread()
    {
        for (const auto id : SnapshotTargets()) {
            ApplyLockMainThread(id);
        }
    }

    RE::Actor* EquipmentLock::ResolveActor(
        RE::FormID actorID) const
    {
        if (actorID == 0) {
            return nullptr;
        }

        auto* form = RE::TESForm::LookupByID(actorID);
        return form ? form->As<RE::Actor>() : nullptr;
    }

    void EquipmentLock::ApplyLockMainThread(
        RE::FormID actorID)
    {
        auto* actor = ResolveActor(actorID);
        if (!actor || actor->IsPlayerRef() || actor->IsDead()) {
            return;
        }

        if (IsLikelySTRRemotePlayerProxy(actor)) {
            return;
        }

        auto* equipManager =
            RE::ActorEquipManager::GetSingleton();

        if (!equipManager) {
            return;
        }

        auto inventory = actor->GetInventory();

        for (auto& [object, data] : inventory) {
            if (!object ||
                !data.second ||
                !data.second->IsWorn()) {
                continue;
            }

            auto* armor = object->As<RE::TESObjectARMO>();
            if (!armor) {
                continue;
            }

            // OCum uses real armor records as transient overlay-bootstrap
            // helpers and as persistent OStim equip-object meshes. The NPC
            // anti-reequip lock runs every few milliseconds, so stripping
            // those records races OCum's own 50 ms overlay setup and keeps
            // creampie meshes unequipped until the OStim lock is released.
            // Exempt every armor whose defining file is OCum.esp; ordinary
            // clothing/armor remains protected by the existing slot mask.
            if (IsOCumRuntimeArmor(armor)) {
                continue;
            }

            const auto armorMask =
                static_cast<std::uint32_t>(
                    armor->GetSlotMask());

            if ((armorMask & _config.slotMask) == 0) {
                continue;
            }

            SKSE::log::trace(
                "Unequip {:08X} from actor {:08X}, slots=0x{:08X}",
                object->GetFormID(),
                actorID,
                armorMask);

            equipManager->UnequipObject(
                actor,
                object,
                nullptr,
                1,
                nullptr,
                false,
                true,
                false,
                true);
        }
    }
}
