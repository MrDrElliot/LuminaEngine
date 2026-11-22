#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
    void FDefaultSceneRenderer::UpdateSunShadowCache(FFrameData& Frame)
    {
        LUMINA_PROFILE_SCOPE();

        const FFrameData::FGeometry::FRetainedUpload& Upload = Frame.Geometry.RetainedUpload;
        const TVector<uint32>& StreamedBlocks = ScenePrimitives.GetStreamedBlocks();

        FSunShadowCache::FSlotChanges Changes;
        Changes.Dirty          = TSpan<const uint32>(Upload.DirtySlots.GetData(), Upload.DirtySlots.Num());
        Changes.DirtyStatic    = TSpan<const uint32>(Upload.DirtyStaticSlots.GetData(), Upload.DirtyStaticSlots.Num());
        Changes.Deactivated    = TSpan<const uint32>(Upload.DeactivatedSlots.GetData(), Upload.DeactivatedSlots.Num());
        Changes.StreamedBlocks = TSpan<const uint32>(StreamedBlocks.GetData(), StreamedBlocks.Num());
        Changes.bFull          = Upload.bFull || Upload.bFullStatic;

        auto& Virtual = Frame.Lighting.VirtualShadow;
        SunShadowCache.Update(Changes, ScenePrimitives, Virtual.bActive, Frame.ViewVolume.GetViewPosition());

        auto& Cache = Frame.Lighting.ShadowCache;
        if (SunShadowCache.IsActive())
        {
            Cache.SlotBits = SunShadowCache.GetSlotBits();
        }
        else
        {
            Cache.SlotBits.Reset();
        }

        // The pages under what changed draw again, and a rebuilt cache redraws every page.
        if (Virtual.bActive)
        {
            bool bAll = false;
            SunShadowCache.TakeTouches(Virtual.InvalidSpheres, bAll);
            // Impostor points are not retained slots, so their streamed tiles clear their own pages.
            ScenePrimitives.GetImpostorPoints().TakeTouches(Virtual.InvalidSpheres);
            // The Shadow Raster view shows how every caster rasterizes, so it redraws each cached page rather than only what moved.
            bAll = bAll || Frame.SceneGlobalData.CullData.DebugMode == (uint32)ERenderSceneDebugFlags::ShadowRaster;
            Virtual.bInvalidateAll = bAll;
            LUMINA_PROFILE_VALUE("Shadows/VirtualInvalidSpheres", (int64)Virtual.InvalidSpheres.Num());
            LUMINA_PROFILE_VALUE("Shadows/VirtualInvalidAll", bAll ? 1 : 0);
            LUMINA_PROFILE_VALUE("Shadows/CacheHeldSlots", (int64)SunShadowCache.GetHeldCount());
            if (bAll)
            {
                Virtual.InvalidSpheres.Reset();
                for (FVirtualShadowLevelGPU& Level : Virtual.Levels)
                {
                    Level.bInvalidate = 1u;
                }
            }
        }
        else
        {
            // Without virtual pages there is nothing to clear, and the spheres would only pile up.
            ImpostorTouchScratch.Reset();
            ScenePrimitives.GetImpostorPoints().TakeTouches(ImpostorTouchScratch);
        }
    }
}
