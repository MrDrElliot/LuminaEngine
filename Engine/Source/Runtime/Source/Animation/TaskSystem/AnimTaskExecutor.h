#pragma once

#include "Animation/TaskSystem/AnimTask.h"
#include "Core/Math/Matrix/MatrixMath.h"

namespace Lumina
{
    namespace Anim
    {
        // Parallel-safe across entities, but one list executes on one thread. False leaves OutMatrices untouched.
        RUNTIME_API bool ExecuteTaskList(FAnimTaskList& List, TVector<FMatrix4>& OutMatrices,
                                         FAnimTaskSnapshot* OutSnapshot = nullptr);

        // The inertialization capture, with bScalarOnly forcing the per-bone path the vector one has to match.
        RUNTIME_API void InertCaptureForTest(FAnimInertializer& In, const FPose& Source, const FPose& SourcePrev, const FPose& Target,
                                             float Dt, bool bHasVel, int32 NumActiveBones, bool bScalarOnly);

        // Disarmed, the cost is a single relaxed atomic load per mesh, so this stays live-safe in a populated world.
        RUNTIME_API void ArmTaskCapture(const void* Owner);
        RUNTIME_API void DisarmTaskCapture();
        RUNTIME_API bool IsTaskCaptureArmed(const void* Owner);

        // Publish and copy-out are serialized by one mutex held for the duration of the copy.
        RUNTIME_API void StoreTaskCapture(const FAnimTaskSnapshot& Snapshot);
        RUNTIME_API bool GetTaskCapture(FAnimTaskSnapshot& OutSnapshot);
    }
}
