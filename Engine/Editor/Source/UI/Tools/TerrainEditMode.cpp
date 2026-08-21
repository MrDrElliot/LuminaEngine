#include "TerrainEditMode.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include "Core/Math/Math.h"

#include "Platform/Process/PlatformProcess.h"
#include "Renderer/Format.h"
#include "Tools/Import/ImportHelpers.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/NameComponent.h"
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
        void BuildRayFromScreen(const SCameraComponent& Camera, ImVec2 PixelWithinViewport, ImVec2 ViewportSize, FVector3& OutOrigin, FVector3& OutDir)
        {
            const float W = Math::Max(ViewportSize.x, 1.0f);
            const float H = Math::Max(ViewportSize.y, 1.0f);

            const float Sx = (PixelWithinViewport.x / W) * 2.0f - 1.0f;
            // ImGui Y=0 is top of viewport; negate so the ray tilts up in world space (camera-basis fix in 5827d9d9).
            const float Sy = 1.0f - (PixelWithinViewport.y / H) * 2.0f;

            const FViewVolume& View    = Camera.GetViewVolume();
            const FVector3    Forward = View.GetForwardVector();
            const FVector3    Up      = View.GetUpVector();

            const FVector3    Right   = Math::Normalize(Math::Cross(Up, Forward));

            const float AspectRatio = W / H;
            const float TanHalfFov  = std::tan(Math::Radians(View.GetFOV()) * 0.5f);

            OutOrigin = Camera.GetPosition();
            OutDir    = Math::Normalize(Forward
                                       + Right * (Sx * TanHalfFov * AspectRatio)
                                       + Up    * (Sy * TanHalfFov));
        }
    }

    entt::entity FTerrainEditMode::FindPreferredTerrain(CWorld* World) const
    {
        if (!World)
        {
            return entt::null;
        }
        auto View = World->View<STerrainComponent>();
        for (auto Entity : View)
        {
            return Entity;
        }
        return entt::null;
    }

    entt::entity FTerrainEditMode::CreateDefaultTerrain(CWorld* World)
    {
        if (!World)
        {
            return entt::null;
        }

        entt::entity Entity = World->ConstructEntity("Terrain");
        STerrainComponent& Terrain = World->EmplaceComponent<STerrainComponent>(Entity);
        Terrain.Resolution      = 513;
        Terrain.ChunkResolution = 64;
        Terrain.TileWorldSize   = 4096.0f;
        Terrain.MaxHeight       = 256.0f;

        const int32 Res = Terrain.Resolution;
        const size_t N = static_cast<size_t>(Res) * static_cast<size_t>(Res);
        // Start dead flat; the user sculpts from a clean slate.
        Terrain.Heightmap.assign(N, 0.0f);

        // Weights start at zero, painting raises them, and the shader normalizes at sample time.
        static const char* LayerNames[4] = { "Layer0", "Layer1", "Layer2", "Layer3" };
        for (int i = 0; i < 4; ++i)
        {
            STerrainLayer& L = Terrain.Layers.emplace_back();
            L.Name    = LayerNames[i];
            L.UVScale = 1.0f / 16.0f;
        }

        Terrain.LayerWeights.assign(N * Terrain.Layers.size(), uint8(0));
        Terrain.CPUState.bFullHeightmapDirty = true;
        Terrain.CPUState.bFullWeightsDirty   = true;
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
        if (!Decoded.has_value() || Decoded->Pixels.empty())
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

        const size_t ElemSize  = (Kind == EKind::U8) ? 1 : (Kind == EKind::U16 ? 2 : 4);
        const size_t Stride    = (size_t)W * (size_t)Channels * ElemSize;
        if (Img.Pixels.size() < Stride * (size_t)H)
        {
            OutError = "Decoded pixel buffer is smaller than its reported dimensions.";
            return false;
        }

        const uint8* Base = Img.Pixels.data();
        auto ReadHeight = [&](int32 px, int32 py) -> float
        {
            const size_t Sample = ((size_t)py * (size_t)W + (size_t)px) * (size_t)Channels;
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

        const size_t N = (size_t)Res * (size_t)Res;
        Terrain.Heightmap.assign(N, 0.0f);

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

                Terrain.Heightmap[(size_t)ty * (size_t)Res + (size_t)tx] = Math::Clamp(Height, 0.0f, 1.0f);
            }
        }

        Terrain.MarkHeightmapReplaced();
        return true;
    }

    void FTerrainEditMode::OnEnter(CWorld* World)
    {
        // Spawning a default terrain makes the mode self-bootstrapping, with something to paint on.
        if (World && FindPreferredTerrain(World) == entt::null)
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
            CreateDefaultTerrain(World);
        }

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_IMPORT " Heightmap", ImVec2(0, ButtonSize)))
        {
            // Import a heightmap image onto the active terrain (creating a default one first if none exists).
            entt::entity TerrainEntity = FindPreferredTerrain(World);
            if (TerrainEntity == entt::null)
            {
                TerrainEntity = CreateDefaultTerrain(World);
            }

            FFixedString File;
            const char* Filter =
                "Heightmap (*.png;*.tga;*.hdr;*.jpg)\0*.png;*.tga;*.hdr;*.jpg\0"
                "PNG (*.png)\0*.png\0"
                "All Files (*.*)\0*.*\0";
            if (TerrainEntity != entt::null && Platform::OpenFileDialogue(File, "Select Heightmap Image", Filter))
            {
                STerrainComponent& Terrain = World->GetComponent<STerrainComponent>(TerrainEntity);

                // Wrap in an undo transaction so the import is a single Ctrl+Z (snapshots the pre-import heightmap).
                const bool bTransaction = (Context != nullptr);
                if (bTransaction)
                {
                    Context->BeginModeTransaction();
                }

                FString Error;
                int32 SrcW = 0;
                int32 SrcH = 0;
                const bool bOk = ImportHeightmap(Terrain, File.c_str(), Error, SrcW, SrcH);

                if (bTransaction)
                {
                    Context->EndModeTransaction("Import Heightmap");
                }

                if (bOk)
                {
                    ImGuiX::Notifications::NotifySuccess("Heightmap imported: {}x{} -> {}^2 grid.", SrcW, SrcH, Terrain.Resolution);
                }
                else
                {
                    ImGuiX::Notifications::NotifyError("Heightmap import failed: {}", Error.c_str());
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

        ImVec2 AnchorPos = ImGui::GetWindowPos();
        AnchorPos.y += ImGui::GetWindowSize().y + 4.0f;
        ImGui::SetNextWindowPos(AnchorPos);
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::Begin("##TerrainBrushSettings", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize);

        const char* ModeNames[] = { "Sculpt", "Flatten", "Smooth", "Noise", "Ramp", "Paint" };
        int ModeIndex = (int)Mode;
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::Combo("Mode", &ModeIndex, ModeNames, IM_ARRAYSIZE(ModeNames)))
        {
            Mode = (ETerrainBrushMode)ModeIndex;
            bRampStarted  = false;
            bHasStrokeHit = false;
        }

        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderFloat("Radius",   &Radius,   8.0f,  4096.0f, "%.0f");
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderFloat("Strength", &Strength, 0.0f,  1024.0f, "%.0f");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Height change rate at the brush center, in world units per second.");
        }
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderFloat("Falloff",  &Falloff,  0.0f,  1.0f,    "%.2f");

        if (Mode == ETerrainBrushMode::Sculpt || Mode == ETerrainBrushMode::Noise)
        {
            ImGui::Checkbox("Invert (LAlt momentary)", &bInvertSculpt);
        }

        if (Mode == ETerrainBrushMode::Flatten)
        {
            ImGui::SetNextItemWidth(140.0f);
            ImGui::SliderFloat("Height", &FlattenHeight, 0.0f, 1024.0f, "%.0f");
            ImGui::TextDisabled("Ctrl+Click: sample height under cursor");
        }

        if (Mode == ETerrainBrushMode::Noise)
        {
            float FreqDisplay = NoiseFrequency * 1000.0f;
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::SliderFloat("Freq (per 1km)", &FreqDisplay, 0.1f, 100.0f, "%.2f"))
            {
                NoiseFrequency = Math::Max(FreqDisplay * 0.001f, 1e-6f);
            }
            ImGui::SetNextItemWidth(140.0f);
            ImGui::SliderInt("Octaves", &NoiseOctaves, 1, 8);
        }

        if (Mode == ETerrainBrushMode::Ramp)
        {
            ImGui::SetNextItemWidth(140.0f);
            ImGui::SliderFloat("Width", &RampHalfWidth, 8.0f, 4096.0f, "%.0f");

            ImGui::Checkbox("Explicit heights", &bRampExplicitHeights);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Use fixed start/end heights instead of sampling the terrain under A and B.");
            }
            if (bRampExplicitHeights)
            {
                ImGui::SetNextItemWidth(140.0f);
                ImGui::SliderFloat("Start H", &RampStartHeight, 0.0f, 1024.0f, "%.0f");
                ImGui::SetNextItemWidth(140.0f);
                ImGui::SliderFloat("End H",   &RampEndHeight,   0.0f, 1024.0f, "%.0f");
            }
            ImGui::TextDisabled("Drag from A to B; release to commit.");
        }

        if (Mode == ETerrainBrushMode::Paint)
        {
            entt::entity TerrainEntity = FindPreferredTerrain(World);
            STerrainComponent* Terrain = (TerrainEntity != entt::null)
                ? &World->GetComponent<STerrainComponent>(TerrainEntity)
                : nullptr;
            if (Terrain)
            {
                DrawLayerPanel(*Terrain);
            }
        }

        ImGui::TextDisabled("[ / ] resize brush");

        ImGui::End();
    }

    void FTerrainEditMode::DrawLayerPanel(STerrainComponent& Terrain)
    {
        const int32 LayerCount = (int32)Terrain.Layers.size();

        ImGui::Separator();
        ImGui::TextUnformatted("Layers");

        // Capped at the shader's four-layer maximum, so the editor cannot paint a slot nothing reads.
        constexpr ImU32 SwatchColors[GTerrainMaxLayers] = {
            IM_COL32(220,  80,  80, 255),
            IM_COL32( 80, 200, 110, 255),
            IM_COL32( 80, 140, 230, 255),
            IM_COL32(220, 200,  80, 255),
        };

        const ImVec2 SwatchSize(28, 28);
        for (int32 i = 0; i < LayerCount; ++i)
        {
            ImGui::PushID(i);
            const bool bSelected = (ActiveLayer == i);
            const ImU32 Border  = bSelected ? IM_COL32(255, 255, 255, 255) : IM_COL32(80, 80, 80, 255);
            const ImU32 Fill    = SwatchColors[i % GTerrainMaxLayers];

            ImVec2 Pos = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##Swatch", SwatchSize);
            if (ImGui::IsItemClicked())
            {
                ActiveLayer = i;
            }
            ImDrawList* Dl = ImGui::GetWindowDrawList();
            Dl->AddRectFilled(Pos, ImVec2(Pos.x + SwatchSize.x, Pos.y + SwatchSize.y), Fill, 4.0f);
            Dl->AddRect(Pos, ImVec2(Pos.x + SwatchSize.x, Pos.y + SwatchSize.y), Border, 4.0f, 0, bSelected ? 2.5f : 1.0f);

            if (ImGui::IsItemHovered() && !Terrain.Layers[i].Name.empty())
            {
                ImGui::SetTooltip("%s", Terrain.Layers[i].Name.c_str());
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
                STerrainLayer& L = Terrain.Layers.emplace_back();
                L.Name = Format("Layer{}", LayerCount);
                L.UVScale = 1.0f / 16.0f;
                EnsureLayerWeightStorage(Terrain);
                Terrain.CPUState.bFullWeightsDirty = true;
            }
        }
        else
        {
            if (LayerCount > 0) ImGui::SameLine();
            ImGui::TextDisabled("(max %d)", GTerrainMaxLayers);
        }

        ActiveLayer = Math::Clamp(ActiveLayer, 0, Math::Max(LayerCount - 1, 0));

        // Reorder also mutates LayerWeights, so weights follow the layer rather than swapping under it.
        if (ActiveLayer >= 0 && ActiveLayer < LayerCount)
        {
            STerrainLayer& L = Terrain.Layers[ActiveLayer];

            char NameBuf[128];
            const FString& Cur = L.Name;
            std::strncpy(NameBuf, Cur.c_str(), sizeof(NameBuf) - 1);
            NameBuf[sizeof(NameBuf) - 1] = '\0';
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::InputText("Name", NameBuf, sizeof(NameBuf), ImGuiInputTextFlags_AutoSelectAll))
            {
                L.Name = NameBuf;
            }

            ImGui::SetNextItemWidth(140.0f);
            ImGui::SliderFloat("UV Scale", &L.UVScale, 1.0f / 1024.0f, 1.0f, "%.4f", ImGuiSliderFlags_Logarithmic);

            const bool bCanUp   = ActiveLayer > 0;
            const bool bCanDown = ActiveLayer + 1 < LayerCount;

            ImGui::BeginDisabled(!bCanUp);
            if (ImGui::Button("Up"))
            {
                SwapLayers(Terrain, ActiveLayer, ActiveLayer - 1);
                --ActiveLayer;
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(!bCanDown);
            if (ImGui::Button("Down"))
            {
                SwapLayers(Terrain, ActiveLayer, ActiveLayer + 1);
                ++ActiveLayer;
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(LayerCount <= 1);
            if (ImGui::Button("Remove"))
            {
                RemoveLayer(Terrain, ActiveLayer);
                ActiveLayer = Math::Min(ActiveLayer, (int32)Terrain.Layers.size() - 1);
            }
            ImGui::EndDisabled();
        }
    }

    void FTerrainEditMode::EnsureLayerWeightStorage(STerrainComponent& Terrain)
    {
        const size_t LayerStride = size_t(Terrain.Resolution) * size_t(Terrain.Resolution);
        const size_t Need = LayerStride * size_t(std::max<size_t>(Terrain.Layers.size(), 1));
        if (Terrain.LayerWeights.size() != Need)
        {
            Terrain.LayerWeights.resize(Need, uint8(0));
        }
    }

    void FTerrainEditMode::SwapLayers(STerrainComponent& Terrain, int32 A, int32 B)
    {
        if (A == B) return;
        const int32 LayerCount = (int32)Terrain.Layers.size();
        if (A < 0 || B < 0 || A >= LayerCount || B >= LayerCount) return;

        EnsureLayerWeightStorage(Terrain);
        std::swap(Terrain.Layers[A], Terrain.Layers[B]);

        const size_t LayerStride = size_t(Terrain.Resolution) * size_t(Terrain.Resolution);
        TVector<uint8> Tmp(LayerStride);
        uint8* PA = &Terrain.LayerWeights[size_t(A) * LayerStride];
        uint8* PB = &Terrain.LayerWeights[size_t(B) * LayerStride];
        std::memcpy(Tmp.data(), PA, LayerStride);
        std::memcpy(PA,         PB, LayerStride);
        std::memcpy(PB, Tmp.data(), LayerStride);

        Terrain.CPUState.bFullWeightsDirty = true;
    }

    void FTerrainEditMode::RemoveLayer(STerrainComponent& Terrain, int32 Index)
    {
        const int32 LayerCount = (int32)Terrain.Layers.size();
        if (Index < 0 || Index >= LayerCount || LayerCount <= 1) return;

        EnsureLayerWeightStorage(Terrain);
        const size_t LayerStride = size_t(Terrain.Resolution) * size_t(Terrain.Resolution);

        // Pack remaining layers down, then resize the trailing slot away.
        for (int32 i = Index; i + 1 < LayerCount; ++i)
        {
            uint8* Dst = &Terrain.LayerWeights[size_t(i)     * LayerStride];
            uint8* Src = &Terrain.LayerWeights[size_t(i + 1) * LayerStride];
            std::memcpy(Dst, Src, LayerStride);
        }
        Terrain.LayerWeights.resize(size_t(LayerCount - 1) * LayerStride);
        Terrain.Layers.erase(Terrain.Layers.begin() + Index);
        Terrain.CPUState.bFullWeightsDirty = true;
    }

    void FTerrainEditMode::Tick(CWorld* World, const SCameraComponent& Camera, bool bViewportHovered, ImVec2 ViewportScreenOrigin, ImVec2 ViewportSize)
    {
        bHitValid = false;

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

        entt::entity TerrainEntity = FindPreferredTerrain(World);
        if (TerrainEntity == entt::null)
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
        const FVector3 TerrainOrigin = TerrainTransform.GetWorldLocation();

        // Wired to ImGui's repeat, so holding a bracket key still scrubs the size.
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, true))
        {
            Radius = Math::Max(8.0f, Radius / 1.1f);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, true))
        {
            Radius = Math::Min(8192.0f, Radius * 1.1f);
        }

        if (!bViewportHovered)
        {
            bRampStarted  = false;
            bHasStrokeHit = false;
            return;
        }

        // Mouse position relative to the viewport image.
        const ImVec2 MousePos = ImGui::GetMousePos();
        const ImVec2 Local    = ImVec2(MousePos.x - ViewportScreenOrigin.x, MousePos.y - ViewportScreenOrigin.y);
        if (Local.x < 0.0f || Local.y < 0.0f || Local.x > ViewportSize.x || Local.y > ViewportSize.y)
        {
            return;
        }

        FVector3 RayOrigin, RayDir;
        BuildRayFromScreen(Camera, Local, ViewportSize, RayOrigin, RayDir);

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

        // On click rather than down, so a held stroke cannot accidentally retarget itself.
        if (Mode == ETerrainBrushMode::Flatten
            && ImGui::IsKeyDown(ImGuiKey_LeftCtrl)
            && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            FlattenHeight = Hit.y - TerrainOrigin.y;
            return;
        }

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            bRampStarted  = false;
            bHasStrokeHit = false;
            return;
        }

        // The host suppresses gizmo manipulation entirely here, so ImGuizmo state is stale.

        // Wall-clock delta, since the simulated tick is zero in edit mode and every dab a no-op.
        const float RealDelta = Math::Max(ImGui::GetIO().DeltaTime, 0.0f);

        // Applied every frame while held, including a stationary cursor, which is how you dig in place.

        // Ramp captures the start of the stroke once, then drags the end while held.
        if (Mode == ETerrainBrushMode::Ramp)
        {
            if (!bRampStarted)
            {
                RampStart    = Hit;
                bRampStarted = true;
            }
            RampEnd = Hit;
        }

        FTerrainSculptDab Dab;
        Dab.Mode          = Mode;
        Dab.WorldPosition = Hit;
        Dab.TerrainOrigin = TerrainOrigin;
        Dab.Radius        = Radius;
        Dab.Strength      = Strength;
        Dab.Falloff       = Falloff;
        Dab.FlattenHeight = FlattenHeight;
        Dab.DeltaSeconds  = RealDelta;
        Dab.ActiveLayer   = ActiveLayer;

        // Shift was the old modifier but is now reserved for constraint and slow-drag shortcuts.
        const bool bInvertNow = bInvertSculpt != ImGui::IsKeyDown(ImGuiKey_LeftAlt);
        Dab.SculptSign = bInvertNow ? int8(-1) : int8(1);

        Dab.NoiseFrequency = NoiseFrequency;
        Dab.NoiseOctaves   = NoiseOctaves;
        Dab.RampStart      = RampStart;
        Dab.RampEnd        = RampEnd;
        Dab.RampHalfWidth  = Math::Max(RampHalfWidth, 1.0f);
        Dab.RampUseExplicitHeights = bRampExplicitHeights;
        Dab.RampStartHeight        = RampStartHeight;
        Dab.RampEndHeight          = RampEndHeight;

        // Opened before the first edit so the snapshot captures the pre-stroke heightmap.
        if (!bTransactionOpen && Context)
        {
            Context->BeginModeTransaction();
            bTransactionOpen = true;
        }

        // Ramp re-applies the whole line each frame, while footprint brushes interpolate dabs along travel.
        if (Mode == ETerrainBrushMode::Ramp || !bHasStrokeHit)
        {
            FTerrainSculptSystem::ApplyDab(Terrain, Dab);
        }
        else
        {
            const FVector3 Delta = Hit - LastStrokeHit;
            const float Dist    = Math::Length(FVector2(Delta.x, Delta.z));
            const float Spacing = Math::Max(Radius * 0.25f, 1.0f);
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

    void FTerrainEditMode::DrawOverlay(CWorld* World, ImVec2 ViewportScreenOrigin, ImVec2 ViewportSize, const SCameraComponent& Camera)
    {
        if (!bHitValid || !World)
        {
            return;
        }

        // A world-space disk projects to an ellipse at oblique angles, so ring samples project individually.
        const FMatrix4 ViewProj = Camera.GetProjectionMatrix() * Camera.GetViewMatrix();
        auto ProjectToScreen = [&](const FVector3& W, ImVec2& Out) -> bool
        {
            FVector4 Clip = ViewProj * FVector4(W, 1.0f);
            if (Clip.w <= 0.0f)
            {
                return false;
            }
            FVector3 Ndc = FVector3(Clip) / Clip.w;
            Out.x = ViewportScreenOrigin.x + (Ndc.x * 0.5f + 0.5f) * ViewportSize.x;
            Out.y = ViewportScreenOrigin.y + (Ndc.y * 0.5f + 0.5f) * ViewportSize.y;
            return true;
        };

        ImVec2 Center;
        if (!ProjectToScreen(LastHit, Center))
        {
            return;
        }

        ImDrawList* Draw = ImGui::GetWindowDrawList();

        auto AddWorldRing = [&](float WorldRadius, ImU32 Color, float Thickness)
        {
            constexpr int Segments = 64;
            ImVec2 Points[Segments];
            int Count = 0;
            for (int i = 0; i < Segments; ++i)
            {
                const float A = (static_cast<float>(i) / static_cast<float>(Segments)) * Math::TwoPi<float>();
                const FVector3 P = LastHit + FVector3(std::cos(A) * WorldRadius, 0.0f, std::sin(A) * WorldRadius);
                ImVec2 S;
                if (ProjectToScreen(P, S))
                {
                    Points[Count++] = S;
                }
            }
            if (Count >= 2)
            {
                Draw->AddPolyline(Points, Count, Color, ImDrawFlags_Closed, Thickness);
            }
        };

        AddWorldRing(Radius, IM_COL32(255, 220,  80, 220), 2.0f);
        AddWorldRing(Radius * Math::Max(0.0f, 1.0f - Falloff * 0.5f),  IM_COL32(255, 220,  80, 120), 1.0f);
        Draw->AddCircleFilled(Center, 3.0f, IM_COL32(255, 255, 255, 220));

        // Anchor, live cursor and a thin perpendicular pair showing the half-width corridor.
        if (Mode == ETerrainBrushMode::Ramp && bRampStarted)
        {
            ImVec2 Anchor;
            if (ProjectToScreen(RampStart, Anchor))
            {
                Draw->AddCircleFilled(Anchor, 4.0f, IM_COL32( 80, 220, 255, 220));
                Draw->AddLine(Anchor, Center, IM_COL32( 80, 220, 255, 220), 2.0f);
            }

            const FVector2 AB2 = FVector2(RampEnd.x - RampStart.x, RampEnd.z - RampStart.z);
            const float Len = Math::Length(AB2);
            if (Len > 1e-3f)
            {
                const FVector2 N = FVector2(-AB2.y, AB2.x) / Len;
                const FVector3 N3 = FVector3(N.x, 0.0f, N.y) * RampHalfWidth;

                ImVec2 P0, P1;
                if (ProjectToScreen(RampStart + N3, P0) && ProjectToScreen(RampEnd + N3, P1))
                {
                    Draw->AddLine(P0, P1, IM_COL32( 80, 220, 255, 160), 1.0f);
                }
                if (ProjectToScreen(RampStart - N3, P0) && ProjectToScreen(RampEnd - N3, P1))
                {
                    Draw->AddLine(P0, P1, IM_COL32( 80, 220, 255, 160), 1.0f);
                }
            }
        }
    }
}
