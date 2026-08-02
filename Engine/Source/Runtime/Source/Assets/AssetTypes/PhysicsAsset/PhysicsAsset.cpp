#include "PhysicsAsset.h"

namespace Lumina
{
    int32 CPhysicsAsset::FindBodyIndex(const FName& BoneName) const
    {
        for (int32 i = 0; i < (int32)Bodies.size(); ++i)
        {
            if (Bodies[i].BoneName == BoneName)
            {
                return i;
            }
        }
        return INDEX_NONE;
    }
}
