#include "FoliageEditMode.h"
#include "World/Scene/RenderScene/ScenePrimitiveSet.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include "Core/Math/Math.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Tools/UI/ImGui/ImGuiDragDrop.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/FoliageComponent.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Subsystems/TerrainSculptSystem.h"
#include "World/World.h"

namespace Lumina
{
    namespace
    {
        void BuildRayFromScreen(const SCameraComponent& Camera, ImVec2 PixelWithinViewport, ImVec2 ViewportSize, FVector3& OutOrigin, FVector3& OutDir)
        {
            const float W = std::max(ViewportSize.x, 1.0f);
            const float H = std::max(ViewportSize.y, 1.0f);

            const float Sx = (PixelWithinViewport.x / W) * 2.0f - 1.0f;
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

        bool RaycastGroundPlane(const FVector3& Origin, const FVector3& Dir, FVector3& OutHit)
        {
            if (std::abs(Dir.y) < 1e-6f)
            {
                return false;
            }
            const float T = -Origin.y / Dir.y;
            if (T <= 0.0f)
            {
                return false;
            }
            OutHit = Origin + Dir * T;
            return true;
        }

        // Shortest-arc rotation taking From onto To (both assumed unit length).
        FQuat QuatFromTo(const FVector3& From, const FVector3& To)
        {
            const float D = Math::Clamp(Math::Dot(From, To), -1.0f, 1.0f);
            if (D > 0.9999f)
            {
                return FQuat::Identity();
            }
            if (D < -0.9999f)
            {
                // Opposite: rotate 180 about any orthogonal axis.
                FVector3 Axis = Math::Cross(FVector3(1.0f, 0.0f, 0.0f), From);
                if (Math::Length(Axis) < 1e-3f)
                {
                    Axis = Math::Cross(FVector3(0.0f, 0.0f, 1.0f), From);
                }
                return Math::FromAxisAngle(Math::Normalize(Axis), LE_PI_F);
            }
            const FVector3 Axis = Math::Normalize(Math::Cross(From, To));
            return Math::FromAxisAngle(Axis, std::acos(D));
        }
    }

    float FFoliageEditMode::RandFloat()
    {
        RngState ^= RngState << 13;
        RngState ^= RngState >> 17;
        RngState ^= RngState << 5;
        return float(RngState & 0xFFFFFFu) / float(0x1000000);
    }

    SFoliageComponent* FFoliageEditMode::FindFoliage(CWorld* World) const
    {
        if (!World)
        {
            return nullptr;
        }
        auto View = World->View<SFoliageComponent>();
        for (auto Entity : View)
        {
            return &View.get<SFoliageComponent>(Entity);
        }
        return nullptr;
    }

    SFoliageComponent* FFoliageEditMode::FindOrCreateFoliage(CWorld* World, entt::entity* OutEntity)
    {
        if (!World)
        {
            return nullptr;
        }

        auto View = World->View<SFoliageComponent>();
        for (auto Entity : View)
        {
            if (OutEntity != nullptr)
            {
                *OutEntity = Entity;
            }
            return &View.get<SFoliageComponent>(Entity);
        }

        entt::entity Entity = World->ConstructEntity("Foliage");
        if (OutEntity != nullptr)
        {
            *OutEntity = Entity;
        }
        return &World->EmplaceComponent<SFoliageComponent>(Entity);
    }

    void FFoliageEditMode::MarkFoliageDirty(CWorld* World, SFoliageComponent& Foliage)
    {
        if (World == nullptr)
        {
            return;
        }

        entt::entity Entity = entt::null;
        if (FindOrCreateFoliage(World, &Entity) != nullptr && Entity != entt::null)
        {
            MarkFoliageChanged(*World, Entity, Foliage);
        }
    }

    STerrainComponent* FFoliageEditMode::FindTerrain(CWorld* World, FVector3& OutOrigin) const
    {
        if (!World)
        {
            return nullptr;
        }
        auto View = World->View<STerrainComponent, STransformComponent>();
        for (auto Entity : View)
        {
            OutOrigin = View.get<STransformComponent>(Entity).GetWorldLocation();
            return &View.get<STerrainComponent>(Entity);
        }
        return nullptr;
    }

    bool FFoliageEditMode::ProjectToSurface(CWorld* World, STerrainComponent* Terrain, const FVector3& TerrainOrigin, float WorldX, float WorldZ, FVector3& OutPos, FVector3& OutNormal) const
    {
        (void)World;
        if (Terrain)
        {
            float Height = 0.0f;
            if (FTerrainSculptSystem::SampleHeight(*Terrain, TerrainOrigin, WorldX, WorldZ, Height))
            {
                OutPos    = FVector3(WorldX, Height, WorldZ);
                OutNormal = FTerrainSculptSystem::SampleNormal(*Terrain, TerrainOrigin, WorldX, WorldZ);
                return true;
            }
            return false; // off the terrain footprint
        }

        OutPos    = FVector3(WorldX, 0.0f, WorldZ);
        OutNormal = FVector3(0.0f, 1.0f, 0.0f);
        return true;
    }

    void FFoliageEditMode::PaintDab(CWorld* World, SFoliageComponent& Foliage, STerrainComponent* Terrain, const FVector3& TerrainOrigin, const FVector3& Center, float DeltaSeconds)
    {
        if (!Foliage.IsValidType(ActiveType))
        {
            return;
        }
        const SFoliageType& Type = Foliage.Types[ActiveType];
        if (!Type.Mesh.IsValid())
        {
            return;
        }

        // Budget attempts by density * brush area * time, accumulating fractional leftovers so even sparse
        // densities place instances over a stroke. Density is instances per (100 unit)^2 per second.
        const float AreaUnits   = Math::Pi<float>() * Radius * Radius;
        const float TargetPerSec = Type.Density * (AreaUnits / 10000.0f);
        PaintAccumulator += TargetPerSec * DeltaSeconds;

        int32 Attempts = (int32)PaintAccumulator;
        Attempts = std::min(Attempts, 256);
        PaintAccumulator -= (float)Attempts;
        if (Attempts <= 0)
        {
            return;
        }

        const FVector3 WorldUp(0.0f, 1.0f, 0.0f);

        for (int32 i = 0; i < Attempts; ++i)
        {
            // Uniform point in the brush disk, with falloff biasing density toward the center.
            const float Angle = RandFloat() * Math::TwoPi<float>();
            float       R     = Radius * std::sqrt(RandFloat());
            if (RandFloat() > (1.0f - Falloff) + Falloff * (1.0f - R / Radius))
            {
                continue;
            }
            const float Px = Center.x + std::cos(Angle) * R;
            const float Pz = Center.z + std::sin(Angle) * R;

            FVector3 SurfacePos, SurfaceNormal;
            if (!ProjectToSurface(World, Terrain, TerrainOrigin, Px, Pz, SurfacePos, SurfaceNormal))
            {
                continue;
            }

            // Orientation: blend up toward the surface normal, then a random yaw about that axis.
            const FVector3 AlignedUp = Math::Normalize(Math::Mix(WorldUp, SurfaceNormal, Math::Clamp(Type.AlignToNormal, 0.0f, 1.0f)));
            FQuat Rotation = QuatFromTo(WorldUp, AlignedUp);
            if (Type.bRandomYaw)
            {
                Rotation = Math::FromAxisAngle(AlignedUp, RandFloat() * Math::TwoPi<float>()) * Rotation;
            }

            const float ScaleT = RandFloat();
            const float Scale  = Math::Mix(Type.ScaleMin, Type.ScaleMax, ScaleT);

            SFoliageInstance Inst;
            Inst.Position  = SurfacePos + AlignedUp * Type.ZOffset;
            Inst.Scale     = FVector3(Scale);
            Inst.TypeIndex = ActiveType;
            Inst.SetRotationQuat(Rotation);

            Foliage.AddInstance(Inst);
        }
    }

    void FFoliageEditMode::EraseDab(SFoliageComponent& Foliage, const FVector3& Center)
    {
        // Erase only the active type, so brushing doesn't wipe other species the user wants kept.
        const int32 Filter = Foliage.IsValidType(ActiveType) ? ActiveType : -1;
        Foliage.RemoveInRadius(Center, Radius, Filter);
    }

    void FFoliageEditMode::OnEnter(CWorld* World)
    {
        // Make sure there's a foliage component to paint into.
        FindOrCreateFoliage(World);
    }

    void FFoliageEditMode::OnExit(CWorld* World)
    {
        if (bTransactionOpen && Context)
        {
            Context->EndModeTransaction("Foliage Edit");
        }
        bTransactionOpen = false;
        bHitValid        = false;
        PaintAccumulator = 0.0f;
    }

    void FFoliageEditMode::DrawToolbar(CWorld* World, float ButtonSize)
    {
        if (!World)
        {
            return;
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
        // Fixed width, auto height: the type settings below are a real property table, and an
        // AlwaysAutoResize window would size itself to the widest row and jump about as rows change.
        //
        // The width is set by the asset picker, not by taste. The property table gives its name column a
        // fixed width (widest header + 18) and stretches the value column, and the picker then spends 64px
        // of that on the thumbnail before the path field and dropdown get any. At 330 the field and its
        // buttons fell off the edge; this leaves the value column around 290.
        constexpr float PanelWidth = 460.0f;
        ImGui::SetNextWindowSizeConstraints(ImVec2(PanelWidth, 0.0f), ImVec2(PanelWidth, FLT_MAX));
        ImGui::Begin("##FoliageBrushSettings", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::SeparatorText("Brush");

        // Paint / Erase as a pair of toggles rather than a dropdown: it is the control switched most
        // often, and a combo costs two clicks and hides the current state behind a label.
        const float HalfWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        const bool  bPainting = (Mode == EFoliageBrushMode::Paint);

        ImGui::PushStyleColor(ImGuiCol_Button, bPainting ? IM_COL32(70, 130, 80, 255) : ImGui::GetColorU32(ImGuiCol_Button));
        if (ImGui::Button(LE_ICON_BRUSH" Paint", ImVec2(HalfWidth, 0.0f)))
        {
            Mode = EFoliageBrushMode::Paint;
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, !bPainting ? IM_COL32(150, 70, 70, 255) : ImGui::GetColorU32(ImGuiCol_Button));
        if (ImGui::Button(LE_ICON_ERASER" Erase", ImVec2(HalfWidth, 0.0f)))
        {
            Mode = EFoliageBrushMode::Erase;
        }
        ImGui::PopStyleColor();

        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##Radius",  &Radius,  16.0f, 4096.0f, "Radius  %.0f");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##Falloff", &Falloff, 0.0f, 1.0f, "Falloff  %.2f");

        if (SFoliageComponent* Foliage = FindOrCreateFoliage(World))
        {
            DrawTypePanel(World, *Foliage);
        }

        ImGui::TextDisabled("[ / ] resize brush");
        ImGui::End();
    }

    void FFoliageEditMode::RebindTypeSettings(CWorld* World, SFoliageComponent& Foliage)
    {
        void* Desired = Foliage.IsValidType(ActiveType) ? (void*)&Foliage.Types[ActiveType] : nullptr;
        if (Desired == BoundTypeData)
        {
            return;
        }

        BoundTypeData = Desired;
        if (Desired != nullptr)
        {
            TypeSettings.SetObject(Desired, SFoliageType::StaticStruct());
            TypeSettings.SetShowSearchBar(false);   // a search box inside a viewport overlay is noise

            // Any edit can invalidate the render cache -- swapping the mesh changes the bounds every
            // instance of this type was baked with. Cheap: the bake is skipped when the version matches,
            // so bumping on an edit that did not need it costs one rebuild, while missing one leaves
            // instances culled against the previous mesh's bounds.
            TypeSettings.SetPostEditCallback([this, World, &Foliage](const FPropertyChangedEvent&)
            {
                MarkFoliageDirty(World, Foliage);
            });
        }
        else
        {
            TypeSettings.SetObject(nullptr, nullptr);
        }
    }

    void FFoliageEditMode::DrawTypePanel(CWorld* World, SFoliageComponent& Foliage)
    {
        (void)World;

        ImGui::SeparatorText("Foliage Types");

        const int32 TypeCount = (int32)Foliage.Types.size();

        // Instance counts, in one pass rather than a scan per type. Painted counts are the thing you
        // actually want to see while working, and this is the only place they are visible.
        TVector<int32> Counts;
        Counts.resize((size_t)Math::Max(TypeCount, 1), 0);
        for (const SFoliageInstance& Inst : Foliage.Instances)
        {
            if (Inst.TypeIndex >= 0 && Inst.TypeIndex < TypeCount)
            {
                ++Counts[(size_t)Inst.TypeIndex];
            }
        }

        const float RowHeight = ImGui::GetTextLineHeightWithSpacing();
        const float ListHeight = Math::Clamp(RowHeight * (float)Math::Max(TypeCount, 1) + 8.0f, RowHeight + 8.0f, RowHeight * 6.0f);
        ImGui::BeginChild("##foliagetypes", ImVec2(-FLT_MIN, ListHeight), ImGuiChildFlags_Borders);

        if (TypeCount == 0)
        {
            ImGui::TextDisabled("No foliage types yet.");
        }

        for (int32 i = 0; i < TypeCount; ++i)
        {
            SFoliageType& Type = Foliage.Types[i];
            ImGui::PushID(i);

            // A type with no mesh cannot paint anything, so it is called out rather than looking ready.
            const bool bPaintable = Type.Mesh.IsValid();
            FString Label = Type.Name.empty() ? FString("Unnamed") : Type.Name;
            if (bPaintable)
            {
                Label += "  (";
                Label += Type.Mesh->GetName().c_str();
                Label += ")";
            }
            else
            {
                Label += "  (no mesh)";
            }

            ImGui::PushStyleColor(ImGuiCol_Text, bPaintable ? IM_COL32(225, 225, 230, 255)
                                                            : IM_COL32(210, 150, 110, 255));
            if (ImGui::Selectable(Label.c_str(), ActiveType == i))
            {
                ActiveType = i;
            }
            ImGui::PopStyleColor();

            // Right-aligned instance count.
            const FString CountText = eastl::to_string(Counts[(size_t)i]).c_str();
            const float   CountW    = ImGui::CalcTextSize(CountText.c_str()).x;
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - CountW);
            ImGui::TextDisabled("%s", CountText.c_str());

            ImGui::PopID();
        }

        ImGui::EndChild();

        if (ImGui::Button("Add"))
        {
            SFoliageType& NewType = Foliage.Types.emplace_back();
            NewType.Name.sprintf("Type%d", TypeCount);
            ActiveType = (int32)Foliage.Types.size() - 1;
            MarkFoliageDirty(World, Foliage);
        }
        ImGui::SameLine();

        ImGui::BeginDisabled(!Foliage.IsValidType(ActiveType));
        if (ImGui::Button("Duplicate"))
        {
            // Copied by value before the push_back: emplace_back can reallocate, and a reference into the
            // vector would dangle exactly when it is about to be read.
            const SFoliageType Source = Foliage.Types[ActiveType];
            Foliage.Types.push_back(Source);
            Foliage.Types.back().Name = Source.Name + " Copy";
            ActiveType = (int32)Foliage.Types.size() - 1;
            // Same reason as Add and Remove: the copy carries an unstamped resolve, so without this it
            // paints instances that never bind a mesh.
            MarkFoliageDirty(World, Foliage);
        }
        ImGui::SameLine();

        if (ImGui::Button("Remove"))
        {
            Foliage.Instances.erase(
                std::remove_if(Foliage.Instances.begin(), Foliage.Instances.end(),
                    [this](const SFoliageInstance& Inst) { return Inst.TypeIndex == ActiveType; }),
                Foliage.Instances.end());
            // Re-index instances that pointed past the removed type.
            for (SFoliageInstance& Inst : Foliage.Instances)
            {
                if (Inst.TypeIndex > ActiveType)
                {
                    --Inst.TypeIndex;
                }
            }
            Foliage.Types.erase(Foliage.Types.begin() + ActiveType);
            ActiveType = std::min(ActiveType, (int32)Foliage.Types.size() - 1);
            // Instances were removed and the rest re-indexed; without this the render cache keeps drawing
            // the deleted type's instances until some other edit happens to bump the version.
            MarkFoliageDirty(World, Foliage);
        }
        ImGui::EndDisabled();

        ActiveType = std::clamp(ActiveType, 0, std::max((int32)Foliage.Types.size() - 1, 0));
        RebindTypeSettings(World, Foliage);

        if (!Foliage.IsValidType(ActiveType))
        {
            ImGui::TextDisabled("Add a type to start painting.");
            return;
        }

        ImGui::SeparatorText("Type Settings");
        TypeSettings.DrawTree();
    }

    void FFoliageEditMode::Tick(CWorld* World, const SCameraComponent& Camera, bool bViewportHovered, ImVec2 ViewportScreenOrigin, ImVec2 ViewportSize)
    {
        bHitValid = false;

        // Close a stroke transaction the moment the button is released.
        if (bTransactionOpen && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            if (Context)
            {
                Context->EndModeTransaction("Foliage Edit");
            }
            bTransactionOpen = false;
            PaintAccumulator = 0.0f;
        }

        if (!World || !bViewportHovered)
        {
            return;
        }

        // Bracket keys resize the brush.
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, true))  { Radius = std::max(16.0f, Radius / 1.1f); }
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, true)) { Radius = std::min(8192.0f, Radius * 1.1f); }

        const ImVec2 MousePos = ImGui::GetMousePos();
        const ImVec2 Local    = ImVec2(MousePos.x - ViewportScreenOrigin.x, MousePos.y - ViewportScreenOrigin.y);
        if (Local.x < 0.0f || Local.y < 0.0f || Local.x > ViewportSize.x || Local.y > ViewportSize.y)
        {
            return;
        }

        FVector3 RayOrigin, RayDir;
        BuildRayFromScreen(Camera, Local, ViewportSize, RayOrigin, RayDir);

        FVector3 TerrainOrigin(0.0f);
        STerrainComponent* Terrain = FindTerrain(World, TerrainOrigin);

        // Cursor hit: prefer the terrain surface, fall back to the ground plane.
        FVector3 Hit;
        bool bHit = false;
        if (Terrain && FTerrainSculptSystem::Raycast(*Terrain, TerrainOrigin, RayOrigin, RayDir, Hit))
        {
            bHit = true;
        }
        else if (RaycastGroundPlane(RayOrigin, RayDir, Hit))
        {
            bHit = true;
        }

        if (!bHit)
        {
            return;
        }

        LastHit   = Hit;
        bHitValid = true;

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            return;
        }

        entt::entity FoliageEntity = entt::null;
        SFoliageComponent* Foliage = FindOrCreateFoliage(World, &FoliageEntity);
        if (!Foliage)
        {
            return;
        }

        if (!bTransactionOpen && Context)
        {
            Context->BeginModeTransaction();
            bTransactionOpen = true;
        }

        const float RealDelta = std::max(ImGui::GetIO().DeltaTime, 1.0f / 240.0f);

        const uint32 VersionBefore = Foliage->InstancesVersion;

        if (Mode == EFoliageBrushMode::Paint)
        {
            PaintDab(World, *Foliage, Terrain, TerrainOrigin, Hit, RealDelta);
        }
        else
        {
            EraseDab(*Foliage, Hit);
        }

        // AddInstance/RemoveInRadius bump InstancesVersion themselves, which only rebakes the CPU cache --
        // the primitive set still has to be told, or the instances are baked and never drawn. Gated on the
        // version so a dab that placed nothing (density budget not yet accumulated, nothing in erase range)
        // does not queue a resync every frame the button is held.
        if (Foliage->InstancesVersion != VersionBefore && FoliageEntity != entt::null)
        {
            MarkFoliageChanged(*World, FoliageEntity, *Foliage);
        }
    }

    void FFoliageEditMode::DrawOverlay(CWorld* World, ImVec2 ViewportScreenOrigin, ImVec2 ViewportSize, const SCameraComponent& Camera)
    {
        if (!bHitValid || !World)
        {
            return;
        }

        const FMatrix4 ViewProj = Camera.GetProjectionMatrix() * Camera.GetViewMatrix();
        auto ProjectToScreen = [&](const FVector3& WorldPos, ImVec2& Out) -> bool
        {
            FVector4 Clip = ViewProj * FVector4(WorldPos, 1.0f);
            if (Clip.w <= 0.0f)
            {
                return false;
            }
            FVector3 Ndc = FVector3(Clip) / Clip.w;
            Out.x = ViewportScreenOrigin.x + (Ndc.x * 0.5f + 0.5f) * ViewportSize.x;
            Out.y = ViewportScreenOrigin.y + (Ndc.y * 0.5f + 0.5f) * ViewportSize.y;
            return true;
        };

        ImDrawList* Draw = ImGui::GetWindowDrawList();
        const ImU32 Color = (Mode == EFoliageBrushMode::Erase) ? IM_COL32(230, 120, 120, 220) : IM_COL32(120, 230, 120, 220);

        auto AddWorldRing = [&](float WorldRadius, ImU32 RingColor, float Thickness)
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
                Draw->AddPolyline(Points, Count, RingColor, ImDrawFlags_Closed, Thickness);
            }
        };

        ImVec2 Center;
        const bool bCenter = ProjectToScreen(LastHit, Center);
        AddWorldRing(Radius, Color, 2.0f);
        if (bCenter)
        {
            Draw->AddCircleFilled(Center, 3.0f, IM_COL32(255, 255, 255, 220));
        }
    }
}
