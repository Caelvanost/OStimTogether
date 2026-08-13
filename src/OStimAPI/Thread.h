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

        // Exact public ABI v1 prefix shared by OStim 7.4c and 7.5b.
        // OStim 7.5b appends ABI v3 furniture accessors after this prefix;
        // none are needed by OStim Together.
        virtual void forEachThreadActor(void* visitor) = 0;
        virtual Node* getCurrentNode() = 0;
    };
}
