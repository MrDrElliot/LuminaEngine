#include "TerrainEditMode.h"
#include "BrushModeUI.h"
#include "World/ECS/Registry.h"

#include <cmath>
#include "Core/Math/Math.h"
#include "Memory/Memcpy.h"

#include "Platform/Process/PlatformProcess.h"
#include "Renderer/Format.h"
#include "Tools/Import/ImportHelpers.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Scene/RenderScene/TerrainRenderTypes.h"
#include "World/Subsystems/TerrainSculptSystem.h"
#include "World/World.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    namespace
    {
        // In ETerrainBrushMode order, so a mode indexes its own entry.
        constexpr BrushUI::FTool kTerrainTools[] =
        {
            { LE_ICON_ELEVATION_RISE,      "Sculpt",  "Raise the ground, or lower it while Alt is held.",                                   IM_COL32(235, 175,  70, 255) },
            { LE_ICON_BLUR,                "Smooth",  "Relax bumps and creases, over a wider area with a wider brush. Shift smooths from any sculpt tool.", IM_COL32( 95, 200, 120, 255) },
            { LE_ICON_FORMAT_ALIGN_MIDDLE, "Flatten", "Level the ground to the height where the stroke starts, or to a fixed height.",       IM_COL32( 80, 200, 230, 255) },
            { LE_ICON_SLOPE_UPHILL,        "Ramp",    "Drag from A to B for a straight slope between the heights there.",                    IM_COL32( 95, 150, 240, 255) },
            { LE_ICON_GRAIN,               "Noise",   "Add fractal bumps, or carve them in while Alt is held.",                              IM_COL32(185, 130, 230, 255) },
            { LE_ICON_LANDSLIDE,           "Erode",   "Slump slopes steeper than the talus angle into scree, the way loose rock settles.",   IM_COL32(215, 120,  80, 255) },
            { LE_ICON_STAIRS,              "Terrace", "Step the ground into flat treads, sharper the longer the brush is held.",             IM_COL32(210, 205, 100, 255) },
            { LE_ICON_FORMAT_PAINT,        "Paint",   "Paint the selected layer, or erase it while Alt is held.",                            IM_COL32(230, 100, 150, 255) },
        };
        static_assert(IM_ARRAYSIZE(kTerrainTools) == size_t(ETerrainBrushMode::Num));

        constexpr const char* kFlattenSideNames[] = { "Raise and lower", "Raise only", "Lower only" };
        static_assert(IM_ARRAYSIZE(kFlattenSideNames) == size_t(ETerrainFlattenSide::Num));

        // Capped at the shader's four-layer maximum, so the editor cannot paint a slot nothing reads.
        constexpr ImU32 kLayerSwatchColors[GTerrainMaxLayers] =
        {
            IM_COL32(220,  80,  80, 255),
            IM_COL32( 80, 200, 110, 255),
            IM_COL32( 80, 140, 230, 255),
            IM_COL32(220, 200,  80, 255),
        };

        constexpr ImU32 kLowerColor = IM_COL32(90, 150, 255, 255);
        constexpr float kMaxRadius  = 8192.0f;

        const BrushUI::FTool& GetTerrainTool(ETerrainBrushMode Mode)
        {
            return kTerrainTools[Math::Min(int32(Mode), int32(IM_ARRAYSIZE(kTerrainTools)) - 1)];
        }
    }

    ECS::FEntity FTerrainEditMode::FindPreferredTerrain(CWorld* World) const
    {
        if (!World)
        {
            return ECS::NullEntity;
        }
        auto View = World->View<STerrainComponent>();
        for (auto Entity : View)
        {
            return Entity;
        }
        return ECS::NullEntity;
    }

    ECS::FEntity FTerrainEditMode::CreateDefaultTerrain(CWorld* World)
    {
        if (!World)
        {
            return ECS::NullEntity;
        }

        ECS::FEntity Entity = World->ConstructEntity("Terrain");
        STerrainComponent& Terrain = World->EmplaceComponent<STerrainComponent>(Entity);
        Terrain.Resolution      = 513;
        Terrain.ChunkResolution = 64;
        Terrain.TileWorldSize   = 4096.0f;
        Terrain.MaxHeight       = 256.0f;

        const int32 Res = Terrain.Resolution;
        // Start dead flat; the user sculpts from a clean slate.
        Terrain.Heightmap.Init(0.0f, Res * Res);

        // Weights start at zero, painting raises them, and the shader normalizes at sample time.
        static const char* LayerNames[4] = { "Layer0", "Layer1", "Layer2", "Layer3" };
        for (int i = 0; i < 4; ++i)
        {
            STerrainLayer& L = Terrain.Layers.EmplaceGetRef();
            L.Name    = LayerNames[i];
            L.UVScale = 1.0f / 16.0f;
        }

        Terrain.LayerWeights.Init(uint8(0), Res * Res * Terrain.Layers.Num());
        Terrain.CPUState.bFullHeightmapDirty = true;
        Terrain.CPUState.bFullWeightsDirty   = true;
        return Entity;
    }

    ECS::FEntity FTerrainEditMode::CreateTerrainTransacted(CWorld* World)
    {
        if (Context)
        {
            Context->BeginModeCreationTransaction();
        }
        ECS::FEntity Entity = CreateDefaultTerrain(World);
        if (Context)
        {
            Context->EndModeTransaction("Create Terrain");
        }
        return Entity;
    }

    bool FTerrainEditMode::ImportHeightmap(STerrainComponent& Terrain, const char* FilePath, FString& OutError, int32& OutSrcW, int32& OutSrcH)
    {
        OutSrcW = 0;
        OutSrcH = 0;

        if (FilePath == nullptr || *FilePath == '\0')
        {
            OutError = "No file selected.";
            return false;
        }

        // Read as-is with no flip and no sRGB curve, so the stored sample maps directly to height.
        TOptional<Import::Textures::FTextureImportResult> Decoded = Import::Textures::ImportTexture(FStringView(FilePath), /*bFlipVertical*/ false);
        if (!Decoded.IsSet() || Decoded->Pixels.IsEmpty())
        {
            OutError = "Could not decode image (unsupported, corrupt, or empty file).";
            return false;
        }

        const Import::Textures::FTextureImportResult& Img = *Decoded;
        const int32 W = (int32)Img.Dimensions.x;
        const int32 H = (int32)Img.Dimensions.y;
        OutSrcW = W;
        OutSrcH = H;

        if (W < 2 || H < 2)
        {
            OutError = "Image is too small to be a heightmap (needs at least 2x2).";
            return false;
        }

        // Classify the decoded format into (element kind, channel count); the height is the red channel.
        enum class EKind { U8, U16, F32 };
        EKind Kind     = EKind::U8;
        int32 Channels = 1;
        switch (Img.Format)
        {
            case EFormat::R8_UNORM:                          Kind = EKind::U8;  Channels = 1; break;
            case EFormat::RG8_UNORM:                         Kind = EKind::U8;  Channels = 2; break;
            case EFormat::RGBA8_UNORM: case EFormat::SRGBA8_UNORM: Kind = EKind::U8;  Channels = 4; break;
            case EFormat::R16_UNORM:                         Kind = EKind::U16; Channels = 1; break;
            case EFormat::RG16_UNORM:                        Kind = EKind::U16; Channels = 2; break;
            case EFormat::RGBA16_UNORM:                      Kind = EKind::U16; Channels = 4; break;
            case EFormat::R32_FLOAT:                         Kind = EKind::F32; Channels = 1; break;
            case EFormat::RG32_FLOAT:                        Kind = EKind::F32; Channels = 2; break;
            case EFormat::RGB32_FLOAT:                       Kind = EKind::F32; Channels = 3; break;
            case EFormat::RGBA32_FLOAT:                      Kind = EKind::F32; Channels = 4; break;
            default:
                OutError = "Unsupported pixel format for a heightmap.";
                return false;
        }

        const int64 ElemSize  = (Kind == EKind::U8) ? 1 : (Kind == EKind::U16 ? 2 : 4);
        const int64 Stride    = static_cast<int64>(W) * Channels * ElemSize;
        if (Img.Pixels.Num() < Stride * H)
        {
            OutError = "Decoded pixel buffer is smaller than its reported dimensions.";
            return false;
        }

        const uint8* Base = Img.Pixels.GetData();
        auto ReadHeight = [&](int32 px, int32 py) -> float
        {
            const int64 Sample = (static_cast<int64>(py) * W + px) * Channels;
            switch (Kind)
            {
                case EKind::U8:  return (float)Base[Sample] / 255.0f;
                case EKind::U16: return (float)reinterpret_cast<const uint16*>(Base)[Sample] / 65535.0f;
                case EKind::F32: return Math::Clamp(reinterpret_cast<const float*>(Base)[Sample], 0.0f, 1.0f);
            }
            return 0.0f;
        };

        // Bilinear resampling means any image size works without disturbing the chunking invariant.
        const int32 Res = Terrain.Resolution;
        if (Res < 2)
        {
            OutError = "Terrain resolution is invalid.";
            return false;
        }

        Terrain.Heightmap.Init(0.0f, Res * Res);

        for (int32 ty = 0; ty < Res; ++ty)
        {
            const float V  = (float)ty / (float)(Res - 1);
            const float Fy = V * (float)(H - 1);
            const int32 Y0 = (int32)Fy;
            const int32 Y1 = Math::Min(Y0 + 1, H - 1);
            const float Dy = Fy - (float)Y0;

            for (int32 tx = 0; tx < Res; ++tx)
            {
                const float U  = (float)tx / (float)(Res - 1);
                const float Fx = U * (float)(W - 1);
                const int32 X0 = (int32)Fx;
                const int32 X1 = Math::Min(X0 + 1, W - 1);
                const float Dx = Fx - (float)X0;

                const float H00 = ReadHeight(X0, Y0);
                const float H10 = ReadHeight(X1, Y0);
                const float H01 = ReadHeight(X0, Y1);
                const float H11 = ReadHeight(X1, Y1);
                const float Height = Math::Mix(Math::Mix(H00, H10, Dx), Math::Mix(H01, H11, Dx), Dy);

                Terrain.Heightmap[ty * Res + tx] = Math::Clamp(Height, 0.0f, 1.0f);
            }
        }

        Terrain.MarkHeightmapReplaced();
        return true;
    }

    void FTerrainEditMode::OnEnter(CWorld* World)
    {
        // Spawning a default terrain makes the mode self-bootstrapping, with something to paint on.
        if (World && FindPreferredTerrain(World) == ECS::NullEntity)
        {
            CreateDefaultTerrain(World);
        }
    }

    void FTerrainEditMode::OnExit(CWorld* World)
    {
        // Leaving mid-stroke must still commit the in-flight transaction so undo stays balanced.
        if (bTransactionOpen && Context)
        {
            Context->EndModeTransaction("Terrain Edit");
        }
        bTransactionOpen = false;
        bHasStrokeHit    = false;
        bRampStarted     = false;
        CloseLayerEditTransaction();
    }

    void FTerrainEditMode::TrackLayerEditTransaction(const char* Name)
    {
        if (ImGui::IsItemActivated() && Context && !OpenLayerEditName)
        {
            Context->BeginModeTransaction();
            OpenLayerEditName = Name;
        }
        else if (ImGui::IsItemDeactivated())
        {
            CloseLayerEditTransaction();
        }
    }

    void FTerrainEditMode::CloseLayerEditTransaction()
    {
        if (OpenLayerEditName && Context)
        {
            Context->EndModeTransaction(OpenLayerEditName);
        }
        OpenLayerEditName = nullptr;
    }

    void FTerrainEditMode::DrawToolbar(CWorld* World, float ButtonSize)
    {
        if (!World)
        {
            return;
        }

        ImGui::SameLine();
        if (ImGui::Button("+ Terrain", ImVec2(0, ButtonSize)))
        {
            CreateTerrainTransacted(World);
        }

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_IMPORT " Heightmap", ImVec2(0, ButtonSize)))
        {
            // Import a heightmap image onto the active terrain (creating a default one first if none exists).
            ECS::FEntity TerrainEntity = FindPreferredTerrain(World);
            if (TerrainEntity == ECS::NullEntity)
            {
                TerrainEntity = CreateTerrainTransacted(World);
            }

            FFixedString File;
            const char* Filter =
                "Heightmap (*.png;*.tga;*.hdr;*.jpg)\0*.png;*.tga;*.hdr;*.jpg\0"
                "PNG (*.png)\0*.png\0"
                "All Files (*.*)\0*.*\0";
            if (TerrainEntity != ECS::NullEntity && Platform::OpenFileDialogue(File, "Select Heightmap Image", Filter))
            {
                STerrainComponent& Terrain = World->GetComponent<STerrainComponent>(TerrainEntity);

                FString Error;
                int32 SrcW = 0;
                int32 SrcH = 0;
                bool bOk = false;
                RunModeTransaction("Import Heightmap", [&] { bOk = ImportHeightmap(Terrain, *File, Error, SrcW, SrcH); });

                if (bOk)
                {
                    ImGuiX::Notifications::NotifySuccess("Heightmap imported: {}x{} -> {}^2 grid.", SrcW, SrcH, Terrain.Resolution);
                }
                else
                {
                    ImGuiX::Notifications::NotifyError("Heightmap import failed: {}", *Error);
                }
            }
        }

        ImGui::SameLine();
        if (ImGui::Button(bShowSettings ? "Hide Brush" : "Brush", ImVec2(0, ButtonSize)))
        {
            bShowSettings = !bShowSettings;
        }

        if (!bShowSettings)
        {
            return;
        }

        // Four tool buttons a row with their labels set the width.
        BrushUI::BeginPanel("##TerrainBrushSettings", 400.0f);

        int32 Tool = int32(Brush.Mode);
        if (BrushUI::ToolPalette(kTerrainTools, IM_ARRAYSIZE(kTerrainTools), Tool))
        {
            Brush.Mode    = ETerrainBrushMode(Tool);
            bRampStarted  = false;
            bHasStrokeHit = false;
        }
        BrushUI::BrushSection(Brush.Radius, kMaxRadius, Brush.Strength, Brush.Falloff, Brush.FalloffShape, GetTerrainTool(Brush.Mode).Color);

        DrawToolSettings(World);

        ImGui::Separator();
        BrushUI::Hint("[ ] size   Shift+[ ] strength   Shift smooths   Alt inverts");

        ImGui::End();
    }

    void FTerrainEditMode::DrawToolSettings(CWorld* World)
    {
        const BrushUI::FTool& Tool = GetTerrainTool(Brush.Mode);
        ImGui::SeparatorText(Tool.Label);
        BrushUI::Hint(Tool.Tip);

        const ECS::FEntity TerrainEntity = FindPreferredTerrain(World);
        STerrainComponent* Terrain = TerrainEntity != ECS::NullEntity ? World->TryGetComponent<STerrainComponent>(TerrainEntity) : nullptr;
        const float MaxHeight = Terrain != nullptr ? Terrain->MaxHeight : 1024.0f;

        switch (Brush.Mode)
        {
        case ETerrainBrushMode::Sculpt:
            ImGui::Checkbox("Lower instead of raise", &bInvertSculpt);
            break;

        case ETerrainBrushMode::Noise:
        {
            float FeatureSize = 1.0f / Math::Max(Brush.NoiseFrequency, 1e-6f);
            if (BrushUI::Slider("Feature", &FeatureSize, 4.0f, 4096.0f, "%.0f m", Tool.Color, true))
            {
                Brush.NoiseFrequency = 1.0f / Math::Max(FeatureSize, 1.0f);
            }
            BrushUI::SliderInt("Octaves", &Brush.NoiseOctaves, 1, 8, "%d", Tool.Color);
            ImGui::Checkbox("Carve instead of add", &bInvertSculpt);
            break;
        }

        case ETerrainBrushMode::Flatten:
        {
            ImGui::Checkbox("Fixed height", &bFixedFlattenHeight);
            if (bFixedFlattenHeight)
            {
                BrushUI::Slider("Height", &Brush.FlattenHeight, 0.0f, MaxHeight, "%.1f m", Tool.Color);
                ImGui::TextDisabled("Ctrl+Click samples the ground under the cursor.");
            }
            else
            {
                ImGui::TextDisabled("Levels to %.1f m, where the last stroke started.", Brush.FlattenHeight);
            }
            int32 Side = int32(Brush.FlattenSide);
            if (BrushUI::Combo("Direction", &Side, kFlattenSideNames, IM_ARRAYSIZE(kFlattenSideNames)))
            {
                Brush.FlattenSide = ETerrainFlattenSide(Side);
            }
            break;
        }

        case ETerrainBrushMode::Ramp:
            BrushUI::Slider("Half width", &Brush.RampHalfWidth, 1.0f, 4096.0f, "%.0f m", Tool.Color, true);
            ImGui::Checkbox("Fixed end heights", &Brush.RampUseExplicitHeights);
            ImGui::SetItemTooltip("Use fixed start and end heights instead of the ground under A and B.");
            if (Brush.RampUseExplicitHeights)
            {
                BrushUI::Slider("Start", &Brush.RampStartHeight, 0.0f, MaxHeight, "%.1f m", Tool.Color);
                BrushUI::Slider("End", &Brush.RampEndHeight, 0.0f, MaxHeight, "%.1f m", Tool.Color);
            }
            break;

        case ETerrainBrushMode::Erode:
            BrushUI::Slider("Talus", &Brush.TalusAngle, 5.0f, 80.0f, "%.0f\xc2\xb0", Tool.Color);
            ImGui::SetItemTooltip("The steepest slope that stays standing.");
            break;

        case ETerrainBrushMode::Terrace:
            BrushUI::Slider("Step", &Brush.TerraceStep, 0.5f, Math::Max(MaxHeight * 0.5f, 1.0f), "%.1f m", Tool.Color, true);
            break;

        case ETerrainBrushMode::Paint:
            if (Terrain != nullptr)
            {
                DrawLayerPanel(*Terrain);
            }
            break;

        default:
            break;
        }
    }

    void FTerrainEditMode::DrawLayerPanel(STerrainComponent& Terrain)
    {
        const int32 LayerCount = Terrain.Layers.Num();

        ImGui::Separator();
        ImGui::TextUnformatted("Layers");

        const ImVec2 SwatchSize(28, 28);
        for (int32 i = 0; i < LayerCount; ++i)
        {
            ImGui::PushID(i);
            const bool bSelected = (Brush.ActiveLayer == i);
            const ImU32 Border  = bSelected ? IM_COL32(255, 255, 255, 255) : IM_COL32(80, 80, 80, 255);
            const ImU32 Fill    = kLayerSwatchColors[i % GTerrainMaxLayers];

            ImVec2 Pos = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##Swatch", SwatchSize);
            if (ImGui::IsItemClicked())
            {
                Brush.ActiveLayer = i;
            }
            ImDrawList* Dl = ImGui::GetWindowDrawList();
            Dl->AddRectFilled(Pos, ImVec2(Pos.x + SwatchSize.x, Pos.y + SwatchSize.y), Fill, 4.0f);
            Dl->AddRect(Pos, ImVec2(Pos.x + SwatchSize.x, Pos.y + SwatchSize.y), Border, 4.0f, bSelected ? 2.5f : 1.0f, 0);

            if (ImGui::IsItemHovered() && !Terrain.Layers[i].Name.IsEmpty())
            {
                ImGui::SetTooltip("%s", *Terrain.Layers[i].Name);
            }
            ImGui::PopID();
            if (i + 1 < LayerCount)
            {
                ImGui::SameLine();
            }
        }

        if (LayerCount < (int32)GTerrainMaxLayers)
        {
            if (LayerCount > 0) ImGui::SameLine();
            if (ImGui::Button("+", SwatchSize))
            {
                RunModeTransaction("Add Terrain Layer", [&]
                {
                    STerrainLayer& L = Terrain.Layers.EmplaceGetRef();
                    L.Name = Format("Layer{}", LayerCount);
                    L.UVScale = 1.0f / 16.0f;
                    EnsureLayerWeightStorage(Terrain);
                    Terrain.CPUState.bFullWeightsDirty = true;
                });
            }
        }
        else
        {
            if (LayerCount > 0) ImGui::SameLine();
            ImGui::TextDisabled("(max %d)", GTerrainMaxLayers);
        }

        Brush.ActiveLayer = Math::Clamp(Brush.ActiveLayer, 0, Math::Max(LayerCount - 1, 0));

        // Reorder also mutates LayerWeights, so weights follow the layer rather than swapping under it.
        if (Brush.ActiveLayer >= 0 && Brush.ActiveLayer < LayerCount)
        {
            STerrainLayer& L = Terrain.Layers[Brush.ActiveLayer];

            BrushUI::RowLabel("Name");
            ImGuiX::InputText("##LayerName", L.Name, ImGuiInputTextFlags_AutoSelectAll);
            TrackLayerEditTransaction("Rename Terrain Layer");

            BrushUI::Slider("UV scale", &L.UVScale, 1.0f / 1024.0f, 1.0f, "%.4f", kLayerSwatchColors[Brush.ActiveLayer % GTerrainMaxLayers], true);
            TrackLayerEditTransaction("Edit Terrain Layer UV Scale");

            const bool bCanUp   = Brush.ActiveLayer > 0;
            const bool bCanDown = Brush.ActiveLayer + 1 < LayerCount;

            ImGui::BeginDisabled(!bCanUp);
            if (ImGui::Button("Up"))
            {
                RunModeTransaction("Move Terrain Layer Up", [&] { SwapLayers(Terrain, Brush.ActiveLayer, Brush.ActiveLayer - 1); });
                --Brush.ActiveLayer;
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(!bCanDown);
            if (ImGui::Button("Down"))
            {
                RunModeTransaction("Move Terrain Layer Down", [&] { SwapLayers(Terrain, Brush.ActiveLayer, Brush.ActiveLayer + 1); });
                ++Brush.ActiveLayer;
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(LayerCount <= 1);
            if (ImGui::Button("Remove"))
            {
                RunModeTransaction("Remove Terrain Layer", [&] { RemoveLayer(Terrain, Brush.ActiveLayer); });
                Brush.ActiveLayer = Math::Min(Brush.ActiveLayer, Terrain.Layers.Num() - 1);
            }
            ImGui::EndDisabled();
        }
    }

    void FTerrainEditMode::EnsureLayerWeightStorage(STerrainComponent& Terrain)
    {
        const int32 LayerStride = Terrain.Resolution * Terrain.Resolution;
        const int32 Need = LayerStride * Math::Max(Terrain.Layers.Num(), 1);
        if (Terrain.LayerWeights.Num() != Need)
        {
            Terrain.LayerWeights.SetNum(Need, uint8(0));
        }
    }

    void FTerrainEditMode::SwapLayers(STerrainComponent& Terrain, int32 A, int32 B)
    {
        if (A == B) return;
        const int32 LayerCount = Terrain.Layers.Num();
        if (A < 0 || B < 0 || A >= LayerCount || B >= LayerCount) return;

        EnsureLayerWeightStorage(Terrain);
        Terrain.Layers.Swap(A, B);

        const int32 LayerStride = Terrain.Resolution * Terrain.Resolution;
        TVector<uint8> Tmp(LayerStride);
        uint8* PA = &Terrain.LayerWeights[A * LayerStride];
        uint8* PB = &Terrain.LayerWeights[B * LayerStride];
        Memory::Memcpy(Tmp.GetData(), PA, LayerStride);
        Memory::Memcpy(PA,         PB, LayerStride);
        Memory::Memcpy(PB, Tmp.GetData(), LayerStride);

        Terrain.CPUState.bFullWeightsDirty = true;
    }

    void FTerrainEditMode::RemoveLayer(STerrainComponent& Terrain, int32 Index)
    {
        const int32 LayerCount = Terrain.Layers.Num();
        if (Index < 0 || Index >= LayerCount || LayerCount <= 1) return;

        EnsureLayerWeightStorage(Terrain);
        const int32 LayerStride = Terrain.Resolution * Terrain.Resolution;

        // Pack remaining layers down, then resize the trailing slot away.
        for (int32 i = Index; i + 1 < LayerCount; ++i)
        {
            uint8* Dst = &Terrain.LayerWeights[i     * LayerStride];
            uint8* Src = &Terrain.LayerWeights[(i + 1) * LayerStride];
            Memory::Memcpy(Dst, Src, LayerStride);
        }
        Terrain.LayerWeights.SetNum((LayerCount - 1) * LayerStride);
        Terrain.Layers.RemoveAt(Index);
        Terrain.CPUState.bFullWeightsDirty = true;
    }

    void FTerrainEditMode::Tick(CWorld* World, const SCameraComponent& Camera, bool bViewportHovered, ImVec2 ViewportScreenOrigin, ImVec2 ViewportSize)
    {
        bHitValid      = false;
        ReadoutSeconds = Math::Max(ReadoutSeconds - ImGui::GetIO().DeltaTime, 0.0f);

        // The moment the button is released anywhere, commit the stroke's undo transaction.
        if (bTransactionOpen && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            if (Context)
            {
                Context->EndModeTransaction("Terrain Edit");
            }
            bTransactionOpen = false;
        }

        if (!World)
        {
            bRampStarted  = false;
            bHasStrokeHit = false;
            return;
        }

        ECS::FEntity TerrainEntity = FindPreferredTerrain(World);
        if (TerrainEntity == ECS::NullEntity)
        {
            return;
        }
        STerrainComponent& Terrain = World->GetComponent<STerrainComponent>(TerrainEntity);
        if (Terrain.Resolution < 2)
        {
            return;
        }

        // The renderer centers on the entity transform, so sculpting needs that origin to match.
        const STransformComponent& TerrainTransform = World->GetComponent<STransformComponent>(TerrainEntity);
        const FVector3 TerrainOrigin = TerrainTransform.ComputeWorldTransform().GetLocation();

        if (BrushUI::HandleBrushKeys(Brush.Radius, kMaxRadius, Brush.Strength))
        {
            ReadoutSeconds = 1.5f;
        }

        if (!bViewportHovered)
        {
            bRampStarted  = false;
            bHasStrokeHit = false;
            return;
        }

        const ImVec2 MousePos = ImGui::GetMousePos();
        const ImVec2 Local    = ImVec2(MousePos.x - ViewportScreenOrigin.x, MousePos.y - ViewportScreenOrigin.y);
        if (Local.x < 0.0f || Local.y < 0.0f || Local.x > ViewportSize.x || Local.y > ViewportSize.y)
        {
            return;
        }

        FVisualizerView View;
        View.SetFromCamera(Camera, ViewportScreenOrigin, ViewportSize);
        FVector3 RayOrigin, RayDir;
        View.ScreenToRay(MousePos, RayOrigin, RayDir);

        FVector3 Hit;
        if (!FTerrainSculptSystem::Raycast(Terrain, TerrainOrigin, RayOrigin, RayDir, Hit))
        {
            // No hit this frame -> stroke is broken; reset continuity bookkeeping.
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                bRampStarted  = false;
                bHasStrokeHit = false;
            }
            return;
        }

        LastHit   = Hit;
        bHitValid = true;

        const ImGuiIO& IO = ImGui::GetIO();

        // On click rather than down, so a held stroke cannot accidentally retarget itself.
        if (Brush.Mode == ETerrainBrushMode::Flatten && IO.KeyCtrl && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            Brush.FlattenHeight = Hit.y - TerrainOrigin.y;
            bFixedFlattenHeight = true;
            return;
        }

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            bRampStarted  = false;
            bHasStrokeHit = false;
            return;
        }

        // The host suppresses gizmo manipulation entirely here, so ImGuizmo state is stale.

        if (Brush.Mode == ETerrainBrushMode::Flatten && !bFixedFlattenHeight && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            Brush.FlattenHeight = Hit.y - TerrainOrigin.y;
        }

        // Ramp captures the start of the stroke once, then drags the end while held.
        if (Brush.Mode == ETerrainBrushMode::Ramp)
        {
            if (!bRampStarted)
            {
                Brush.RampStart = Hit;
                bRampStarted    = true;
            }
            Brush.RampEnd = Hit;
        }

        // Wall-clock delta, since the simulated tick is zero in edit mode and every dab a no-op.
        const float RealDelta = Math::Max(IO.DeltaTime, 0.0f);

        // Applied every frame while held, including a stationary cursor, which is how you dig in place.
        FTerrainSculptDab Dab = Brush;
        Dab.Mode          = IsMomentarySmooth() ? ETerrainBrushMode::Smooth : Brush.Mode;
        Dab.WorldPosition = Hit;
        Dab.TerrainOrigin = TerrainOrigin;
        Dab.DeltaSeconds  = RealDelta;
        Dab.SculptSign    = (bInvertSculpt != IO.KeyAlt) ? int8(-1) : int8(1);

        // Opened before the first edit so the snapshot captures the pre-stroke heightmap.
        if (!bTransactionOpen && Context)
        {
            Context->BeginModeTransaction();
            bTransactionOpen = true;
        }

        // Ramp re-applies the whole line each frame, while footprint brushes interpolate dabs along travel.
        if (Dab.Mode == ETerrainBrushMode::Ramp || !bHasStrokeHit)
        {
            FTerrainSculptSystem::ApplyDab(Terrain, Dab);
        }
        else
        {
            const FVector3 Delta = Hit - LastStrokeHit;
            const float Dist    = Math::Length(FVector2(Delta.x, Delta.z));
            const float Spacing = Math::Max(Brush.Radius * 0.25f, 1.0f);
            const int   Steps   = Math::Clamp((int)std::ceil(Dist / Spacing), 1, 32);
            const float StepDt  = RealDelta / float(Steps);
            for (int i = 1; i <= Steps; ++i)
            {
                Dab.WorldPosition = Math::Mix(LastStrokeHit, Hit, float(i) / float(Steps));
                Dab.DeltaSeconds  = StepDt;
                FTerrainSculptSystem::ApplyDab(Terrain, Dab);
            }
        }

        LastStrokeHit = Hit;
        bHasStrokeHit = true;
    }

    bool FTerrainEditMode::IsMomentarySmooth() const
    {
        // Paint and Ramp keep Shift, since smoothing in their place would surprise rather than help.
        return ImGui::GetIO().KeyShift && Brush.Mode != ETerrainBrushMode::Paint && Brush.Mode != ETerrainBrushMode::Ramp;
    }

    void FTerrainEditMode::DrawOverlay(CWorld* World, ImVec2 ViewportScreenOrigin, ImVec2 ViewportSize, const SCameraComponent& Camera)
    {
        if (!bHitValid || !World)
        {
            return;
        }

        FVisualizerView View;
        View.SetFromCamera(Camera, ViewportScreenOrigin, ViewportSize);

        ImVec2 Center;
        if (!View.WorldToScreen(LastHit, Center))
        {
            return;
        }

        ImDrawList* Draw = ImGui::GetWindowDrawList();

        const ECS::FEntity TerrainEntity = FindPreferredTerrain(World);
        const STerrainComponent* Terrain = TerrainEntity != ECS::NullEntity ? World->TryGetComponent<STerrainComponent>(TerrainEntity) : nullptr;
        const FVector3 TerrainOrigin     = Terrain != nullptr ? World->GetComponent<STransformComponent>(TerrainEntity).ComputeWorldTransform().GetLocation() : FVector3(0.0f);
        auto OnGround = [&](FVector3& P)
        {
            if (Terrain != nullptr)
            {
                FTerrainSculptSystem::SampleHeight(*Terrain, TerrainOrigin, P.x, P.z, P.y);
            }
        };

        const ETerrainBrushMode StrokeMode = IsMomentarySmooth() ? ETerrainBrushMode::Smooth : Brush.Mode;
        const bool bInverted = bInvertSculpt != ImGui::GetIO().KeyAlt;
        ImU32 Color = GetTerrainTool(StrokeMode).Color;
        if (bInverted && (StrokeMode == ETerrainBrushMode::Sculpt || StrokeMode == ETerrainBrushMode::Noise))
        {
            Color = kLowerColor;
        }
        else if (StrokeMode == ETerrainBrushMode::Paint)
        {
            Color = kLayerSwatchColors[Math::Clamp(Brush.ActiveLayer, 0, int32(GTerrainMaxLayers) - 1)];
        }

        BrushUI::DrawRing(Draw, View, LastHit, Brush.Radius, BrushUI::ScaleColor(Color, 1.0f, 0.9f), 2.0f, OnGround);
        // The core, inside which the brush works at full strength.
        if (Brush.Falloff > 0.02f)
        {
            BrushUI::DrawRing(Draw, View, LastHit, Brush.Radius * (1.0f - Brush.Falloff), BrushUI::ScaleColor(Color, 1.0f, 0.45f), 1.0f, OnGround);
        }
        if (StrokeMode == ETerrainBrushMode::Flatten)
        {
            const float TargetY = TerrainOrigin.y + Brush.FlattenHeight;
            BrushUI::DrawRing(Draw, View, LastHit, Brush.Radius, BrushUI::ScaleColor(Color, 1.0f, 0.6f), 1.0f, [TargetY](FVector3& P) { P.y = TargetY; });
        }
        Draw->AddCircleFilled(Center, 3.0f, IM_COL32(255, 255, 255, 220));

        // What the brush will do and how strongly, shown when a modifier changes it or a shortcut just did.
        char Readout[96] = {};
        if (ReadoutSeconds > 0.0f)
        {
            ImFormatString(Readout, sizeof(Readout), "%.0f m  %.0f%%", Brush.Radius, Brush.Strength * 100.0f);
        }
        else if (StrokeMode != Brush.Mode)
        {
            ImFormatString(Readout, sizeof(Readout), "%s", GetTerrainTool(StrokeMode).Label);
        }
        else if (bInverted && (StrokeMode == ETerrainBrushMode::Sculpt || StrokeMode == ETerrainBrushMode::Noise || StrokeMode == ETerrainBrushMode::Paint))
        {
            ImFormatString(Readout, sizeof(Readout), "%s", StrokeMode == ETerrainBrushMode::Paint ? "Erase" : "Lower");
        }
        if (Readout[0] != '\0')
        {
            BrushUI::CursorLabel(Draw, Center, Readout, Color);
        }

        // Anchor, live cursor and a thin perpendicular pair showing the half-width corridor.
        if (Brush.Mode == ETerrainBrushMode::Ramp && bRampStarted)
        {
            ImVec2 Anchor;
            if (View.WorldToScreen(Brush.RampStart, Anchor))
            {
                Draw->AddCircleFilled(Anchor, 4.0f, IM_COL32( 80, 220, 255, 220));
                Draw->AddLine(Anchor, Center, IM_COL32( 80, 220, 255, 220), 2.0f);
            }

            const FVector2 AB2 = FVector2(Brush.RampEnd.x - Brush.RampStart.x, Brush.RampEnd.z - Brush.RampStart.z);
            const float Len = Math::Length(AB2);
            if (Len > 1e-3f)
            {
                const FVector2 N = FVector2(-AB2.y, AB2.x) / Len;
                const FVector3 N3 = FVector3(N.x, 0.0f, N.y) * Brush.RampHalfWidth;

                ImVec2 P0, P1;
                if (View.WorldToScreen(Brush.RampStart + N3, P0) && View.WorldToScreen(Brush.RampEnd + N3, P1))
                {
                    Draw->AddLine(P0, P1, IM_COL32( 80, 220, 255, 160), 1.0f);
                }
                if (View.WorldToScreen(Brush.RampStart - N3, P0) && View.WorldToScreen(Brush.RampEnd - N3, P1))
                {
                    Draw->AddLine(P0, P1, IM_COL32( 80, 220, 255, 160), 1.0f);
                }
            }
        }
    }
}
