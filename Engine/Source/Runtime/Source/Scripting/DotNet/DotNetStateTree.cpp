#include "RuntimePCH.h"
#include "DotNetStateTree.h"

#include "DotNetExport.h"
#include "DotNetHost.h"
#include "AI/StateTree/StateTree.h"
#include "AI/StateTree/StateTreeExecution.h"
#include "Containers/Algorithm.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectIterator.h"
#include "Scripting/ScriptStruct.h"

namespace Lumina::StateTreeScript
{
    static_assert(sizeof(FCall) == 48, "FStateTreeScriptCall in LuminaSharp mirrors this layout.");

    bool IsScriptNode(const CStruct* Type)
    {
        return Type != nullptr && Cast<CScriptStruct>(Type) != nullptr;
    }

    bool ExtendsNodeBase(const CStruct* Type)
    {
        const CStruct* Base = static_cast<const CScriptStruct*>(Type)->GetNativeBase();
        return Base == SStateTreeTaskBase::StaticStruct() || Base == SStateTreeConditionBase::StaticStruct() || Base == SStateTreeEvaluatorBase::StaticStruct();
    }

    void* ResolveDispatch()
    {
        return DotNet::ResolveManagedExport("StateTreeDispatch");
    }

    bool NeedsTick(void* Dispatch, const CStruct* Type)
    {
        FCall Call;
        Call.Default = 1;
        return Invoke(Dispatch, Type, EStateTreeScriptOp::NeedsTick, Call) != 0;
    }

    void FStateTreeStage::Compile(TSpan<const Scripting::FManagedTypeDefinition>, TSpan<const Scripting::FManagedTypeAlias>)
    {
        auto RunsRetiredNode = [](const CStateTree* Tree)
        {
            return Algo::AnyOf(Tree->GetRuntime()->Nodes, [](const FStateTreeNode& Node)
            {
                return Node.bScript && static_cast<const CScriptStruct*>(Node.InstanceDataType.Get())->IsSuperseded();
            });
        };

        // A code-only reload keeps the runtime, so a C# task that gained a Tick would otherwise sleep in a state compiled without one.
        auto SleepsThroughNewTick = [](const CStateTree* Tree)
        {
            const FStateTreeRuntime& Runtime = *Tree->GetRuntime();
            return Algo::AnyOf(Runtime.States, [&](const FStateTreeRuntimeState& State)
            {
                for (uint16 Index = State.FirstTask; !State.bNeedsTick && Index < State.FirstTask + State.NumTasks; ++Index)
                {
                    const FStateTreeNode& Node = Runtime.Nodes[Index];
                    if (Node.bScript && NeedsTick(Runtime.ScriptDispatch, Node.InstanceDataType.Get()))
                    {
                        return true;
                    }
                }
                return false;
            });
        };

        // Gathered first, since compiling a tree recompiles the trees linking it.
        TVector<CStateTree*> Stale;
        for (TObjectIterator<CStateTree> It; It; ++It)
        {
            CStateTree* Tree = *It;
            if (!Tree->HasAnyFlag(OF_NeedsLoad | OF_NeedsPostLoad) && (RunsRetiredNode(Tree) || SleepsThroughNewTick(Tree) || !Tree->GetCompileErrors().IsEmpty()))
            {
                Stale.Add(Tree);
            }
        }
        for (CStateTree* Tree : Stale)
        {
            Tree->Compile();
        }
    }
}

using namespace Lumina;

LUMINA_DOTNET_EXPORT(void, StateTreeRequestWakeAt)(const FStateTreeExecutionContext* Context, double WakeTime)
{
    Context->RequestWakeAt(WakeTime);
}

LUMINA_DOTNET_EXPORT(int, StateTreeSendEvent)(const FStateTreeExecutionContext* Context, uint32 EventTag)
{
    return Context->SendEvent(EventTag) ? 1 : 0;
}
