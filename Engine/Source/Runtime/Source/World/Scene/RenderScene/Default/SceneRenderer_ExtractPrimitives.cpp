#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
    void FDefaultSceneRenderer::ExtractBatchedTriangles(ECS::FRegistry& Registry)
    {
        LUMINA_PROFILE_SECTION("Batched Triangle Processing");

        auto TriangleBatcherView = Registry.View<FTriangleBatcherComponent>();

        TriangleBatcherView.ForEach([&](FTriangleBatcherComponent& TriangleBatcherComponent)
        {
            ProcessBatchedTriangles(TriangleBatcherComponent);
        });
    }

    void FDefaultSceneRenderer::ExtractWidgets(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        LUMINA_PROFILE_SECTION("Process Widget Primitives");

        const FSceneGlobalData& SceneGlobalData = Frame.SceneGlobalData;
        auto& WidgetInstances = Frame.Primitives.WidgetInstances;
        auto TransformStorage = Registry.GetStorage<STransformComponent>();
        auto WidgetView = Registry.View<SWidgetComponent>(ECS::TExclude<SDisabledTag>{});

        const FFrustum& WidgetFrustum = Frame.CameraFrustum;
        const bool      bCullWidgets   = SceneGlobalData.CullData.bFrustumCull != 0u;

        WidgetView.ForEach([&](ECS::FEntity Entity, SWidgetComponent& WidgetComponent)
        {
            FWidgetRuntime& Runtime = WidgetComponent.Runtime;

            const FMatrix4 WorldMatrix = TransformStorage.Get(Entity).GetWorldMatrix();
            const FVector3 Center = FVector3(WorldMatrix[3]);
            const float ScaleXY = Math::Max(Math::Length(FVector3(WorldMatrix[0])), Math::Length(FVector3(WorldMatrix[1])));
            const float Radius  = 0.5f * Math::Length(WidgetComponent.WorldSize) * Math::Max(1.0f, ScaleXY);

            Runtime.bVisible = !bCullWidgets || WidgetFrustum.IntersectsSphere(Center, Radius);

            if (!Runtime.bVisible || Runtime.ResourceID < 0)
            {
                return;
            }

            FWidgetInstance& Inst = WidgetInstances.emplace_back();
            Inst.Transform    = WorldMatrix;
            Inst.WorldSize    = WidgetComponent.WorldSize;
            Inst.TextureIndex = (uint32)Runtime.ResourceID;
            Inst.Flags        = WidgetComponent.bBillboard ? WIDGET_FLAG_BILLBOARD : 0u;
            Inst.ColorPack    = PackColor(WidgetComponent.Tint);
            Inst.EntityID     = (Entity).Value;
            Inst.Pad0         = 0u;
            Inst.Pad1         = 0u;
        });
    }

    void FDefaultSceneRenderer::ExtractText(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        LUMINA_PROFILE_SECTION("Process Text Primitives");

        const FSceneGlobalData& SceneGlobalData = Frame.SceneGlobalData;
        auto& GlyphInstances = Frame.Primitives.GlyphInstances;
        auto& TextBatches    = Frame.Primitives.TextBatches;
        auto TransformStorage = Registry.GetStorage<STransformComponent>();
        auto TextView = Registry.View<STextComponent>(ECS::TExclude<SDisabledTag>{});

        const FFrustum& TextFrustum = Frame.CameraFrustum;
        const bool      bCullText   = SceneGlobalData.CullData.bFrustumCull != 0u;
        const FVector3  CamRight    = FVector3(SceneGlobalData.CameraData.Right);
        const FVector3  CamUp       = FVector3(SceneGlobalData.CameraData.Up);

        TextView.ForEach([&](ECS::FEntity Entity, STextComponent& TextComponent)
        {
            if (TextComponent.Text.empty())
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

            const FMatrix4 WorldMatrix = TransformStorage.Get(Entity).GetWorldMatrix();
            const FVector3 Origin = FVector3(WorldMatrix[3]);

            const float HAlign = (TextComponent.HorizontalAlign == ETextHorizontalAlign::Left)   ? 0.0f
                               : (TextComponent.HorizontalAlign == ETextHorizontalAlign::Center) ? 0.5f : 1.0f;
            // Top places the text above the origin, Bottom below (block bottom/top anchored at origin).
            const float VAlign = (TextComponent.VerticalAlign == ETextVerticalAlign::Top)        ? 1.0f
                               : (TextComponent.VerticalAlign == ETextVerticalAlign::Middle)     ? 0.5f : 0.0f;

            FTextRenderCache& Cache = TextComponent.RenderCache;
            const uint64      TextHash = Hash::GetHash64(TextComponent.Text);

            const bool bCacheValid =
                   Cache.bValid
                && Cache.Font        == Font
                && Cache.FontVersion == Font->GetShapeVersion()
                && Cache.TextHash    == TextHash
                && Cache.TextLength  == (uint32)TextComponent.Text.size()
                && Cache.HAlign      == TextComponent.HorizontalAlign
                && Cache.VAlign      == TextComponent.VerticalAlign
                && Cache.LineSpacing == TextComponent.LineSpacing;

            if (!bCacheValid)
            {
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
                Cache.TextLength  = (uint32)TextComponent.Text.size();
                Cache.Font        = Font;
                Cache.FontVersion = Font->GetShapeVersion();
                Cache.HAlign      = TextComponent.HorizontalAlign;
                Cache.VAlign      = TextComponent.VerticalAlign;
                Cache.LineSpacing = TextComponent.LineSpacing;
                Cache.bValid      = true;
            }

            const TVector<FShapedGlyph>& Shaped = Cache.Glyphs;
            if (Shaped.empty())
            {
                return;
            }

            if (bCullText && !TextFrustum.IntersectsSphere(Origin, Cache.EmExtent * TextComponent.WorldSize * 1.5f))
            {
                return;
            }

            FVector3 RightDir, UpDir;
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
            const uint32   First       = (uint32)GlyphInstances.size();

            for (const FShapedGlyph& S : Shaped)
            {
                FGPUGlyph& G = GlyphInstances.emplace_back();
                G.Origin    = Origin;
                G.Pad0      = 0.0f;
                G.Right     = RightScaled;
                G.Pad1      = 0.0f;
                G.Up        = UpScaled;
                G.Pad2      = 0.0f;
                G.UVRect    = S.UV;
                G.PlaneMin  = S.Min;
                G.PlaneMax  = S.Max;
                G.ColorPack = Color;
                G.EntityID  = (Entity).Value;
            }

            const uint32 GlyphCount = (uint32)GlyphInstances.size() - First;

            // Extending the open batch never reorders anything, and same-font neighbors are common.
            if (!TextBatches.empty())
            {
                FFrameData::FTextBatch& Last = TextBatches.back();
                if (Last.AtlasIndex == (uint32)AtlasID
                    && Last.bDepthTest == TextComponent.bDepthTest
                    && Last.FirstInstance + Last.Count == First)
                {
                    Last.Count += GlyphCount;
                    return;
                }
            }

            FFrameData::FTextBatch& Batch = TextBatches.emplace_back();
            Batch.AtlasIndex    = (uint32)AtlasID;
            Batch.AtlasWidth    = Font->GetAtlasWidth();
            Batch.AtlasHeight   = Font->GetAtlasHeight();
            Batch.DistanceRange = Font->GetDistanceRange();
            Batch.FirstInstance = First;
            Batch.Count         = GlyphCount;
            Batch.bDepthTest    = TextComponent.bDepthTest;
        });
    }

    void FDefaultSceneRenderer::ExtractSprites(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        LUMINA_PROFILE_SECTION("Process Sprite Primitives");

        const FSceneGlobalData& SceneGlobalData = Frame.SceneGlobalData;
        auto& SpriteInstances = Frame.Primitives.SpriteInstances;
        auto& SpriteBatches   = Frame.Primitives.SpriteBatches;
        auto TransformStorage = Registry.GetStorage<STransformComponent>();
        auto SpriteView = Registry.View<SSprite3DComponent>(ECS::TExclude<SDisabledTag>{});

        const FFrustum& SpriteFrustum  = Frame.CameraFrustum;
        const bool      bCullSprites   = SceneGlobalData.CullData.bFrustumCull != 0u;
        const FVector3  SpriteCamRight = FVector3(SceneGlobalData.CameraData.Right);
        const FVector3  SpriteCamUp    = FVector3(SceneGlobalData.CameraData.Up);
        const FVector3  SpriteCamPos   = FVector3(SceneGlobalData.CameraData.Location);

        SpriteSortScratch.clear();

        SpriteView.ForEach([&](ECS::FEntity Entity, const SSprite3DComponent& Sprite)
        {
            CTexture* Texture = Sprite.Texture.Get();
            if (Texture == nullptr || Texture->GetResourceID() < 0 || Texture->GetNumMips() == 0)
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

            const FMatrix4 WorldMatrix = TransformStorage.Get(Entity).GetWorldMatrix();
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

            FSpriteSortEntry& Entry = SpriteSortScratch.emplace_back();
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
            Out.EntityID  = (Entity).Value;
            Out.Flags     = (Sprite.AlphaCut == ESpriteAlphaCut::Discard) ? SPRITE_FLAG_ALPHA_CUT : 0u;
            Out.AlphaCutThreshold = Sprite.AlphaCutThreshold;
        });

        if (SpriteSortScratch.empty())
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

        SpriteInstances.reserve(SpriteSortScratch.size());
        for (const FSpriteSortEntry& Entry : SpriteSortScratch)
        {
            const uint32 First = (uint32)SpriteInstances.size();
            SpriteInstances.push_back(Entry.Gpu);

            if (!SpriteBatches.empty())
            {
                FFrameData::FSpriteBatch& Last = SpriteBatches.back();
                if (Last.TextureIndex == Entry.TextureIndex
                    && Last.bDepthTest == Entry.bDepthTest
                    && Last.bDoubleSided == Entry.bDoubleSided
                    && Last.FirstInstance + Last.Count == First)
                {
                    ++Last.Count;
                    continue;
                }
            }

            FFrameData::FSpriteBatch& Batch = SpriteBatches.emplace_back();
            Batch.TextureIndex  = Entry.TextureIndex;
            Batch.FirstInstance = First;
            Batch.Count         = 1;
            Batch.bDepthTest    = Entry.bDepthTest;
            Batch.bDoubleSided  = Entry.bDoubleSided;
        }
    }

    void FDefaultSceneRenderer::ExtractBillboards(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        LUMINA_PROFILE_SECTION("Process Billboard Primitives");

        auto& BillboardInstances = Frame.Primitives.BillboardInstances;
        auto TransformStorage = Registry.GetStorage<STransformComponent>();
        auto BillboardView = Registry.View<SBillboardComponent>(ECS::TExclude<SDisabledTag>{});

        BillboardView.ForEach([this, &BillboardInstances, &TransformStorage](ECS::FEntity Entity, const SBillboardComponent& BillboardComponent)
        {
            if (!BillboardComponent.Texture.IsValid() || BillboardComponent.Texture->GetResourceID() < 0)
            {
                return;
            }

            FBillboardInstance& Billboard   = BillboardInstances.emplace_back();
            Billboard.TextureIndex          = BillboardComponent.Texture->GetResourceID();
            Billboard.Position              = TransformStorage.Get(Entity).GetWorldLocationCached();
            Billboard.Size                  = BillboardComponent.Scale;
            Billboard.EntityID              = (Entity).Value;
        });
        
        #if USING(WITH_EDITOR)
        auto CameraView = Registry.View<SCameraComponent>(ECS::TExclude<SDisabledTag>{});
        auto CharacterView = Registry.View<SCharacterControllerComponent>(ECS::TExclude<SDisabledTag>{});
        auto PointLightView = Registry.View<SPointLightComponent>(ECS::TExclude<SDisabledTag>{});
        auto SpotLightView = Registry.View<SSpotLightComponent>(ECS::TExclude<SDisabledTag>{});
        auto DirectionalView = Registry.View<SDirectionalLightComponent>(ECS::TExclude<SDisabledTag>{});
        auto SkyLightView = Registry.View<SSkyLightComponent>(ECS::TExclude<SDisabledTag>{});
        auto ParticleView = Registry.View<SParticleSystemComponent>(ECS::TExclude<SDisabledTag>{});
        auto AudioSourceView = Registry.View<SAudioSourceComponent>(ECS::TExclude<SDisabledTag>{});
        auto AudioListenerView = Registry.View<SAudioListenerComponent>(ECS::TExclude<SDisabledTag>{});
        // Editor visualizer billboards, skipped in game and thumbnail worlds.
        if (!World->IsGameWorld())
        {
            auto EmplaceVisualizer = [this, &BillboardInstances](ECS::FEntity Entity, const FVector3& Position, ENamedImage Icon, const FVector4& Color, float Size = 0.20f)
            {
                FBillboardInstance& Billboard = BillboardInstances.emplace_back();
                Billboard.TextureIndex        = (uint32)GetNamedImage(Icon).GetResourceID();
                Billboard.ColorPack           = PackColor(Color);
                Billboard.Position            = Position;
                Billboard.Size                = Size;
                Billboard.EntityID            = (Entity).Value;
            };

            // Skip editor viewport camera so the billboard doesn't sit on the user's view.
            CameraView.ForEach([&](ECS::FEntity Entity, SCameraComponent&)
            {
                if (Registry.HasAll<FEditorComponent>(Entity))
                {
                    return;
                }
                EmplaceVisualizer(Entity, TransformStorage.Get(Entity).GetWorldLocationCached(), ENamedImage::CameraIcon, FColor::White);
            });

            CharacterView.ForEach([&](ECS::FEntity Entity, SCharacterControllerComponent&)
            {
                EmplaceVisualizer(Entity, TransformStorage.Get(Entity).GetWorldLocationCached(), ENamedImage::CharacterIcon, FColor::White);
            });

            PointLightView.ForEach([&](ECS::FEntity Entity, const SPointLightComponent& Light)
            {
                EmplaceVisualizer(Entity, TransformStorage.Get(Entity).GetWorldLocationCached(), ENamedImage::PointLightIcon, FVector4(Light.LightColor, 1.0f));
            });

            SpotLightView.ForEach([&](ECS::FEntity Entity, const SSpotLightComponent& Light)
            {
                EmplaceVisualizer(Entity, TransformStorage.Get(Entity).GetWorldLocationCached(), ENamedImage::SpotLightIcon, FVector4(Light.LightColor, 1.0f));
            });

            DirectionalView.ForEach([&](ECS::FEntity Entity, const SDirectionalLightComponent& Light)
            {
                const auto& Transform = Registry.Get<STransformComponent>(Entity);
                EmplaceVisualizer(Entity, Transform.GetWorldLocationCached(), ENamedImage::DirectionalLightIcon, FVector4(Light.Color, 1.0f));
            });

            SkyLightView.ForEach([&](ECS::FEntity Entity, const SSkyLightComponent&)
            {
                const auto& Transform = Registry.Get<STransformComponent>(Entity);
                EmplaceVisualizer(Entity, Transform.GetWorldLocationCached(), ENamedImage::SkyLightIcon, FVector4(1.0f));
            });

            ParticleView.ForEach([&](ECS::FEntity Entity, const SParticleSystemComponent&)
            {
                EmplaceVisualizer(Entity, TransformStorage.Get(Entity).GetWorldLocationCached(), ENamedImage::ParticleSystemIcon, FVector4(1.0f));
            });

            AudioSourceView.ForEach([&](ECS::FEntity Entity, const SAudioSourceComponent& Source)
            {
                // A live voice tints cyan, so an audible emitter is obvious without selecting it.
                const FVector4 Tint = Source.bPlaying ? FVector4(0.35f, 0.95f, 1.0f, 1.0f) : FVector4(1.0f);
                EmplaceVisualizer(Entity, TransformStorage.Get(Entity).GetWorldLocationCached(), ENamedImage::AudioSourceIcon, Tint);
            });

            AudioListenerView.ForEach([&](ECS::FEntity Entity, const SAudioListenerComponent&)
            {
                EmplaceVisualizer(Entity, TransformStorage.Get(Entity).GetWorldLocationCached(), ENamedImage::AudioListenerIcon, FVector4(1.0f));
            });
        }
        #endif
    }

#if USING(WITH_EDITOR)
    void FDefaultSceneRenderer::ExtractSelection(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        LUMINA_PROFILE_SECTION("Extract Selection");

        TVector<uint32>& Bits = Frame.Extracts.SelectionBits;
        Bits.clear();

        // Sized to the highest selected slot, since an entity above the top bit reads as unselected.
        uint32 HighestWord = 0u;
        bool   bAnySelected = false;
        auto   Selected = Registry.View<FSelectedInEditorComponent>();
        Selected.ForEach([&](ECS::FEntity Entity)
        {
            HighestWord  = Math::Max(HighestWord, (uint32)(Entity).GetIndex() >> 5u);
            bAnySelected = true;
        });

        // Sized once, since the view yields entities in no particular order.
        if (bAnySelected)
        {
            Bits.resize(HighestWord + 1u, 0u);
            Selected.ForEach([&](ECS::FEntity Entity)
            {
                const uint32 Index = (uint32)(Entity).GetIndex();
                Bits[Index >> 5u] |= (1u << (Index & 31u));
            });
        }
    }
#endif

    void FDefaultSceneRenderer::ExtractDebugText(FFrameData& Frame)
    {
        Frame.Primitives.DebugTextGlyphs.clear();
        Frame.Primitives.DebugTextBatch = {};
#if !defined(LE_SHIPPING)
        {
            TVector<FDebugTextLine> DebugLines;
            World->DrainDebugTextLines(DebugLines);

            CFont* DebugFont = CFontManager::Get().GetDefaultFont();
            const int32 DebugAtlasID = DebugFont ? DebugFont->GetAtlasResourceID() : -1;
            if (!DebugLines.empty() && DebugFont && DebugFont->HasAtlas() && DebugAtlasID >= 0)
            {
                const float PxSize = 32.0f;   // pixels per em
                const float Margin = 12.0f;
                float       PenY   = Margin;

                TVector<FShapedGlyph> DebugShaped;
                for (const FDebugTextLine& Line : DebugLines)
                {
                    const uint32 Color = PackColor(Line.Color);
                    if (DebugFont->ShapeText(Line.Text, 0.0f /*left*/, 0.0f, 1.0f, DebugShaped))
                    {
                        for (const FShapedGlyph& S : DebugShaped)
                        {
                            FGPUGlyph& G = Frame.Primitives.DebugTextGlyphs.emplace_back();
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

                if (!Frame.Primitives.DebugTextGlyphs.empty())
                {
                    FFrameData::FTextBatch& Batch = Frame.Primitives.DebugTextBatch;
                    Batch.AtlasIndex    = (uint32)DebugAtlasID;
                    Batch.AtlasWidth    = DebugFont->GetAtlasWidth();
                    Batch.AtlasHeight   = DebugFont->GetAtlasHeight();
                    Batch.DistanceRange = DebugFont->GetDistanceRange();
                    Batch.FirstInstance = 0;
                    Batch.Count         = (uint32)Frame.Primitives.DebugTextGlyphs.size();
                }
            }
        }
#endif
    }

    void FDefaultSceneRenderer::ExtractDecals(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        LUMINA_PROFILE_SECTION("Extract Decals");

        auto TransformStorage = Registry.GetStorage<STransformComponent>();
        auto DecalView = Registry.View<SDecalComponent>(ECS::TExclude<SDisabledTag>{});

        Frame.Primitives.DecalExtracts.clear();
        Frame.Primitives.DecalBatches.clear();
        DecalSortScratch.clear();
        DecalGroupMinSort.clear();

        DecalView.ForEach([&](ECS::FEntity Entity, const SDecalComponent& Decal)
        {
            CMaterialInterface* Material = Decal.DecalMaterial.Get();
            FShaderH DecalVS;
            FShaderH DecalPS;
            if (Material == nullptr || !Material->ResolveDomainShaders(EMaterialType::Decal, DecalVS, DecalPS))
            {
                return;
            }
            CMaterial* ShaderOwner = Material->GetMaterial();
            const int32 MaterialIndex = Material->GetMaterialIndex();
            if (MaterialIndex < 0)
            {
                return;
            }

            FGPUDecal Item;
            Item.DecalToWorld  = Math::Scale(TransformStorage.Get(Entity).GetWorldMatrix(), Decal.Size);
            Item.WorldToDecal  = Math::Inverse(Item.DecalToWorld);
            Item.FadeAngleCos  = Math::Cos(Math::Radians(Math::Clamp(Decal.FadeAngle, 0.0f, 89.9f)));
            Item.Opacity       = Math::Clamp(Decal.Opacity, 0.0f, 1.0f);
            Item.MaterialIndex = (uint32)MaterialIndex;
            Item.Flags         = 0;

            DecalSortScratch.push_back({ ShaderOwner, Decal.SortOrder, Item });
        });

        for (const FDecalSortEntry& E : DecalSortScratch)
        {
            auto It = DecalGroupMinSort.find(E.ShaderOwner);
            if (It == DecalGroupMinSort.end() || E.SortOrder < It->second)
            {
                DecalGroupMinSort[E.ShaderOwner] = E.SortOrder;
            }
        }
        Algo::StableSort(DecalSortScratch, [&](const FDecalSortEntry& A, const FDecalSortEntry& B)
        {
            const int32 GA = DecalGroupMinSort[A.ShaderOwner];
            const int32 GB = DecalGroupMinSort[B.ShaderOwner];
            if (GA != GB)
            {
                return GA < GB;
            }
            if (A.ShaderOwner != B.ShaderOwner)
            {
                return A.ShaderOwner < B.ShaderOwner;
            }
            return A.SortOrder < B.SortOrder;
        });

        Frame.Primitives.DecalExtracts.reserve(DecalSortScratch.size());
        CMaterial* PrevOwner = nullptr;
        for (uint32 i = 0; i < (uint32)DecalSortScratch.size(); ++i)
        {
            Frame.Primitives.DecalExtracts.push_back(DecalSortScratch[i].Gpu);

            CMaterial* Owner = DecalSortScratch[i].ShaderOwner;
            if (Owner == PrevOwner && !Frame.Primitives.DecalBatches.empty())
            {
                Frame.Primitives.DecalBatches.back().Count++;
            }
            else
            {
                FFrameData::FDecalBatch& Batch = Frame.Primitives.DecalBatches.emplace_back();
                Batch.Shaders.VertexShader = Owner->GetVertexShader();
                Batch.Shaders.PixelShader  = Owner->GetPixelShader();
                Batch.FirstInstance        = i;
                Batch.Count                = 1u;
                PrevOwner                  = Owner;
            }
        }
    }

    void FDefaultSceneRenderer::ExtractWater(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        LUMINA_PROFILE_SECTION("Extract Water");

        auto TransformStorage = Registry.GetStorage<STransformComponent>();
        auto WaterView = Registry.View<SWaterComponent>(ECS::TExclude<SDisabledTag>{});

        Frame.Water.Surfaces.clear();
        Frame.Water.bUnderwaterActive = false;

        const FVector4& CamLoc = Frame.SceneGlobalData.CameraData.Location;
        const FVector3  CameraPos = FVector3(CamLoc.x, CamLoc.y, CamLoc.z);

        // Track the nearest water surface above the camera (largest local.y still below the plane).
        float BestUnderwaterLocalY = -1.0e30f;

        auto ResolveTexture = [](const TObjectPtr<CTexture>& Tex) -> uint32
        {
            const CTexture* T = Tex.Get();
            const int32 ID = T ? T->GetResourceID() : -1;
            return ID >= 0 ? (uint32)ID : ~0u;
        };

        WaterView.ForEach([&](ECS::FEntity Entity, const SWaterComponent& Water)
        {
            const FMatrix4 WorldMatrix = TransformStorage.Get(Entity).GetWorldMatrix();
            const float ExtentX = Math::Max(Water.Extent.x, 0.01f);
            const float ExtentZ = Math::Max(Water.Extent.y, 0.01f);

            FGPUWater Item    = {};
            Item.WaterToWorld = Math::Scale(WorldMatrix, FVector3(ExtentX, 1.0f, ExtentZ));
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
            Item.DetailParams   = FVector4(Math::Max(Water.DetailTiling, 0.01f),
                                           Math::Max(Water.DetailScrollSpeed, 0.0f),
                                           Math::Max(Water.FoamTiling, 0.01f),
                                           Math::Max(Water.FoamIntensity, 0.0f));

            Item.DetailNormalIndex = ResolveTexture(Water.DetailNormalMap);
            Item.FoamTextureIndex  = ResolveTexture(Water.FoamTexture);
            Item.GridResolution    = (uint32)Math::Clamp(Water.GridResolution, 2, 512);
            Item.Flags             = 0;

            Frame.Water.Surfaces.push_back(Item);
            const FVector4 LocalCam = Item.WorldToWater * FVector4(CameraPos, 1.0f);
            if (Math::Abs(LocalCam.x) <= 0.5f && Math::Abs(LocalCam.z) <= 0.5f &&
                LocalCam.y < 0.0f && LocalCam.y > BestUnderwaterLocalY)
            {
                BestUnderwaterLocalY = LocalCam.y;

                const FVector3 SurfaceCenter = TransformStorage.Get(Entity).GetWorldLocation();
                Frame.Water.bUnderwaterActive = true;
                Frame.Water.Underwater.PlaneNormalAndHeight = FVector4(0.0f, 1.0f, 0.0f, SurfaceCenter.y);
                Frame.Water.Underwater.FogColorDensity      = FVector4(Water.UnderwaterFogColor, Math::Max(Water.UnderwaterFogDensity, 0.0f));
                Frame.Water.Underwater.TintDistortion       = FVector4(Water.UnderwaterTint, Math::Max(Water.UnderwaterDistortion, 0.0f));
                Frame.Water.Underwater.DeepColor            = FVector4(Water.DeepColor, 0.0f);
            }
        });
    }
}
