#include "EditorPCH.h"
#include "Animation/AnimGraphOps.h"

#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Object/Package/Package.h"
#include "UI/Tools/NodeGraph/GraphNodeRegistry.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphNodeGraph.h"
#include "UI/Tools/NodeGraph/Animation/AnimStateMachineGraph.h"
#include "UI/Tools/NodeGraph/Animation/AnimStateTransition.h"

namespace Lumina::AnimGraphOps
{
    CAnimationGraphNodeGraph* FindOrCreateGraph(CAnimationGraph* Asset)
    {
        CPackage* Package = Asset != nullptr ? Asset->GetPackage() : nullptr;
        if (Package == nullptr)
        {
            return nullptr;
        }

        const FName GraphName = "AssetAnimationGraph";

        // A graph created since the last save is not in the export table LoadObjectByName reads.
        CAnimationGraphNodeGraph* Graph = nullptr;
        for (TObjectIterator<CAnimationGraphNodeGraph> It; It; ++It)
        {
            if ((*It)->GetPackage() == Package && (*It)->GetName() == GraphName
                && !(*It)->HasAnyFlag(OF_MarkedDestroy))
            {
                Graph = *It;
                break;
            }
        }

        if (Graph == nullptr)
        {
            Graph = Cast<CAnimationGraphNodeGraph>(Package->LoadObjectByName(GraphName));
        }

        if (Graph == nullptr)
        {
            Graph = NewObject<CAnimationGraphNodeGraph>(Package, GraphName);
        }

        Graph->SetAnimationGraph(Asset);
        Graph->EnsureSetup();

        return Graph;
    }

    TVector<CClass*> GetPlaceableNodeTypes(CEdNodeGraph* Graph)
    {
        TVector<CClass*> Types;
        if (Graph == nullptr)
        {
            return Types;
        }

        for (CClass* Class : FGraphNodeRegistry::Get().GetNodesForGraphClass(Graph->GetClass()))
        {
            if (Class != nullptr)
            {
                Types.push_back(Class);
            }
        }

        return Types;
    }

    CClass* ResolveNodeType(CEdNodeGraph* Graph, FStringView TypeName)
    {
        // Bound to a local because the getter returns by value, so end() must come from the same vector.
        const TVector<CClass*> Types = GetPlaceableNodeTypes(Graph);
        const auto It = Algo::FindIf(Types,
            [TypeName](CClass* Class) { return FStringView(Class->GetName().ToString()) == TypeName; });

        return It != Types.end() ? *It : nullptr;
    }

    TVector<CAnimStateTransition*> GetTransitions(CEdNodeGraph* Graph)
    {
        TVector<CAnimStateTransition*> Transitions;

        CAnimStateMachineGraph* Machine = Cast<CAnimStateMachineGraph>(Graph);
        if (Machine == nullptr)
        {
            return Transitions;
        }

        for (const TObjectPtr<CAnimStateTransition>& Transition : Machine->GetTransitions())
        {
            if (Transition.IsValid())
            {
                Transitions.push_back(Transition.Get());
            }
        }

        return Transitions;
    }

    bool Compile(CAnimationGraph* Asset, CAnimationGraphNodeGraph* NodeGraph, FAnimationGraphCompiler& Compiler)
    {
        if (Asset == nullptr || NodeGraph == nullptr)
        {
            return false;
        }

        // Resolved up front so Layered Blend Per Bone nodes can look them up during GenerateBytecode.
        if (Asset->Skeleton.IsValid())
        {
            Compiler.ResolveBoneMasks(Asset->BoneMaskDefs, Asset->Skeleton->GetSkeletonResource());
        }

        // Registered before the node walk so a runtime-chosen clip has slots to write into.
        for (const FName& CurveName : Asset->DeclaredCurves)
        {
            Compiler.AddCurve(CurveName);
        }

        // Give the compiler the parameter struct so it can warn about renamed or retyped fields.
        Compiler.SetDataStruct(Asset->GetParameterStruct());

        NodeGraph->CompileGraph(Compiler);

        if (Compiler.HasErrors())
        {
            return false;
        }

        Compiler.BuildGraph(Asset);
        return true;
    }
}
