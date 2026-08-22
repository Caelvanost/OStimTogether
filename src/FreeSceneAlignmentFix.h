#pragma once

#include "PCH.h"
#include "OStimAPI/ThreadEventListener.h"
#include "OStimAPI/ThreadInterface.h"

namespace OStimTogether
{
    class FreeSceneAlignmentFix
    {
    public:
        static FreeSceneAlignmentFix& GetSingleton();
        bool Initialize();

    private:
        FreeSceneAlignmentFix() = default;

        class StartListener final : public OStim::ThreadEventListener
        {
        public:
            void listen(OStim::Thread* thread) override;
        };

        void HandleStart(OStim::Thread* thread);
        void ReleaseFreeSceneProxy(std::int32_t threadID);

        OStim::ThreadInterface* _threads{ nullptr };
        StartListener _startListener;
        std::atomic_bool _initialized{ false };
    };
}
