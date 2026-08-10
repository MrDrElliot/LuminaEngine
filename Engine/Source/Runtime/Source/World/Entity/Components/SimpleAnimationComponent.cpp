#include "RuntimePCH.h"
#include "SimpleAnimationComponent.h"

#include "Assets/AssetTypes/Mesh/Animation/Animation.h"

namespace Lumina
{
    bool SSimpleAnimationComponent::IsNotifyStateActive(const FName& NotifyName) const
    {
        if (!Animation.IsValid())
        {
            return false;
        }

        const TVector<FAnimationNotifyState>& States = Animation->GetNotifyStates();
        for (int32 Index : ActiveNotifyStates)
        {
            if (Index >= 0 && Index < (int32)States.size() && States[Index].NotifyName == NotifyName)
            {
                return true;
            }
        }
        return false;
    }

    float SSimpleAnimationComponent::GetCurveValue(const FName& CurveName, float Default) const
    {
        return Animation.IsValid() ? Animation->EvaluateCurve(CurveName, CurrentTime, Default) : Default;
    }
}
