#include "RuntimePCH.h"
#include "AnimationGraphComponent.h"

#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"

namespace Lumina
{
    void SAnimationGraphComponent::EnsureStateInitialized()
    {
        // Re-init when the parameter count drifts: an editor edit can add a transition condition
        // referencing a new parameter, leaving VMState.Parameters too small to address it.
        if (!VMState.bInitialized ||
            VMState.SourceGraph != Graph.Get() ||
            (Graph.IsValid() && VMState.Parameters.size() != Graph->Parameters.size()))
        {
            FAnimationGraphVM::InitState(Graph.Get(), VMState);
        }
    }

    void SAnimationGraphComponent::EnsureParametersInitialized() const
    {
        CStruct* Wanted = Graph.IsValid() ? Graph->GetParameterStruct() : nullptr;
        if (Parameters.GetScriptStruct() == Wanted)
        {
            return;
        }

        FInstancedStruct& Mutable = const_cast<FInstancedStruct&>(Parameters);
        if (Wanted == nullptr)
        {
            Mutable.Reset();
            return;
        }

        // Seeded from the graph's own instance, so its authored values are this entity's starting point.
        Mutable = Graph->ParameterStruct;
    }

    void* SAnimationGraphComponent::GetParameterMemory()
    {
        EnsureParametersInitialized();
        return Parameters.GetMutableMemory();
    }

    float SAnimationGraphComponent::GetEvaluatedParameter(const FName& ParameterName, float Default) const
    {
        if (!Graph.IsValid())
        {
            return Default;
        }

        const int32 Index = Graph->FindParameterIndex(ParameterName);
        if (Index == INDEX_NONE || Index >= (int32)VMState.Parameters.size())
        {
            return Default;
        }

        return VMState.Parameters[Index];
    }

    bool SAnimationGraphComponent::HasParameter(const FName& ParameterName) const
    {
        return Graph.IsValid() && Graph->FindParameterIndex(ParameterName) != INDEX_NONE;
    }

    float SAnimationGraphComponent::GetCurveValue(const FName& CurveName, float Default) const
    {
        if (!Graph.IsValid())
        {
            return Default;
        }

        const int32 Index = Graph->FindCurveIndex(CurveName);
        if (Index == INDEX_NONE || Index >= (int32)VMState.CurveValues.size())
        {
            return Default;
        }

        return VMState.CurveValues[Index];
    }

    bool SAnimationGraphComponent::HasCurve(const FName& CurveName) const
    {
        return Graph.IsValid() && Graph->FindCurveIndex(CurveName) != INDEX_NONE;
    }
}
