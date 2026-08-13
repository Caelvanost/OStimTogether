#include "PCH.h"
#include "EquipmentLock.h"

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
            "Equipment lock started: interval={}ms slotMask=0x{:08X}",
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

        // v0.18.16 diagnostic: do not fight Skyrim Together's remote-player
        // equipment replication with UnequipObject every 25 ms. That churn
        // starts exactly when the proxy enters OStim and is the strongest
        // remaining suspect for RaceMenu/overlay/attached-visual loss.
        // Real NPCs keep the existing automatic equipment lock.
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

        {
            std::scoped_lock lock(_targetMutex);

            auto& ids = _threadTargets[threadID];
            if (!ids.insert(id).second) {
                return;
            }

            ++_ostimRefCounts[id];
        }

        SKSE::log::info(
            "OStim auto-lock ON thread={} actor={:08X} ({})",
            threadID,
            id,
            actor->GetDisplayFullName());

        if (_config.debugNotifications) {
            RE::DebugNotification(
                "OStim Together: NPC OStim verrouille");
        }

        // IMPORTANT:
        // Do not touch the actor's inventory directly from OStim's
        // ThreadStart callback. OStim can still be constructing the thread
        // and manipulating actor state at that point.
        //
        // The worker will notice the new target and enqueue the first
        // equipment pass through SKSE::TaskInterface on the game thread.
    }

    void EquipmentLock::RemoveOStimThread(
        std::int32_t threadID)
    {
        std::vector<RE::FormID> released;

        {
            std::scoped_lock lock(_targetMutex);

            const auto it = _threadTargets.find(threadID);
            if (it == _threadTargets.end()) {
                return;
            }

            for (const auto id : it->second) {
                auto countIt = _ostimRefCounts.find(id);
                if (countIt == _ostimRefCounts.end()) {
                    continue;
                }

                if (countIt->second > 1) {
                    --countIt->second;
                } else {
                    _ostimRefCounts.erase(countIt);
                    released.push_back(id);
                }
            }

            _threadTargets.erase(it);
        }

        for (const auto id : released) {
            SKSE::log::info(
                "OStim auto-lock OFF thread={} actor={:08X}",
                threadID,
                id);
        }

        if (!released.empty() && _config.debugNotifications) {
            RE::DebugNotification(
                "OStim Together: verrou OStim relache");
        }
    }

    void EquipmentLock::ClearAllOStimTargets()
    {
        std::scoped_lock lock(_targetMutex);
        _threadTargets.clear();
        _ostimRefCounts.clear();

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

        // Defensive guard for targets captured before a proxy was identified.
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
