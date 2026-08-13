#pragma once
#include <cstdint>
#include "ThreadActor.h"
#include "Node.h"

namespace OStim
{
    class Thread
    {
    public:
        virtual std::int32_t getThreadID() = 0;
        virtual bool isPlayerThread() = 0;
        virtual std::uint32_t getActorCount() = 0;
        virtual ThreadActor* getActor(std::uint32_t position) = 0;

        // Exact public OStim 7.4c Thread ABI.
        // Do not append concrete/private Thread methods here.
        virtual void forEachThreadActor(void* visitor) = 0;
        virtual Node* getCurrentNode() = 0;
    };
}
