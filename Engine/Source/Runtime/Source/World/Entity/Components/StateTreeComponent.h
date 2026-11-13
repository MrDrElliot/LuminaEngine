#pragma once

#include "AI/StateTree/StateTree.h"
#include "AI/StateTree/StateTreeExecution.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "Memory/SmartPtr.h"
#include "StateTreeComponent.generated.h"

namespace Lumina
{
    // Owned by one entity, so a copied component starts without an instance and the system starts it fresh.
    struct FStateTreeRunningInstance
    {
        // Keeps the runtime the instance points into alive, and is declared first so it outlives the instance.
        TStrongObjectPtr<CStateTree>   Tree;
        TUniquePtr<FStateTreeInstance> Instance;

        // Requests the system carries out on its next update, since a tree only starts or stops between its own ticks.
        bool bRestart = false;
        bool bStopped = false;

        FStateTreeRunningInstance() = default;
        FStateTreeRunningInstance(const FStateTreeRunningInstance&) {}
        FStateTreeRunningInstance(FStateTreeRunningInstance&&) noexcept = default;
        FStateTreeRunningInstance& operator=(const FStateTreeRunningInstance&) { Instance.reset(); Tree = nullptr; bRestart = bStopped = false; return *this; }
        FStateTreeRunningInstance& operator=(FStateTreeRunningInstance&&) noexcept = default;
    };

    // Destroys the tree's entity on entering its state, at the next sync point when the tree runs on a worker.
    REFLECT()
    struct RUNTIME_API SStateTreeDestroyOwnerTask : public SStateTreeTaskBase
    {
        GENERATED_BODY()

        EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context) const override;

        bool NeedsTick() const override { return false; }
    };

    // Runs a state tree on its entity, started by CStateTreeSystem on its first tick and restarted when StateTree changes.
    REFLECT(Component, Category = "AI")
    struct RUNTIME_API SStateTreeComponent
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "AI")
        TStrongObjectPtr<CStateTree> StateTree;

        // Replaces the tree's default parameters when it holds the tree's parameter struct.
        PROPERTY(Editable, Category = "AI")
        TInstancedStruct<SStateTreeParametersBase> Parameters;

        // Starts the tree over from its root on the system's next update, also after Stop or after it finished.
        FUNCTION()
        void Start() { Running.bRestart = true; Running.bStopped = false; }

        // Exits every active state on the system's next update and keeps the tree stopped until Start.
        FUNCTION()
        void Stop() { Running.bStopped = true; }

        // Wakes the instance on its next tick; false before it has started or when its event queue is full.
        FUNCTION()
        bool SendEvent(uint32 EventTag) { return Running.Instance != nullptr && Running.Instance->SendEvent(EventTag); }

        FUNCTION()
        bool IsRunning() const { return Running.Instance != nullptr && Running.Instance->GetStatus() == EStateTreeRunStatus::Running; }

        // The leaf of the active path, None when the tree is not running.
        FUNCTION()
        FName GetActiveState() const { return IsRunning() ? Running.Instance->GetActiveStateName() : FName(); }

        // The running instance's parameters when they are a Type, which bindings read again on the next tick, so a write takes effect live.
        void* GetParameters(const CStruct* Type) const { return Running.Instance != nullptr && Type != nullptr ? Running.Instance->GetParameters(Type) : nullptr; }

        template<typename T>
        T* GetParameters() const { return static_cast<T*>(GetParameters(T::StaticStruct())); }

        FUNCTION(ScriptInternal)
        void* GetParameterMemoryAs(FName TypeName) const;

        FStateTreeRunningInstance Running;
    };
}
