#pragma once

#include "Animation/TaskSystem/AnimTask.h"
#include "Core/Math/Matrix/MatrixMath.h"

namespace Lumina
{
    namespace Anim
    {
        // Runs a recorded task list into GPU skinning matrices, then clears the list. Only tasks
        // reachable from the output task execute (inactive state-machine branches are skipped).
        // Pose buffers come from a per-thread pool with steal-in-place semantics, so a whole entity
        // costs at most a few live buffers regardless of graph size. Parallel-safe across entities;
        // one list executes on one thread.
        RUNTIME_API void ExecuteTaskList(FAnimTaskList& List, TVector<FMatrix4>& OutMatrices);
    }
}
