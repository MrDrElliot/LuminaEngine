#include "RuntimePCH.h"
#include "RenderLayers.h"

#include "Config/EngineSettings.h"

namespace Lumina
{
    FName SRenderLayerMask::GetLayerName(int32 Layer)
    {
        if (Layer == (int32)RenderLayers::kForegroundBit)
        {
            return FName("Foreground");
        }
        const TVector<FName>& Names = GetDefault<CRenderLayerSettings>()->LayerNames;
        return Layer >= 0 && Layer < Names.Num() ? Names[Layer] : NAME_None;
    }

    int32 SRenderLayerMask::FindLayer(FName Name)
    {
        for (int32 Layer = 0; Name != NAME_None && Layer < (int32)RenderLayers::kCount; ++Layer)
        {
            if (GetLayerName(Layer) == Name)
            {
                return Layer;
            }
        }
        return Constants::kIndexNone;
    }
}
