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
        //
        // OutSnapshot (optional) records what actually happened -- reachability, execution order,
        // buffer ownership -- as the executor decides it, so tools never have to re-derive it.
        RUNTIME_API void ExecuteTaskList(FAnimTaskList& List, TVector<FMatrix4>& OutMatrices,
                                         FAnimTaskSnapshot* OutSnapshot = nullptr);

        // Debug capture. A tool arms one component (any stable pointer identifying the mesh); the
        // animation system then passes a snapshot for that component only. Disarmed, the cost is a
        // single relaxed atomic load per mesh, so this stays live-safe in a populated world.
        RUNTIME_API void ArmTaskCapture(const void* Owner);
        RUNTIME_API void DisarmTaskCapture();
        RUNTIME_API bool IsTaskCaptureArmed(const void* Owner);

        // Publishes a filled snapshot (animation thread) / copies the last published one out (tool
        // thread). Both sides are serialized by one mutex held for the duration of the copy.
        RUNTIME_API void StoreTaskCapture(const FAnimTaskSnapshot& Snapshot);
        RUNTIME_API bool GetTaskCapture(FAnimTaskSnapshot& OutSnapshot);
    }
}
