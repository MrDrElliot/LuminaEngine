#pragma once

#include <box3d/box3d.h>
#include "Core/Threading/Atomic.h"
#include "TaskSystem/TaskSystem.h"

namespace Lumina::Physics
{
    // Routes Box3D's enqueue/finish task pair onto the engine job runtime's shared worker pool.
    class RUNTIME_API FBox3DTaskBridge
    {
    public:

        FBox3DTaskBridge();
        ~FBox3DTaskBridge();

        LE_NO_COPYMOVE(FBox3DTaskBridge);

        static void* EnqueueTask(b3TaskCallback* Task, void* TaskContext, void* UserContext, const char* Name);
        static void FinishTask(void* UserTask, void* UserContext);

        uint32 GetWorkerCount() const { return WorkerCount; }

    private:

        struct FTaskSlot
        {
            b3TaskCallback* Task = nullptr;
            void*           Context = nullptr;
            Jobs::FTaskEvent Done;
            TAtomic<bool>   bInUse{ false };
        };

        static Jobs::TTask<void> RunTask(FTaskSlot* Slot);

        FTaskSlot       Slots[B3_MAX_TASKS];
        TAtomic<uint32> NextSlotHint{ 0 };
        uint32          WorkerCount = 1;
    };
}
