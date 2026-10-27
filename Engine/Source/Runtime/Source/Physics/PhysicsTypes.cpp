#include "RuntimePCH.h"
#include "PhysicsTypes.h"

#include "Config/EngineSettings.h"

namespace Lumina
{
    FName SCollisionProfile::GetLayerName(int32 Layer)
    {
        if (Layer == 0)
        {
            return FName("Static");
        }
        if (Layer == 1)
        {
            return FName("Dynamic");
        }
        const TVector<FName>& Names = GetDefault<CCollisionSettings>()->ChannelNames;
        const int32 Channel = Layer - (int32)CollisionLayers::kFirstChannel;
        return Channel >= 0 && Channel < (int32)Names.size() ? Names[Channel] : NAME_None;
    }

    ECollisionProfiles SCollisionProfile::FindLayer(FName Name)
    {
        for (int32 Layer = 0; Name != NAME_None && Layer < (int32)CollisionLayers::kCount; ++Layer)
        {
            if (GetLayerName(Layer) == Name)
            {
                return (ECollisionProfiles)(1u << Layer);
            }
        }
        return (ECollisionProfiles)0;
    }
}
