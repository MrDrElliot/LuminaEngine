#include "EditorPCH.h"
#include "Animation/AnimGraphOps.h"

#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Object/Package/Package.h"
#include "Animation/AnimGraphPlan.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "UI/Tools/NodeGraph/Animation/AnimGraphDebugView.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphNodeGraph.h"
#include "UI/Tools/NodeGraph/Animation/AnimStateMachineGraph.h"
#include "UI/Tools/NodeGraph/Animation/AnimStateTransition.h"
#include "UI/Tools/NodeGraph/Animation/Nodes/AnimGraphNode_GetObjectParameter.h"
#include "UI/Tools/NodeGraph/Animation/Nodes/AnimGraphNode_GetParameter.h"
#include "World/Entity/Components/Component.h"
#include "World/World.h"

namespace Lumina::AnimGraphOps
{
    void* FindInputComponent(CWorld& World, ECS::FEntity Entity, const CStruct& Struct, EAnimGraphInputLookup Lookup)
    {
        const FComponentOps* Ops = Struct.GetComponentOps();
        if (Ops == nullptr)
        {
            return nullptr;
        }
        ECS::FRegistry& Registry = World.GetEntityRegistry();
        for (ECS::FEntity At = Entity; !At.IsNull() && World.IsValidEntity(At);
             At = Lookup == EAnimGraphInputLookup::SelfOrAncestor ? World.GetParent(At) : ECS::NullEntity)
        {
            if (Ops->Has(Registry, At))
            {
                return Ops->Get(Registry, At);
            }
        }
        return nullptr;
    }

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

    TVector<CAnimStateTransition*> GetTransitions(CEdNodeGraph* Graph)
    {
        TVector<CAnimStateTransition*> Transitions;

        CAnimStateMachineGraph* Machine = Cast<CAnimStateMachineGraph>(Graph);
        if (Machine == nullptr)
        {
            return Transitions;
        }

        for (const TStrongObjectPtr<CAnimStateTransition>& Transition : Machine->GetTransitions())
        {
            if (Transition.IsValid())
            {
                Transitions.Add(Transition.Get());
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

        // Give the compiler the parameter struct and the inputs so it can warn about renamed or retyped fields.
        Compiler.SetDataStruct(Asset->GetParameterStruct());
        Asset->RefreshInputStructs();
        Compiler.SetGraph(Asset);

        NodeGraph->CompileGraph(Compiler);

        if (Compiler.HasErrors())
        {
            return false;
        }

        Compiler.BuildGraph(Asset);
        return true;
    }

    namespace
    {
        // Old.Field becomes New.Field, and a path under any other input is left alone.
        bool RenamePath(FName& Path, FStringView OldPrefix, const FString& NewPrefix)
        {
            const FString Text = Path.ToString();
            if (!FStringView(Text).StartsWith(OldPrefix))
            {
                return false;
            }
            FString Renamed = NewPrefix;
            Renamed += FStringView(Text).Mid(OldPrefix.Len());
            Path = FName(*Renamed);
            return true;
        }

        void RenameInGraph(CEdNodeGraph* Graph, FStringView OldPrefix, const FString& NewPrefix, THashSet<CEdNodeGraph*>& Seen, TVector<CEdNodeGraph*>& Changed)
        {
            if (Graph == nullptr || !Seen.TryAdd(Graph))
            {
                return;
            }

            bool bChanged = false;
            for (const TStrongObjectPtr<CEdGraphNode>& NodeRef : Graph->Nodes)
            {
                CEdGraphNode* Node = NodeRef.Get();
                if (CAnimGraphNode_GetParameter* Get = Cast<CAnimGraphNode_GetParameter>(Node))
                {
                    bChanged |= RenamePath(Get->ParameterName, OldPrefix, NewPrefix);
                }
                else if (CAnimGraphNode_GetObjectParameter* GetObject = Cast<CAnimGraphNode_GetObjectParameter>(Node))
                {
                    bChanged |= RenamePath(GetObject->ParameterName, OldPrefix, NewPrefix);
                }
                if (Node != nullptr)
                {
                    RenameInGraph(Node->GetOwnedSubGraph(), OldPrefix, NewPrefix, Seen, Changed);
                }
            }
            for (CAnimStateTransition* Transition : GetTransitions(Graph))
            {
                for (SAnimTransitionCondition& Condition : Transition->Conditions)
                {
                    bChanged |= RenamePath(Condition.ParameterName, OldPrefix, NewPrefix);
                }
            }
            if (bChanged)
            {
                Changed.Add(Graph);
            }
        }
    }

    TVector<CEdNodeGraph*> RenameInputPaths(CAnimationGraphNodeGraph* Root, FName OldInput, FName NewInput)
    {
        TVector<CEdNodeGraph*> Changed;
        THashSet<CEdNodeGraph*> Seen;
        const FString OldPrefix = OldInput.ToString() + ".";
        RenameInGraph(Root, OldPrefix, NewInput.ToString() + ".", Seen, Changed);
        return Changed;
    }

    void BuildCrowdView(const CAnimationGraph& Asset, const FAnimationGraphCompiler& Compiler, FAnimGraphCrowdView& Out)
    {
        const bool bShow = Out.bShow;
        Out = FAnimGraphCrowdView();
        Out.bShow = bShow;

        FAnimGraphPlan Full;
        FAnimGraphPlan Crowd;
        FAnimPlanSources Sources;
        TVector<EAnimCrowdOpFate> Fates;
        LowerAnimGraphPlan(Asset, Full, &Sources);
        PruneAnimGraphPlanForCrowd(Asset, Full, Crowd, &Fates);

        // An overlay also writes the bind pose for an unconnected input, so one bypassed op marks the node whatever else it kept.
        struct FNodeFates
        {
            bool bKept     = false;
            bool bBypassed = false;
        };
        THashMap<const CEdGraphNode*, FNodeFates> ByNode;
        for (int32 i = 0; i < Full.Ops.Num(); ++i)
        {
            const CEdGraphNode* Node = Compiler.FindOpOwner(Sources.Ops[i]);
            if (Node == nullptr)
            {
                continue;
            }
            FNodeFates& NodeFates = ByNode.FindOrAdd(Node);
            NodeFates.bKept     |= Fates[i] == EAnimCrowdOpFate::Kept;
            NodeFates.bBypassed |= Fates[i] == EAnimCrowdOpFate::Bypassed;

            const FAnimPlanOp& Op = Full.Ops[i];
            const uint16 ClipIndex = Op.U[0];
            if (Fates[i] == EAnimCrowdOpFate::Kept && (EAnimOp)Op.Op == EAnimOp::SampleAnim && ClipIndex < Asset.Clips.Num() && Asset.Clips[ClipIndex].IsValid())
            {
                const CAnimation* Clip = Asset.Clips[ClipIndex].Get();
                if ((Clip->IsAdditive() || Clip->ScalesBones()) && !Out.CpuClips.Contains(Clip))
                {
                    Out.CpuClips.Add(Clip);
                }
            }
        }

        // A load lifted out of the ops survives in the crowd plan exactly when the crowd still reads its register.
        const auto TallyLoads = [&](const auto& FullLoads, const auto& CrowdLoads, const TVector<uint32>& Offsets)
        {
            for (int32 i = 0; i < FullLoads.Num(); ++i)
            {
                const CEdGraphNode* Node = Compiler.FindOpOwner(Offsets[i]);
                if (Node == nullptr)
                {
                    continue;
                }
                const uint16 Register = FullLoads[i].Register;
                ByNode.FindOrAdd(Node).bKept |= CrowdLoads.ContainsByPredicate([Register](const auto& Load) { return Load.Register == Register; });
            }
        };
        TallyLoads(Full.Constants, Crowd.Constants, Sources.Constants);
        TallyLoads(Full.ParameterLoads, Crowd.ParameterLoads, Sources.ParameterLoads);
        TallyLoads(Full.ObjectParameterLoads, Crowd.ObjectParameterLoads, Sources.ObjectParameterLoads);
        TallyLoads(Full.ObjectConstantLoads, Crowd.ObjectConstantLoads, Sources.ObjectConstantLoads);

        Out.NumNodes = ByNode.Num();
        for (const auto& [Node, NodeFates] : ByNode)
        {
            if (NodeFates.bBypassed)
            {
                Out.Skipped.FindOrAdd(Node) = EAnimCrowdNodeFate::Bypassed;
            }
            else if (!NodeFates.bKept)
            {
                Out.Skipped.FindOrAdd(Node) = EAnimCrowdNodeFate::Unused;
            }
        }
    }
}
