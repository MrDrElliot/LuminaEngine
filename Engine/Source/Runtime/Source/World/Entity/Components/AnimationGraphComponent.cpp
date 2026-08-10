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

    void SAnimationGraphComponent::SetFloat(const FName& ParameterName, float Value)
    {
        if (!Graph.IsValid())
        {
            return;
        }

        const int32 Index = Graph->FindParameterIndex(ParameterName);
        if (Index == INDEX_NONE)
        {
            return;
        }

        EnsureStateInitialized();
        if (Index < (int32)VMState.Parameters.size())
        {
            VMState.Parameters[Index] = Value;
        }
    }

    float SAnimationGraphComponent::GetFloat(const FName& ParameterName, float Default) const
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

    void SAnimationGraphComponent::SetBool(const FName& ParameterName, bool bValue)
    {
        SetFloat(ParameterName, bValue ? 1.0f : 0.0f);
    }

    bool SAnimationGraphComponent::GetBool(const FName& ParameterName, bool Default) const
    {
        return GetFloat(ParameterName, Default ? 1.0f : 0.0f) != 0.0f;
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
