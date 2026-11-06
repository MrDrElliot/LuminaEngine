#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
    namespace SceneRendererExtractPrimitivesInternal
    {
        float DecalLifetimeFade(const SDecalComponent& Decal, float WorldTime)
        {
            if (Decal.Lifetime <= 0.0f)
            {
                return 1.0f;
            }
            const float SpawnTime = Decal.SpawnTime >= 0.0f ? Decal.SpawnTime : WorldTime;
            const float Remaining = Decal.Lifetime + Decal.FadeOutDuration - (WorldTime - SpawnTime);
            if (Decal.FadeOutDuration <= 0.0f)
            {
                return Remaining > 0.0f ? 1.0f : 0.0f;
            }
            return Math::Clamp(Remaining / Decal.FadeOutDuration, 0.0f, 1.0f);
        }

        FVector4 DecalAtlasTransform(const SDecalComponent& Decal)
        {
            const int32 Columns = Math::Max(Decal.AtlasColumns, 1);
            const int32 Rows    = Math::Max(Decal.AtlasRows, 1);
            const int32 Cell    = Math::Clamp(Decal.AtlasCell, 0, Columns * Rows - 1);
            return FVector4(1.0f / (float)Columns, 1.0f / (float)Rows,
                            (float)(Cell % Columns) / (float)Columns, (float)(Cell / Columns) / (float)Rows);
        }

        uint64 DecalBoxKey(const FTransform& World, const FVector3& Size)
        {
            return HashExtractValue(Size, HashExtractValue(World, 0));
        }
    }

    void FDefaultSceneRenderer::SchedulePrimitiveExtracts(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        auto Transforms   = Registry.GetStorage<STransformComponent>();
        auto TriangleView = Registry.View<FTriangleBatcherComponent>();
        auto WidgetView   = Registry.View<ECS::Write<SWidgetComponent>, ECS::Without<SDisabledTag>>();
        auto TextView     = Registry.View<ECS::Read<STextComponent>, ECS::Without<SDisabledTag>>();
        auto SpriteView   = Registry.View<ECS::Read<SSprite3DComponent>, ECS::Without<SDisabledTag>>();
        auto DecalView    = Registry.View<ECS::Read<SDecalComponent>, ECS::Without<SDisabledTag>>();
        auto WaterView    = Registry.View<ECS::Read<SWaterComponent>, ECS::Without<SDisabledTag>>();

        const bool bTextMayHaveChanged  = PoolMayHaveChanged(Registry, ECS::GetComponentTypeID<STextComponent>(), TextPoolWatch);

        #if USING(WITH_EDITOR)
        auto SelectedView = Registry.View<FSelectedInEditorComponent>();
        #endif

        Jobs::Spawn(Jobs::EJobPriority::Normal, ExtractLatch, [=, this, &Frame]() -> Jobs::TTask<void>
        {
            co_await Jobs::Fork([this, TriangleView]                                    { ExtractBatchedTriangles(TriangleView); });
            co_await Jobs::Fork([this, &Frame, WidgetView, Transforms]                  { ExtractWidgets(Frame, WidgetView, Transforms); });
            co_await Jobs::Fork([this, &Frame, TextView, Transforms, bTextMayHaveChanged] { ExtractText(Frame, TextView, Transforms, bTextMayHaveChanged); });
            co_await Jobs::Fork([this, &Frame, SpriteView, Transforms]                  { ExtractSprites(Frame, SpriteView, Transforms); });
            #if USING(WITH_EDITOR)
            co_await Jobs::Fork([this, &Frame, SelectedView]                            { ExtractSelection(Frame, SelectedView); });
            #endif
            co_await Jobs::Fork([this, &Frame, DecalView, Transforms]                   { ExtractDecals(Frame, DecalView, Transforms); });
            co_await Jobs::Call([this, &Frame, WaterView, Transforms]                   { ExtractWater(Frame, WaterView, Transforms); });
            co_await Jobs::Join();
        });
    }

    void FDefaultSceneRenderer::ExtractBatchedTriangles(const auto& TriangleBatcherView)
    {
        LUMINA_PROFILE_SECTION("Batched Triangle Processing");

        TriangleBatcherView.ForEach([&](FTriangleBatcherComponent& TriangleBatcherComponent)
        {
            ProcessBatchedTriangles(TriangleBatcherComponent);
        });
    }

    void FDefaultSceneRenderer::ExtractWidgets(FFrameData& Frame, const auto& WidgetView, const auto& Transforms)
    {
        LUMINA_PROFILE_SECTION("Process Widget Primitives");

        const FSceneGlobalData& SceneGlobalData = Frame.SceneGlobalData;
        auto& WidgetInstances = Frame.Primitives.WidgetInstances;

        const FFrustum& WidgetFrustum = Frame.CameraFrustum;
        const bool      bCullWidgets   = SceneGlobalData.CullData.bFrustumCull != 0u;

        WidgetView.ForEach([&](ECS::FEntity Entity, ECS::TMut<SWidgetComponent> WidgetComponentMut)
        {
            SWidgetComponent& WidgetComponent = WidgetComponentMut.BypassChangeDetection();
            FWidgetRuntime& Runtime = WidgetComponent.Runtime;

            const FMatrix4 WorldMatrix = Transforms.Get(Entity).GetWorldMatrixCached();
            const FVector3 Center = FVector3(WorldMatrix[3]);
            const float ScaleXY = Math::Max(Math::Length(FVector3(WorldMatrix[0])), Math::Length(FVector3(WorldMatrix[1])));
            const float Radius  = 0.5f * Math::Length(WidgetComponent.WorldSize) * Math::Max(1.0f, ScaleXY);

            Runtime.bVisible = Frame.ViewLayers.Draws(WidgetComponent.RenderLayers)
                            && (!bCullWidgets || WidgetFrustum.IntersectsSphere(Center, Radius));

            if (!Runtime.bVisible || Runtime.ResourceID < 0)
            {
                return;
            }

            FWidgetInstance& Inst = WidgetInstances.EmplaceGetRef();
            Inst.Transform    = WorldMatrix;
            Inst.WorldSize    = WidgetComponent.WorldSize;
            Inst.TextureIndex = (uint32)Runtime.ResourceID;
            Inst.Flags        = WidgetComponent.bBillboard ? WIDGET_FLAG_BILLBOARD : 0u;
            Inst.ColorPack    = PackColor(WidgetComponent.Tint);
            Inst.EntityID     = Entity.Value;
            Inst.Pad0         = 0u;
            Inst.Pad1         = 0u;
        });
    }

    void FDefaultSceneRenderer::ExtractText(FFrameData& Frame, const auto& TextView, const auto& Transforms, bool bTextMayHaveChanged)
    {
        LUMINA_PROFILE_SECTION("Process Text Primitives");

        const FSceneGlobalData& SceneGlobalData = Frame.SceneGlobalData;
        auto& GlyphInstances = Frame.Primitives.GlyphInstances;
        auto& TextBatches    = Frame.Primitives.TextBatches;

        const FFrustum& TextFrustum = Frame.CameraFrustum;
        const bool      bCullText   = SceneGlobalData.CullData.bFrustumCull != 0u;
        const FVector3  CamRight    = FVector3(SceneGlobalData.CameraData.Right);
        const FVector3  CamUp       = FVector3(SceneGlobalData.CameraData.Up);

        uint32 TextVisited = 0;
        TextView.ForEach([&](ECS::FEntity Entity, const STextComponent& TextComponent)
        {
            if (TextComponent.Text.IsEmpty() || !Frame.ViewLayers.Draws(TextComponent.RenderLayers))
            {
                return;
            }

            // Fall back to the engine default font when none is set, or its atlas failed to bake.
            CFont* Font = TextComponent.Font.Get();
            if (Font == nullptr || !Font->HasAtlas())
            {
                Font = CFontManager::Get().GetDefaultFont();
            }
            if (Font == nullptr || !Font->HasAtlas())
            {
                return;
            }

            const int32 AtlasID = Font->GetAtlasResourceID();
            if (AtlasID < 0)
            {
                return;
            }

            const FMatrix4 WorldMatrix = Transforms.Get(Entity).GetWorldMatrixCached();
            const FVector3 Origin = FVector3(WorldMatrix[3]);

            const float HAlign = (TextComponent.HorizontalAlign == ETextHorizontalAlign::Left)   ? 0.0f
                               : (TextComponent.HorizontalAlign == ETextHorizontalAlign::Center) ? 0.5f : 1.0f;
            // Top places the text above the origin, Bottom below (block bottom/top anchored at origin).
            const float VAlign = (TextComponent.VerticalAlign == ETextVerticalAlign::Top)        ? 1.0f
                               : (TextComponent.VerticalAlign == ETextVerticalAlign::Middle)     ? 0.5f : 0.0f;

            FTextRenderCache& Cache = TextCache.FindOrAdd(Entity);
            Cache.SeenSerial = ExtractSerial;
            ++TextVisited;
            // A pool nothing wrote cannot hold a new string, so only a possible write pays for the hash.
            const uint64      TextHash = (bTextMayHaveChanged || !Cache.bValid) ? Hash::GetHash64(TextComponent.Text) : Cache.TextHash;

            const bool bCacheValid =
                   Cache.bValid
                && Cache.Font        == Font
                && Cache.FontVersion == Font->GetShapeVersion()
                && Cache.TextHash    == TextHash
                && Cache.TextLength  == (uint32)TextComponent.Text.Len()
                && Cache.HAlign      == TextComponent.HorizontalAlign
                && Cache.VAlign      == TextComponent.VerticalAlign
                && Cache.LineSpacing == TextComponent.LineSpacing;

            if (!bCacheValid)
            {
                Cache.bValid = false;
                if (!Font->ShapeText(TextComponent.Text, HAlign, VAlign, TextComponent.LineSpacing, Cache.Glyphs))
                {
                    return;
                }

                float EmExtent = 0.0f;
                for (const FShapedGlyph& S : Cache.Glyphs)
                {
                    EmExtent = Math::Max(EmExtent, Math::Max(Math::Abs(S.Min.x), Math::Abs(S.Max.x)));
                    EmExtent = Math::Max(EmExtent, Math::Max(Math::Abs(S.Min.y), Math::Abs(S.Max.y)));
                }

                Cache.EmExtent    = EmExtent;
                Cache.TextHash    = TextHash;
                Cache.TextLength  = (uint32)TextComponent.Text.Len();
                Cache.Font        = Font;
                Cache.FontVersion = Font->GetShapeVersion();
                Cache.HAlign      = TextComponent.HorizontalAlign;
                Cache.VAlign      = TextComponent.VerticalAlign;
                Cache.LineSpacing = TextComponent.LineSpacing;
                Cache.bValid      = true;
            }

            const TVector<FShapedGlyph>& Shaped = Cache.Glyphs;
            if (Shaped.IsEmpty())
            {
                return;
            }

            if (bCullText && !TextFrustum.IntersectsSphere(Origin, Cache.EmExtent * TextComponent.WorldSize * 1.5f))
            {
                return;
            }

            FVector3 RightDir;
            FVector3 UpDir;
            if (TextComponent.bBillboard)
            {
                RightDir = CamRight;
                UpDir    = CamUp;
            }
            else
            {
                RightDir = Math::Normalize(FVector3(WorldMatrix[0]));
                UpDir    = Math::Normalize(FVector3(WorldMatrix[1]));
            }

            const FVector3 RightScaled = RightDir * TextComponent.WorldSize;
            const FVector3 UpScaled    = UpDir    * TextComponent.WorldSize;
            const uint32   Color       = PackColor(TextComponent.Color);
            const uint32   First       = (uint32)GlyphInstances.Num();

            for (const FShapedGlyph& S : Shaped)
            {
                FGPUGlyph& G = GlyphInstances.EmplaceGetRef();
                G.Origin    = Origin;
                G.Intensity = TextComponent.Intensity;
                G.Right     = RightScaled;
                G.Pad1      = 0.0f;
                G.Up        = UpScaled;
                G.Pad2      = 0.0f;
                G.UVRect    = S.UV;
                G.PlaneMin  = S.Min;
                G.PlaneMax  = S.Max;
                G.ColorPack = Color;
                G.EntityID  = Entity.Value;
            }

            const uint32 GlyphCount = (uint32)GlyphInstances.Num() - First;

            // Extending the open batch never reorders anything, and same-font neighbors are common.
            if (!TextBatches.IsEmpty())
            {
                FFrameData::FTextBatch& Last = TextBatches.Last();
                if (Last.AtlasIndex == (uint32)AtlasID
                    && Last.bDepthTest == TextComponent.bDepthTest
                    && Last.FirstInstance + Last.Count == First)
                {
                    Last.Count += GlyphCount;
                    return;
                }
            }

            FFrameData::FTextBatch& Batch = TextBatches.EmplaceGetRef();
            Batch.AtlasIndex    = (uint32)AtlasID;
            Batch.AtlasWidth    = Font->GetAtlasWidth();
            Batch.AtlasHeight   = Font->GetAtlasHeight();
            Batch.DistanceRange = Font->GetDistanceRange();
            Batch.FirstInstance = First;
            Batch.Count         = GlyphCount;
            Batch.bDepthTest    = TextComponent.bDepthTest;
        });

        SweepUnseenExtractCache(TextCache, ExtractSerial, TextVisited);
    }

    void FDefaultSceneRenderer::ExtractSprites(FFrameData& Frame, const auto& SpriteView, const auto& Transforms)
    {
        LUMINA_PROFILE_SECTION("Process Sprite Primitives");

        const FSceneGlobalData& SceneGlobalData = Frame.SceneGlobalData;
        auto& SpriteInstances = Frame.Primitives.SpriteInstances;
        auto& SpriteBatches   = Frame.Primitives.SpriteBatches;

        const FFrustum& SpriteFrustum  = Frame.CameraFrustum;
        const bool      bCullSprites   = SceneGlobalData.CullData.bFrustumCull != 0u;
        const FVector3  SpriteCamRight = FVector3(SceneGlobalData.CameraData.Right);
        const FVector3  SpriteCamUp    = FVector3(SceneGlobalData.CameraData.Up);
        const FVector3  SpriteCamPos   = FVector3(SceneGlobalData.CameraData.Location);

        SpriteSortScratch.Reset();

        SpriteView.ForEach([&](ECS::FEntity Entity, const SSprite3DComponent& Sprite)
        {
            CTexture* Texture = Sprite.Texture.Get();
            if (Texture == nullptr || Texture->GetResourceID() < 0 || Texture->GetNumMips() == 0 || !Frame.ViewLayers.Draws(Sprite.RenderLayers))
            {
                return;
            }

            const FTextureResource::FMip& Base = Texture->GetTextureResource().Mips[0];
            const float TexW = (float)Base.Width;
            const float TexH = (float)Base.Height;
            if (TexW <= 0.0f || TexH <= 0.0f)
            {
                return;
            }

            float U0, V0, U1, V1, FrameW, FrameH;
            if (Sprite.bRegionEnabled)
            {
                FrameW = Sprite.RegionRect.z;
                FrameH = Sprite.RegionRect.w;
                if (FrameW <= 0.0f || FrameH <= 0.0f)
                {
                    return;
                }
                U0 = Sprite.RegionRect.x / TexW;
                V0 = Sprite.RegionRect.y / TexH;
                U1 = (Sprite.RegionRect.x + FrameW) / TexW;
                V1 = (Sprite.RegionRect.y + FrameH) / TexH;
            }
            else
            {
                const int32 HF    = Math::Max(Sprite.HFrames, 1);
                const int32 VF    = Math::Max(Sprite.VFrames, 1);
                const int32 Index = Math::Clamp(Sprite.Frame, 0, HF * VF - 1);
                const int32 Cx    = Index % HF;
                const int32 Cy    = Index / HF;

                FrameW = TexW / (float)HF;
                FrameH = TexH / (float)VF;
                U0 = (float)Cx / (float)HF;
                V0 = (float)Cy / (float)VF;
                U1 = (float)(Cx + 1) / (float)HF;
                V1 = (float)(Cy + 1) / (float)VF;
            }

            if (Sprite.bFlipH)
            {
                const float SwapU = U0; U0 = U1; U1 = SwapU;
            }
            if (Sprite.bFlipV)
            {
                const float SwapV = V0; V0 = V1; V1 = SwapV;
            }

            const FMatrix4 WorldMatrix = Transforms.Get(Entity).GetWorldMatrixCached();
            const FVector3 Origin      = FVector3(WorldMatrix[3]);

            const float ScaleX = Math::Length(FVector3(WorldMatrix[0]));
            const float ScaleY = Math::Length(FVector3(WorldMatrix[1]));
            const float QuadW  = FrameW * Sprite.PixelSize * ScaleX;
            const float QuadH  = FrameH * Sprite.PixelSize * ScaleY;

            // Uncentered anchors the frame's top-left at the origin, so it hangs right and down.
            FVector2 PlaneMin = Sprite.bCentered ? FVector2(-QuadW * 0.5f, -QuadH * 0.5f) : FVector2(0.0f, -QuadH);
            FVector2 PlaneMax = Sprite.bCentered ? FVector2( QuadW * 0.5f,  QuadH * 0.5f) : FVector2(QuadW, 0.0f);

            // Offset is authored in texture pixels with y running down, as the 2D sprite editors do.
            const FVector2 OffsetWorld( Sprite.Offset.x * Sprite.PixelSize * ScaleX,
                                       -Sprite.Offset.y * Sprite.PixelSize * ScaleY);
            PlaneMin += OffsetWorld;
            PlaneMax += OffsetWorld;

            const float Radius = Math::Max(Math::Abs(PlaneMin.x), Math::Abs(PlaneMax.x))
                               + Math::Max(Math::Abs(PlaneMin.y), Math::Abs(PlaneMax.y));
            if (bCullSprites && !SpriteFrustum.IntersectsSphere(Origin, Radius))
            {
                return;
            }

            FVector3 RightDir;
            FVector3 UpDir;
            switch (Sprite.BillboardMode)
            {
            case ESpriteBillboardMode::Enabled:
                RightDir = SpriteCamRight;
                UpDir    = SpriteCamUp;
                break;

            case ESpriteBillboardMode::YBillboard:
            {
                UpDir = FVector3(0.0f, 1.0f, 0.0f);
                FVector3 ToCamera = SpriteCamPos - Origin;
                ToCamera.y = 0.0f;
                // Directly overhead leaves no yaw to resolve, so any stable axis will do.
                RightDir = Math::LengthSquared(ToCamera) > 1e-8f
                         ? Math::Normalize(Math::Cross(UpDir, ToCamera))
                         : SpriteCamRight;
                break;
            }

            default:
                RightDir = Math::Normalize(FVector3(WorldMatrix[0]));
                UpDir    = Math::Normalize(FVector3(WorldMatrix[1]));
                break;
            }

            FSpriteSortEntry& Entry = SpriteSortScratch.EmplaceGetRef();
            Entry.TextureIndex = (uint32)Texture->GetResourceID();
            Entry.SortOrder    = Sprite.SortOrder;
            Entry.ViewDepthSq  = Math::LengthSquared(Origin - SpriteCamPos);
            Entry.bDepthTest   = Sprite.bDepthTest;
            Entry.bDoubleSided = Sprite.bDoubleSided;

            FGPUSprite& Out = Entry.Gpu;
            Out.Origin   = Origin;   Out.Pad0 = 0.0f;
            Out.Right    = RightDir; Out.Pad1 = 0.0f;
            Out.Up       = UpDir;    Out.Pad2 = 0.0f;
            Out.UVRect   = FVector4(U0, V0, U1, V1);
            Out.PlaneMin = PlaneMin;
            Out.PlaneMax = PlaneMax;
            Out.ColorPack = PackColor(Sprite.Modulate);
            Out.EntityID  = Entity.Value;
            Out.Flags     = (Sprite.AlphaCut == ESpriteAlphaCut::Discard) ? SPRITE_FLAG_ALPHA_CUT : 0u;
            Out.AlphaCutThreshold = Sprite.AlphaCutThreshold;
        });

        if (SpriteSortScratch.IsEmpty())
        {
            return;
        }

        Algo::StableSort(SpriteSortScratch, [](const FSpriteSortEntry& A, const FSpriteSortEntry& B)
        {
            if (A.SortOrder != B.SortOrder)
            {
                return A.SortOrder < B.SortOrder;
            }
            return A.ViewDepthSq > B.ViewDepthSq;
        });

        SpriteInstances.Reserve(SpriteSortScratch.Num());
        for (const FSpriteSortEntry& Entry : SpriteSortScratch)
        {
            const uint32 First = (uint32)SpriteInstances.Num();
            SpriteInstances.Add(Entry.Gpu);

            if (!SpriteBatches.IsEmpty())
            {
                FFrameData::FSpriteBatch& Last = SpriteBatches.Last();
                if (Last.TextureIndex == Entry.TextureIndex
                    && Last.bDepthTest == Entry.bDepthTest
                    && Last.bDoubleSided == Entry.bDoubleSided
                    && Last.FirstInstance + Last.Count == First)
                {
                    ++Last.Count;
                    continue;
                }
            }

            FFrameData::FSpriteBatch& Batch = SpriteBatches.EmplaceGetRef();
            Batch.TextureIndex  = Entry.TextureIndex;
            Batch.FirstInstance = First;
            Batch.Count         = 1;
            Batch.bDepthTest    = Entry.bDepthTest;
            Batch.bDoubleSided  = Entry.bDoubleSided;
        }
    }

    // In the draw tasks, since the editor icons reuse what the light pass read instead of walking the lights again.
    void FDefaultSceneRenderer::ScheduleBillboardExtract(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        auto Transforms    = Registry.GetStorage<STransformComponent>();
        auto BillboardView = Registry.View<ECS::Read<SBillboardComponent>, ECS::Without<SDisabledTag>>();

        #if USING(WITH_EDITOR)
        if (bRecordLightIcons)
        {
            auto CameraView        = Registry.View<ECS::Read<SCameraComponent>, ECS::Without<SDisabledTag>, ECS::Without<FEditorComponent>>();
            auto CharacterView     = Registry.View<ECS::Read<SCharacterControllerComponent>, ECS::Without<SDisabledTag>>();
            auto SkyLightView      = Registry.View<ECS::Read<SSkyLightComponent>, ECS::Without<SDisabledTag>>();
            auto ParticleView      = Registry.View<ECS::Read<SParticleSystemComponent>, ECS::Without<SDisabledTag>>();
            auto AudioSourceView   = Registry.View<ECS::Read<SAudioSourceComponent>, ECS::Without<SDisabledTag>>();
            auto AudioListenerView = Registry.View<ECS::Read<SAudioListenerComponent>, ECS::Without<SDisabledTag>>();

            Jobs::Spawn(Jobs::EJobPriority::Normal, ExtractLatch,
                [this, &Frame, Transforms, BillboardView, CameraView, CharacterView, SkyLightView, ParticleView, AudioSourceView, AudioListenerView]() -> Jobs::TTask<void>
            {
                ExtractBillboards(Frame, BillboardView, Transforms);

                // After the component billboards, so the icons keep their place at the end of the list.
                co_await LightsReady;
                ExtractEditorIcons(Frame, Transforms, CameraView, CharacterView, SkyLightView, ParticleView, AudioSourceView, AudioListenerView);
            });
            return;
        }
        #endif

        Jobs::Spawn(Jobs::EJobPriority::Normal, ExtractLatch, [this, &Frame, BillboardView, Transforms]
        {
            ExtractBillboards(Frame, BillboardView, Transforms);
        });
    }

    void FDefaultSceneRenderer::ExtractBillboards(FFrameData& Frame, const auto& BillboardView, const auto& Transforms)
    {
        LUMINA_PROFILE_SECTION("Process Billboard Primitives");

        auto& BillboardInstances = Frame.Primitives.BillboardInstances;
        BillboardView.ForEach([&](ECS::FEntity Entity, const SBillboardComponent& BillboardComponent)
        {
            if (!BillboardComponent.Texture.IsValid() || BillboardComponent.Texture->GetResourceID() < 0)
            {
                return;
            }

            FBillboardInstance& Billboard   = BillboardInstances.EmplaceGetRef();
            Billboard.TextureIndex          = BillboardComponent.Texture->GetResourceID();
            Billboard.Position              = Transforms.Get(Entity).GetWorldLocationCached();
            Billboard.Size                  = BillboardComponent.Scale;
            Billboard.EntityID              = Entity.Value;
        });
    }

#if USING(WITH_EDITOR)
    // Editor visualizer billboards, skipped in game and thumbnail worlds.
    void FDefaultSceneRenderer::ExtractEditorIcons(FFrameData& Frame, const auto& Transforms, const auto& CameraView, const auto& CharacterView,
                                                   const auto& SkyLightView, const auto& ParticleView, const auto& AudioSourceView, const auto& AudioListenerView)
    {
        LUMINA_PROFILE_SECTION("Process Editor Icons");

        auto& BillboardInstances = Frame.Primitives.BillboardInstances;
        auto EmplaceVisualizer = [this, &BillboardInstances](ECS::FEntity Entity, const FVector3& Position, ENamedImage Icon, const FVector4& Color)
        {
            FBillboardInstance& Billboard = BillboardInstances.EmplaceGetRef();
            Billboard.TextureIndex        = (uint32)GetNamedImage(Icon).GetResourceID();
            Billboard.ColorPack           = PackColor(Color);
            Billboard.Position            = Position;
            Billboard.Size                = 0.20f;
            Billboard.EntityID            = Entity.Value;
        };

        auto EmplaceAt = [&](ECS::FEntity Entity, ENamedImage Icon, const FVector4& Color)
        {
            EmplaceVisualizer(Entity, Transforms.Get(Entity).GetWorldLocationCached(), Icon, Color);
        };

        // The light pass recorded each light's icon; its chunks run back to front to match a whole-pool walk.
        auto EmplaceLightIcons = [&](uint32 FirstChunk, uint32 NumChunks, ENamedImage Icon)
        {
            for (uint32 Chunk = FirstChunk + NumChunks; Chunk-- > FirstChunk;)
            {
                const FLightBatch& Batch = LightBatches[Chunk];
                for (uint32 Index = 0; Index < Batch.NumIcons; ++Index)
                {
                    const FLightIcon& LightIcon = Batch.Icons[Index];
                    EmplaceVisualizer(LightIcon.Entity, LightIcon.Position, Icon, FVector4(LightIcon.Color, 1.0f));
                }
            }
        };

        CameraView.ForEach([&](ECS::FEntity Entity, const SCameraComponent&)                            { EmplaceAt(Entity, ENamedImage::CameraIcon, FColor::White); });
        CharacterView.ForEach([&](ECS::FEntity Entity, const SCharacterControllerComponent&)            { EmplaceAt(Entity, ENamedImage::CharacterIcon, FColor::White); });

        EmplaceLightIcons(0u, NumPointLightChunks, ENamedImage::PointLightIcon);
        EmplaceLightIcons(NumPointLightChunks, NumSpotLightChunks, ENamedImage::SpotLightIcon);
        EmplaceLightIcons(NumPointLightChunks + NumSpotLightChunks, NumAreaLightChunks, ENamedImage::SpotLightIcon);
        for (const FLightIcon& LightIcon : DirectionalLightIcons)
        {
            EmplaceVisualizer(LightIcon.Entity, LightIcon.Position, ENamedImage::DirectionalLightIcon, FVector4(LightIcon.Color, 1.0f));
        }

        SkyLightView.ForEach([&](ECS::FEntity Entity, const SSkyLightComponent&)                  { EmplaceAt(Entity, ENamedImage::SkyLightIcon, FVector4(1.0f)); });
        ParticleView.ForEach([&](ECS::FEntity Entity, const SParticleSystemComponent&)            { EmplaceAt(Entity, ENamedImage::ParticleSystemIcon, FVector4(1.0f)); });
        AudioSourceView.ForEach([&](ECS::FEntity Entity, const SAudioSourceComponent& Source)
        {
            // A live voice tints cyan, so an audible emitter is obvious without selecting it.
            EmplaceAt(Entity, ENamedImage::AudioSourceIcon, Source.bPlaying ? FVector4(0.35f, 0.95f, 1.0f, 1.0f) : FVector4(1.0f));
        });
        AudioListenerView.ForEach([&](ECS::FEntity Entity, const SAudioListenerComponent&)        { EmplaceAt(Entity, ENamedImage::AudioListenerIcon, FVector4(1.0f)); });
    }

    void FDefaultSceneRenderer::ExtractSelection(FFrameData& Frame, const auto& SelectedView)
    {
        LUMINA_PROFILE_SECTION("Extract Selection");

        TVector<uint32>& Bits = Frame.Extracts.SelectionBits;
        Bits.Reset();

        // Sized to the highest selected slot, since an entity above the top bit reads as unselected.
        uint32 HighestWord = 0u;
        bool   bAnySelected = false;
        SelectedView.ForEach([&](ECS::FEntity Entity)
        {
            HighestWord  = Math::Max(HighestWord, Entity.GetIndex() >> 5u);
            bAnySelected = true;
        });

        // Sized once, since the view yields entities in no particular order.
        if (bAnySelected)
        {
            Bits.SetNum(HighestWord + 1u, 0u);
            SelectedView.ForEach([&](ECS::FEntity Entity)
            {
                const uint32 Index = Entity.GetIndex();
                Bits[Index >> 5u] |= (1u << (Index & 31u));
            });
        }
    }
#endif

    void FDefaultSceneRenderer::ExtractDebugText(FFrameData& Frame)
    {
        Frame.Primitives.DebugTextGlyphs.Reset();
        Frame.Primitives.DebugTextBatch = {};
#if !defined(LE_SHIPPING)
        {
            TVector<FDebugTextLine> DebugLines;
            World->DrainDebugTextLines(DebugLines);

            CFont* DebugFont = CFontManager::Get().GetDefaultFont();
            const int32 DebugAtlasID = DebugFont ? DebugFont->GetAtlasResourceID() : -1;
            if (!DebugLines.IsEmpty() && DebugFont && DebugFont->HasAtlas() && DebugAtlasID >= 0)
            {
                // Pixels per em.
                const float PxSize = 32.0f;
                const float Margin = 12.0f;
                float       PenY   = Margin;

                TVector<FShapedGlyph> DebugShaped;
                for (const FDebugTextLine& Line : DebugLines)
                {
                    const uint32 Color = PackColor(Line.Color);
                    if (DebugFont->ShapeText(Line.Text, 0.0f, 0.0f, 1.0f, DebugShaped))
                    {
                        for (const FShapedGlyph& S : DebugShaped)
                        {
                            FGPUGlyph& G = Frame.Primitives.DebugTextGlyphs.EmplaceGetRef();
                            G.PlaneMin  = FVector2(Margin + S.Min.x * PxSize, PenY - S.Max.y * PxSize);
                            G.PlaneMax  = FVector2(Margin + S.Max.x * PxSize, PenY - S.Min.y * PxSize);
                            G.UVRect    = S.UV;
                            G.ColorPack = Color;
                        }
                    }

                    int32 NumLines = 1;
                    for (const char C : Line.Text)
                    {
                        if (C == '\n') ++NumLines;
                    }
                    PenY += (float)NumLines * DebugFont->GetLineHeight() * PxSize;
                }

                if (!Frame.Primitives.DebugTextGlyphs.IsEmpty())
                {
                    FFrameData::FTextBatch& Batch = Frame.Primitives.DebugTextBatch;
                    Batch.AtlasIndex    = (uint32)DebugAtlasID;
                    Batch.AtlasWidth    = DebugFont->GetAtlasWidth();
                    Batch.AtlasHeight   = DebugFont->GetAtlasHeight();
                    Batch.DistanceRange = DebugFont->GetDistanceRange();
                    Batch.FirstInstance = 0;
                    Batch.Count         = (uint32)Frame.Primitives.DebugTextGlyphs.Num();
                }
            }
        }
#endif
    }

    FDefaultSceneRenderer::FDecalMaterialState FDefaultSceneRenderer::FindOrResolveDecalMaterial(CMaterialInterface* Material)
    {
        if (Material == nullptr)
        {
            return {};
        }
        if (const int32* Found = DecalMaterialLookup.Find(Material))
        {
            return DecalMaterialStates[*Found];
        }

        FDecalMaterialState State;
        FShaderH DecalVS;
        FShaderH DecalPS;
        // Textures load on demand, and a decal drawn on the placeholder would stamp the missing-texture color onto the world.
        if (Material->ResolveDomainShaders(EMaterialType::Decal, DecalVS, DecalPS) && Material->RequestTexturesResolved())
        {
            State.ShaderOwner   = Material->GetMaterial();
            State.MaterialIndex = Material->GetMaterialIndex();
            State.bUsable       = State.MaterialIndex >= 0;
        }

        DecalMaterialLookup.Add(Material, DecalMaterialStates.Add(State));
        return State;
    }

    void FDefaultSceneRenderer::ExtractDecals(FFrameData& Frame, const auto& DecalView, const auto& Transforms)
    {
        using namespace SceneRendererExtractPrimitivesInternal;

        LUMINA_PROFILE_SECTION("Extract Decals");

        Frame.Primitives.DecalExtracts.Reset();
        Frame.Primitives.DecalBatches.Reset();
        DecalSortScratch.Reset();
        DecalMaterialStates.Reset();
        DecalMaterialLookup.Reset();

        const float WorldTime = (float)World->GetTimeSinceWorldCreation();

        uint32 Visited = 0;
        DecalView.ForEach([&](ECS::FEntity Entity, const SDecalComponent& Decal)
        {
            ++Visited;
            FDecalExtractCache& Cache = DecalCache.FindOrAdd(Entity);
            Cache.SeenSerial = ExtractSerial;

            if (!Frame.ViewLayers.Draws(Decal.RenderLayers))
            {
                return;
            }

            const float LifetimeFade = DecalLifetimeFade(Decal, WorldTime);
            if (LifetimeFade <= 0.0f)
            {
                return;
            }

            const FDecalMaterialState Material = FindOrResolveDecalMaterial(Decal.DecalMaterial.Get());
            if (!Material.bUsable)
            {
                return;
            }

            // Transforms carry no change ticks, so the cheap key is checked every frame and only the inverse is cached.
            const FTransform& WorldTransform = Transforms.Get(Entity).GetWorldTransformCached();
            const uint64      Key            = DecalBoxKey(WorldTransform, Decal.Size);
            if (!Cache.bValid || Cache.Key != Key)
            {
                Cache.DecalToWorld = Math::Scale(WorldTransform.GetMatrix(), Decal.Size);
                Cache.WorldToDecal = Math::Inverse(Cache.DecalToWorld);
                Cache.Key          = Key;
                Cache.bValid       = true;
            }

            FGPUDecal Item;
            Item.DecalToWorld   = Cache.DecalToWorld;
            Item.WorldToDecal   = Cache.WorldToDecal;
            Item.FadeAngleCos   = Math::Cos(Math::Radians(Math::Clamp(Decal.FadeAngle, 0.0f, 89.9f)));
            Item.Opacity        = Math::Clamp(Decal.Opacity * Decal.Modulate.w, 0.0f, 1.0f) * LifetimeFade;
            Item.MaterialIndex  = (uint32)Material.MaterialIndex;
            Item.EmissionEnergy = Math::Max(Decal.EmissionEnergy, 0.0f);
            Item.UVTransform    = DecalAtlasTransform(Decal);
            Item.Modulate       = FVector4(Decal.Modulate.x, Decal.Modulate.y, Decal.Modulate.z, Math::Clamp(Decal.AlbedoMix, 0.0f, 1.0f));
            Item.Fades          = FVector4(Decal.bDistanceFade ? Math::Max(Decal.DistanceFadeBegin, 0.0f) : -1.0f,
                                           Math::Max(Decal.DistanceFadeLength, 0.0f),
                                           Math::Clamp(Decal.UpperFade, 0.0f, 1.0f), Math::Clamp(Decal.LowerFade, 0.0f, 1.0f));

            DecalSortScratch.Emplace(Material.ShaderOwner, Decal.SortOrder, Item);
        });

        SweepUnseenExtractCache(DecalCache, ExtractSerial, Visited);

        // Sort order decides first, so materials interleave exactly, and runs of one material within an order still batch.
        Algo::StableSort(DecalSortScratch, [](const FDecalSortEntry& A, const FDecalSortEntry& B)
        {
            if (A.SortOrder != B.SortOrder)
            {
                return A.SortOrder < B.SortOrder;
            }
            return A.ShaderOwner < B.ShaderOwner;
        });

        Frame.Primitives.DecalExtracts.Reserve(DecalSortScratch.Num());
        CMaterial* PrevOwner = nullptr;
        for (int32 i = 0; i < DecalSortScratch.Num(); ++i)
        {
            Frame.Primitives.DecalExtracts.Add(DecalSortScratch[i].Gpu);

            CMaterial* Owner = DecalSortScratch[i].ShaderOwner;
            if (Owner == PrevOwner && !Frame.Primitives.DecalBatches.IsEmpty())
            {
                Frame.Primitives.DecalBatches.Last().Count++;
            }
            else
            {
                FFrameData::FDecalBatch& Batch = Frame.Primitives.DecalBatches.EmplaceGetRef();
                Batch.Shaders.VertexShader = Owner->GetVertexShader();
                Batch.Shaders.PixelShader  = Owner->GetPixelShader();
                Batch.FirstInstance        = i;
                Batch.Count                = 1u;
                PrevOwner                  = Owner;
            }
        }
    }

    void FDefaultSceneRenderer::ExtractWater(FFrameData& Frame, const auto& WaterView, const auto& Transforms)
    {
        LUMINA_PROFILE_SECTION("Extract Water");

        Frame.Water.Surfaces.Reset();
        Frame.Water.bUnderwaterActive = false;

        const FVector4& CamLoc = Frame.SceneGlobalData.CameraData.Location;
        const FVector3  CameraPos = FVector3(CamLoc.x, CamLoc.y, CamLoc.z);

        // Track the nearest water surface above the camera (largest local.y still below the plane).
        float BestUnderwaterLocalY = -1.0e30f;

        auto ResolveTexture = [](const TStrongObjectPtr<CTexture>& Tex) -> uint32
        {
            const CTexture* T = Tex.Get();
            const int32 ID = T ? T->GetResourceID() : -1;
            return ID >= 0 ? (uint32)ID : Constants::kIndexNoneU32;
        };

        WaterView.ForEach([&](ECS::FEntity Entity, const SWaterComponent& Water)
        {
            const STransformComponent& Transform = Transforms.Get(Entity);

            FGPUWater Item    = {};
            Item.WaterToWorld = WaterToWorld(Water, Transform.GetWorldMatrixCached());
            Item.WorldToWater = Math::Inverse(Item.WaterToWorld);

            Item.ShallowColor = FVector4(Water.ShallowColor, 1.0f);
            Item.DeepColor    = FVector4(Water.DeepColor, 1.0f);
            Item.FoamColor    = FVector4(Water.FoamColor, 1.0f);

            FVector2    Wind    = Water.WindDirection;
            const float WindLen = Math::Sqrt(Wind.x * Wind.x + Wind.y * Wind.y);
            Wind = (WindLen > 1e-4f) ? FVector2(Wind.x / WindLen, Wind.y / WindLen) : FVector2(1.0f, 0.0f);

            Item.WindAndWave    = FVector4(Wind.x, Wind.y, Water.WindSpeed, Water.WaveAmplitude);
            Item.WaveParams     = FVector4(Math::Clamp(Water.Choppiness, 0.0f, 1.0f),
                                           Math::Max(Water.WaveLength, 0.5f),
                                           (float)Math::Clamp(Water.WaveCount, 1, 8),
                                           Math::Clamp(Water.DetailStrength, 0.0f, 1.0f));
            Item.RefractReflect = FVector4(Math::Max(Water.RefractionStrength, 0.0f),
                                           Math::Clamp(Water.ReflectionStrength, 0.0f, 1.0f),
                                           Math::Clamp(Water.Roughness, 0.0f, 1.0f),
                                           Math::Max(Water.FresnelPower, 1.0f));
            Item.FoamAbsorb     = FVector4(Math::Max(Water.ShorelineFoamWidth, 0.0f),
                                           Math::Clamp(Water.CrestFoamAmount, 0.0f, 1.0f),
                                           Math::Max(Water.DepthFadeDistance, 0.01f),
                                           Math::Max(Water.AbsorptionScale, 0.0f));
            Item.SSRSpecOpacity = FVector4(Math::Max(Water.SSRMaxDistance, 1.0f),
                                           (float)Math::Clamp(Water.SSRStepCount, 8, 128),
                                           Math::Max(Water.SpecularIntensity, 0.0f),
                                           Math::Clamp(Water.Opacity, 0.0f, 1.0f));
            Item.DetailParams   = FVector4(Math::Max(Water.DetailTileSize, 0.01f),
                                           Math::Max(Water.DetailScrollSpeed, 0.0f),
                                           Math::Max(Water.FoamTiling, 0.01f),
                                           Math::Max(Water.FoamIntensity, 0.0f));

            Item.InteractionParams = FVector4(Water.bInteractive ? Math::Max(Water.InteractionRippleScale, 0.0f) : 0.0f,
                                              Water.bInteractive ? Math::Max(Water.InteractionFoamScale, 0.0f) : 0.0f,
                                              Math::Max(Water.RainRippleScale, 0.0f), 0.0f);
            Item.DetailNormalIndex = ResolveTexture(Water.DetailNormalMap);
            Item.FoamTextureIndex  = ResolveTexture(Water.FoamTexture);
            Item.GridResolution    = (uint32)Math::Clamp(Water.GridResolution, 2, 512);
            const float BodyWidth  = Math::Max(Math::Max(Water.Extent.x, Water.Extent.y), 1.0f);
            Item.HorizonScale      = Math::Max(Water.HorizonExtent / BodyWidth, 1.0f);

            Frame.Water.Surfaces.Add(Item);
            const FVector4 LocalCam = Item.WorldToWater * FVector4(CameraPos, 1.0f);
            if (Math::Abs(LocalCam.x) <= 0.5f && Math::Abs(LocalCam.z) <= 0.5f &&
                LocalCam.y < 0.0f && LocalCam.y > BestUnderwaterLocalY)
            {
                BestUnderwaterLocalY = LocalCam.y;

                const FVector3 SurfaceCenter = Transform.GetWorldLocationCached();
                Frame.Water.bUnderwaterActive = true;
                Frame.Water.Underwater.PlaneNormalAndHeight = FVector4(0.0f, 1.0f, 0.0f, SurfaceCenter.y);
                Frame.Water.Underwater.FogColorDensity      = FVector4(Water.UnderwaterFogColor, Math::Max(Water.UnderwaterFogDensity, 0.0f));
                Frame.Water.Underwater.TintDistortion       = FVector4(Water.UnderwaterTint, Math::Max(Water.UnderwaterDistortion, 0.0f));
                Frame.Water.Underwater.DeepColor            = FVector4(Water.DeepColor, 0.0f);
            }
        });
    }
}
