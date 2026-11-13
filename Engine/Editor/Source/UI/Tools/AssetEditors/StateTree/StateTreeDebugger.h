#pragma once

#include "StateTreeEditing.h"
#include "AI/StateTree/StateTreeExecution.h"
#include "Containers/Optional.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "UI/Properties/PropertyTable.h"
#include "imgui.h"

namespace Lumina
{
    class CWorld;
    struct FStateTreeRunningInstance;

    // Watches one entity running the edited tree during Play, tracing it while watched and scrubbing back through the trace.
    class EDITOR_API FStateTreeDebugger
    {
    public:

        ~FStateTreeDebugger() { SetTarget(nullptr, ECS::NullEntity); }

        NODISCARD static ImVec4 GetStatusColor(EStateTreeRunStatus Status);

        // Stops tracing the previous target, which records nothing more until it is watched again.
        void SetTarget(CWorld* InWorld, ECS::FEntity InEntity);

        // The target's instance while it runs Tree, null otherwise.
        NODISCARD FStateTreeInstance* FindInstance(const CStateTree& Tree) const;

        // The authored state's status on the path at the scrubbed time, or live, and none when it is off the path.
        NODISCARD TOptional<EStateTreeRunStatus> GetStateStatus(const CStateTree& Tree, int32 State) const;

        void ScrubTo(double Time);
        void ResumeLive() { bLive = true; }

        // The target picker and the timeline of the target's trace.
        void Draw(const CStateTree& Tree);

        // The selected node's live instance data, read only.
        void DrawInstanceData(CStateTree& Tree, const FStateTreeSelection& Selection);

    private:

        // The target's running instance whichever tree it runs, null when it has none.
        NODISCARD FStateTreeRunningInstance* FindTarget() const;
        void DrawTargetPicker(const CStateTree& Tree);
        void DrawTimeline(const FStateTreeInstance& Instance, const FStateTreeTrace& Trace);

        TWeakObjectPtr<CWorld> World;
        ECS::FEntity           Entity = ECS::NullEntity;
        double                 ScrubTime = 0.0;
        bool                   bLive = true;
        FPropertyTable         InstanceDataTable;
    };
}
