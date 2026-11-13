#include "RuntimePCH.h"
#include "StateTree.h"

#include "Containers/Algorithm.h"
#include "Core/Object/ObjectIterator.h"
#include "Log/Log.h"

namespace Lumina
{
    void CStateTree::PostLoad()
    {
        Super::PostLoad();
        Compile();
    }

    void CStateTree::PostPropertyChange(FProperty* ChangedProperty)
    {
        Super::PostPropertyChange(ChangedProperty);
        Compile();
    }

    bool CStateTree::Compile(bool bLogErrors)
    {
        const bool bCompiled = CompileSelf(bLogErrors);

        // Linked states copy the linked tree's states at compile time, so every tree reaching this one holds a stale copy.
        TVector<CStateTree*> Trees;
        for (TObjectIterator<CStateTree> It; It; ++It)
        {
            Trees.Add(*It);
        }
        TVector<const CStateTree*> Recompiled = { this };
        for (int32 Index = 0; Index < Recompiled.Num(); ++Index)
        {
            for (CStateTree* Tree : Trees)
            {
                const bool bLinks = Algo::AnyOf(Tree->States, [Linked = Recompiled[Index]](const SStateTreeState& State) { return State.LinkedSubtree.Get() == Linked; });
                if (bLinks && !Tree->HasAnyFlag(OF_NeedsLoad | OF_NeedsPostLoad) && !Algo::Contains(Recompiled, Tree))
                {
                    Tree->CompileSelf(bLogErrors);
                    Recompiled.Add(Tree);
                }
            }
        }
        return bCompiled;
    }

    bool CStateTree::CompileSelf(bool bLogErrors)
    {
        TSharedPtr<FStateTreeRuntime> Compiled = MakeShared<FStateTreeRuntime>();
        FStateTreeCompiler Compiler;
        const bool bCompiled = Compiler.Compile(*this, *Compiled);
        Runtime->bSuperseded = true;
        Runtime = Move(Compiled);
        CompileErrors = Compiler.GetErrors();
        if (bLogErrors)
        {
            for (const FString& Error : CompileErrors)
            {
                LOG_ERROR("State tree {} {}", GetName().ToString(), Error);
            }
        }
        return bCompiled;
    }
}
