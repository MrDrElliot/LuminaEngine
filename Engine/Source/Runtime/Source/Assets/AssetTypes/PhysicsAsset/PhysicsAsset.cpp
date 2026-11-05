#include "PhysicsAsset.h"

namespace Lumina
{
    int32 CPhysicsAsset::FindBodyIndex(const FName& BoneName) const
    {
        for (int32 i = 0; i < Bodies.Num(); ++i)
        {
            if (Bodies[i].BoneName == BoneName)
            {
                return i;
            }
        }
        return Constants::kIndexNone;
    }
}
