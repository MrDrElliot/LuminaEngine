#pragma once

#include "StateTreeTrace.h"
#include "Core/Math/Random.h"
#include "Memory/SmartPtr.h"
#include "World/ECS/Entity.h"

namespace Lumina
{
    class CWorld;
    class FStateTreeInstance;
    enum class EStateTreeScriptOp : int32;

    // What a node sees while it runs; GetInstanceData returns the running node's own data.
    class FStateTreeExecutionContext
    {
    public:

        FStateTreeExecutionContext(FStateTreeInstance& InInstance, CWorld* InWorld, ECS::FEntity InOwner, double InTime, float InDeltaTime)
            : Instance(InInstance), World(InWorld), Owner(InOwner), Time(InTime), DeltaTime(InDeltaTime)
        {}

        NODISCARD CWorld* GetWorld() const { return World; }
        NODISCARD ECS::FEntity GetOwner() const { return Owner; }
        NODISCARD double GetTime() const { return Time; }
        NODISCARD float GetDeltaTime() const { return DeltaTime; }

        template<typename T>
        NODISCARD T& GetInstanceData() const
        {
            DEBUG_ASSERT(NodeData != nullptr && T::StaticStruct() == NodeDataType);
            return *reinterpret_cast<T*>(NodeData);
        }

        // Wakes a sleeping instance at Time, for a task that does not need every tick.
        RUNTIME_API void RequestWakeAt(double WakeTime) const;

        // Queued for the next tick; false when the queue is full.
        RUNTIME_API bool SendEvent(uint32 EventTag) const;

        // Seeded by Start, so a tree's random choices repeat for the same seed.
        NODISCARD RUNTIME_API FRandomStream& GetRandom() const;

        // The parameters of the tree the running node was authored in, null when that tree has none.
        NODISCARD RUNTIME_API void* GetParameters(const CStruct*& OutType) const;

        // Runs Op on the running C# node and returns its result, or Default when the node is native.
        RUNTIME_API int32 CallScript(EStateTreeScriptOp Op, int32 Default, uint32 EventTag = 0) const;

    private:

        friend class FStateTreeInstance;

        FStateTreeInstance& Instance;
        CWorld*             World       = nullptr;
        ECS::FEntity        Owner       = ECS::NullEntity;
        double              Time        = 0.0;
        float               DeltaTime   = 0.0f;
        uint8*              NodeData    = nullptr;
        const CStruct*      NodeDataType = nullptr;
        uint16              NodeScope   = 0;
        bool                bScriptNode  = false;
    };

    // One running tree, its node data in a single block allocated once by Start.
    class FStateTreeInstance
    {
    public:

        FStateTreeInstance() = default;

        // Stops at the last tick's time, so a removed component or destroyed entity still exits its active tasks.
        RUNTIME_API ~FStateTreeInstance();

        FStateTreeInstance(const FStateTreeInstance&) = delete;
        FStateTreeInstance& operator=(const FStateTreeInstance&) = delete;

        // Parameters replaces the tree's defaults when it holds the same struct.
        RUNTIME_API EStateTreeRunStatus Start(TSharedPtr<const FStateTreeRuntime> InRuntime, CWorld* InWorld, ECS::FEntity InOwner, double Time, const FInstancedStruct* Parameters = nullptr, uint64 Seed = 0);

        // Does nothing while the instance sleeps, so a caller may tick every instance every frame. Stops when the runtime was superseded.
        RUNTIME_API EStateTreeRunStatus Tick(double Time);

        // Exits every active state leaf to root and frees the block.
        RUNTIME_API void Stop(double Time);

        // Queued for the next tick; false when the queue is full.
        RUNTIME_API bool SendEvent(uint32 EventTag);

        // Nothing on the active path ticks and no event is queued, so only NextWakeTime can move it.
        NODISCARD RUNTIME_API bool IsWaiting() const;

        // Earliest time a task or a delayed transition asked to wake at, infinity when none.
        NODISCARD RUNTIME_API double GetNextWakeTime() const;

        // The tree recompiled after this instance started, so it needs a restart on the new runtime.
        NODISCARD bool IsSuperseded() const { return Runtime != nullptr && Runtime->bSuperseded; }

        NODISCARD EStateTreeRunStatus GetStatus() const { return Status; }
        NODISCARD const FStateTreeRuntime* GetRuntime() const { return Runtime.get(); }
        NODISCARD TSpan<const FStateTreeActiveState> GetActivePath() const { return TSpan<const FStateTreeActiveState>(ActivePath.GetData(), PathDepth); }

        // A node's instance data, for tools and tests reading what a node wrote.
        NODISCARD void* GetNodeInstanceData(uint16 NodeIndex) const { return Block + Runtime->Nodes[NodeIndex].InstanceDataOffset; }

        // The leaf of the active path, None when nothing is active.
        NODISCARD FName GetActiveStateName() const { return PathDepth > 0 ? Runtime->States[ActivePath[PathDepth - 1].State].Name : FName(); }

        // The root tree's parameters as bindings read them, null unless they are a Type and the instance runs.
        NODISCARD RUNTIME_API void* GetParameters(const CStruct* Type) const;

        // A debugger watching this instance records its changes into a bounded trace; shipping compiles tracing out.
        RUNTIME_API void SetTraced(bool bTraced);
        NODISCARD RUNTIME_API const FStateTreeTrace* GetTrace() const;

    private:

        friend class FStateTreeExecutionContext;

        struct FPendingTransition
        {
            uint16 Transition = kStateTreeNoIndex;
            double FireTime   = 0.0;
        };

        using FPath = TArray<FStateTreeActiveState, kStateTreeMaxDepth>;

        template<typename T>
        const T& NodeAs(uint16 NodeIndex) const { return *static_cast<const T*>(Runtime->Nodes[NodeIndex].Definition.GetMemory()); }

        FStateTreeExecutionContext MakeContext(double Time, float DeltaTime);
        void InitializeNodeData(const FStateTreeNode& Node) const;
        void RunCopies(uint16 First, uint16 Num) const;
        void Bind(FStateTreeExecutionContext& Context, uint16 NodeIndex) const;

        bool TestConditions(FStateTreeExecutionContext& Context, uint16 First, uint16 Num) const;
        bool SelectState(FStateTreeExecutionContext& Context, uint16 State, FPath& Path, int32& Depth) const;
        bool SelectChildren(FStateTreeExecutionContext& Context, uint16 Parent, FPath& Path, int32& Depth) const;
        bool SelectPathTo(FStateTreeExecutionContext& Context, uint16 Target, FPath& Path, int32& Depth) const;

        void ExitStates(FStateTreeExecutionContext& Context, int32 KeepDepth);
        void EnterStates(FStateTreeExecutionContext& Context, const FPath& Path, int32 Depth, int32 KeepDepth);
        void ChangeState(FStateTreeExecutionContext& Context, uint16 Target);
        void Finish(FStateTreeExecutionContext& Context, EStateTreeRunStatus Result);

        void TickEvaluators(FStateTreeExecutionContext& Context);
        bool IsTriggered(const FStateTreeRuntimeTransition& Transition, EStateTreeRunStatus Completion, TSpan<const uint32> TickEvents) const;
        void UpdateTransitions(FStateTreeExecutionContext& Context, TSpan<const uint32> TickEvents);
        void ExecuteTransition(FStateTreeExecutionContext& Context, uint16 TransitionIndex);

        void ReleaseBlock();

        // One null check while no debugger watches, Depth being how much of the active path the change leaves.
        void TraceChange(const FStateTreeExecutionContext& Context, EStateTreeTraceKind Kind, uint16 State, EStateTreeRunStatus StateStatus, uint32 Value, int32 Depth) const
        {
#if !defined(LE_SHIPPING)
            if (Trace != nullptr)
            {
                RecordTrace(Context.GetTime(), Kind, State, StateStatus, Value, Depth);
            }
#endif
        }

#if !defined(LE_SHIPPING)
        void RecordTrace(double Time, EStateTreeTraceKind Kind, uint16 State, EStateTreeRunStatus StateStatus, uint32 Value, int32 Depth) const;

        TUniquePtr<FStateTreeTrace> Trace;
#endif

        TSharedPtr<const FStateTreeRuntime> Runtime;
        uint8*                   Block   = nullptr;
        CWorld*                  World   = nullptr;
        ECS::FEntity             Owner   = ECS::NullEntity;
        FRandomStream            Random;

        FPath  ActivePath = {};
        int32  PathDepth  = 0;
        bool   bPathNeedsTick = false;

        TArray<uint32, kStateTreeMaxEvents> Events = {};
        int32 NumEvents = 0;

        FPendingTransition  Pending;
        double              TaskWakeTime = 0.0;
        double              LastTickTime = 0.0;
        EStateTreeRunStatus Status = EStateTreeRunStatus::Failed;
    };
}
