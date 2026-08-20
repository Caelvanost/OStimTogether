#pragma once

#include "PCH.h"
#include "OStimAPI/ThreadEventListener.h"
#include "OStimAPI/ThreadInterface.h"

namespace OStimTogether
{
    class MirrorUndressRepair
    {
    public:
        static MirrorUndressRepair& GetSingleton();
        bool Initialize();

    private:
        class StartListener final : public OStim::ThreadEventListener
        {
        public:
            void listen(OStim::Thread* thread) override;
        };

        MirrorUndressRepair() = default;
        void HandleStart(OStim::Thread* thread);
        void Repair(std::int32_t threadID);

        OStim::ThreadInterface* _threads{ nullptr };
        StartListener _startListener;
        std::atomic_bool _initialized{ false };
    };
}
