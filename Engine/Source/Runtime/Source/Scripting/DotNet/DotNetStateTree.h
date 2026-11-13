#pragma once

#include "Platform/GenericPlatform.h"
#include "Scripting/ManagedTypeRegistry.h"

namespace Lumina
{
    class CStruct;
    class CWorld;
    class FStateTreeExecutionContext;

    // What the node base virtuals ask a C# node to run, numbered as EStateTreeScriptOp in LuminaSharp.
    enum class EStateTreeScriptOp : int32
    {
        EnterState,
        Tick,
        ExitState,
        OnEvent,
        TestCondition,
        NeedsTick,
    };
}

namespace Lumina::StateTreeScript
{
    // One call into a C# node, mirrored field for field by FStateTreeScriptCall in LuminaSharp.
    struct FCall
    {
        const FStateTreeExecutionContext* Context = nullptr;
        CWorld* World     = nullptr;
        void*   NodeData  = nullptr;
        double  Time      = 0.0;
        uint32  Owner     = 0;
        float   DeltaTime = 0.0f;
        uint32  EventTag  = 0;
        // What the op returns when the node cannot run.
        int32   Default   = 0;
    };

    // A struct minted from a C# type, which a state tree runs as a C# node.
    NODISCARD bool IsScriptNode(const CStruct* Type);

    // Only the three node bases hand their virtuals to C#, so a C# node has to extend one of them directly.
    NODISCARD bool ExtendsNodeBase(const CStruct* Type);

    // The managed entry every C# node runs through, null without the .NET host.
    NODISCARD void* ResolveDispatch();

    // Runs Op on the C# node laid out as Type, returning Call.Default without a host.
    FORCEINLINE int32 Invoke(void* Dispatch, const CStruct* Type, EStateTreeScriptOp Op, FCall& Call)
    {
        using FDispatchFn = int32 (*)(const CStruct*, int32, FCall*);
        return Dispatch != nullptr ? reinterpret_cast<FDispatchFn>(Dispatch)(Type, (int32)Op, &Call) : Call.Default;
    }

    // A C# task without a Tick lets its instance sleep, as a native one does by answering false from NeedsTick.
    NODISCARD RUNTIME_API bool NeedsTick(void* Dispatch, const CStruct* Type);

    // Recompiles every loaded tree a reload left on a reshaped or removed C# node, or that failed before its C# nodes existed.
    class FStateTreeStage final : public Scripting::IManagedTypeCompiler
    {
    public:

        const char* GetName() const override { return "StateTrees"; }

        void Compile(TSpan<const Scripting::FManagedTypeDefinition> Definitions, TSpan<const Scripting::FManagedTypeAlias> Aliases) override;
    };
}
