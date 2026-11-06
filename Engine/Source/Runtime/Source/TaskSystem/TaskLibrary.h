#pragma once

#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectMacros.h"
#include "Scripting/ScriptCallback.h"
#include "TaskLibrary.generated.h"

namespace Lumina
{
    // Scheduling work on the engine's job workers. A body that blocks or yields breaks CLR affinity.
    REFLECT()
    class RUNTIME_API CTaskLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        // Runs Body once on a worker and returns a handle, which the caller has to Release exactly once.
        FUNCTION()
        static uint64 Run(SScriptCallback Body, int32 Priority);

        // Runs Body over [0, Num) in chunks and returns once all are done; each payload holds Start low and End high.
        FUNCTION(ScriptInternal)
        static void ParallelFor(uint32 Num, uint32 MinRange, SScriptCallback Body, int32 Priority);

        // Blocks until the task behind Handle has completed.
        FUNCTION()
        static void Wait(uint64 Handle);

        // Drops the handle's refcount on the completion state.
        FUNCTION()
        static void Release(uint64 Handle);

        // Blocks until every task submitted so far has completed.
        FUNCTION()
        static void WaitForAll();

        // Number of job worker threads.
        FUNCTION()
        static int32 NumWorkers();
    };
}
