#include "NavMeshEditMode.h"
#include "World/ECS/Registry.h"

#include "AI/Navigation/NavMesh.h"
#include "Core/Console/ConsoleVariable.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "World/Entity/Components/NavLinkComponent.h"
#include "World/Entity/Components/NavMeshComponent.h"
#include "World/Entity/Components/NavModifierComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/World.h"

namespace Lumina
{
    namespace
    {
        const FVector4 kVolumeColor(0.95f, 0.65f, 0.10f, 1.0f);
        const FVector4 kBakedColor (0.30f, 0.85f, 0.45f, 1.0f);
        const FVector4 kBakingColor(0.85f, 0.85f, 0.20f, 1.0f);
        const FVector4 kLinkColor  (1.00f, 0.30f, 0.95f, 1.0f);

        // Mirrors the nav debug palette so a modifier reads as the area it stamps.
        FVector4 ModifierColor(ENavArea Area)
        {
            switch (Area)
            {
                case ENavArea::Ground: return FVector4(0.20f, 0.85f, 0.30f, 1.0f);
                case ENavArea::Water:  return FVector4(0.20f, 0.45f, 0.95f, 1.0f);
                case ENavArea::Door:   return FVector4(0.95f, 0.65f, 0.15f, 1.0f);
                case ENavArea::Danger: return FVector4(0.95f, 0.20f, 0.20f, 1.0f);
                case ENavArea::Null:   return FVector4(0.85f, 0.25f, 0.25f, 1.0f);
                default:               return FVector4(0.75f, 0.30f, 0.95f, 1.0f);
            }
        }
    }

    void FNavigationEditMode::DrawOverlay(CWorld* World, ImVec2, ImVec2, const SCameraComponent&)
    {
        if (!World)
        {
            return;
        }

        auto View = World->View<SNavMeshComponent>();
        for (ECS::FEntity Entity : View)
        {
            const SNavMeshComponent& Nav = View.Get<SNavMeshComponent>(Entity);
            const bool bBaking = Nav.Runtime.State == ENavBakeState::Building || Nav.Runtime.State == ENavBakeState::Initializing;
            const FVector4 Color = bBaking ? kBakingColor : (Nav.HasBakedData() ? kBakedColor : kVolumeColor);
            // Duration -1 = single frame; the overlay re-emits each tick so the wireframe is always present.
            World->DrawBox(Nav.Center, Nav.GetWorldExtents(), FQuat(1.0f, 0.0f, 0.0f, 0.0f), Color, 1.5f, true, -1.0f);
        }

        // Authoring aids, so a volume or link is visible while it is being placed rather than only after a bake.
        auto ModifierView = World->View<SNavModifierComponent, STransformComponent>();
        for (ECS::FEntity Entity : ModifierView)
        {
            const SNavModifierComponent& Modifier = ModifierView.Get<SNavModifierComponent>(Entity);
            if (!Modifier.bEnabled)
            {
                continue;
            }

            const FTransform& WT = ModifierView.Get<STransformComponent>(Entity).GetWorldTransform();
            const FVector3 Scale = WT.GetScale();
            const FVector3 Center = WT.GetLocation() + Math::Rotate(WT.GetRotation(), Modifier.Offset * Scale);
            const FVector3 Half(Modifier.Extents.x * Math::Abs(Scale.x), Modifier.Extents.y * Math::Abs(Scale.y), Modifier.Extents.z * Math::Abs(Scale.z));
            World->DrawBox(Center, Half, WT.GetRotation(), ModifierColor(Modifier.Area), 1.5f, true, -1.0f);
        }

        auto LinkView = World->View<SNavLinkComponent, STransformComponent>();
        for (ECS::FEntity Entity : LinkView)
        {
            const SNavLinkComponent& Link = LinkView.Get<SNavLinkComponent>(Entity);
            if (!Link.bEnabled)
            {
                continue;
            }

            const FMatrix4 World4 = LinkView.Get<STransformComponent>(Entity).GetWorldMatrix();
            const FVector3 Start = FVector3(World4 * FVector4(Link.Start, 1.0f));
            const FVector3 End   = FVector3(World4 * FVector4(Link.End,   1.0f));
            World->DrawSphere(Start, Link.Radius, kLinkColor, 12, 1.5f, true, -1.0f);
            World->DrawSphere(End,   Link.Radius, kLinkColor, 12, 1.5f, true, -1.0f);

            const FVector3 Delta = End - Start;
            const float Length = Math::Length(Delta);
            if (Length > Math::kSmallNumber)
            {
                World->DrawArrow(Start, Delta / Length, Length, kLinkColor, 2.0f, true, -1.0f, 0.25f);
                if (!Link.bBidirectional)
                {
                    continue;
                }
                World->DrawArrow(End, -Delta / Length, Length, kLinkColor, 2.0f, true, -1.0f, 0.25f);
            }
        }
    }

    void FNavigationEditMode::DrawToolbar(CWorld* World, float ButtonSize)
    {
        if (!World)
        {
            return;
        }

        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        // Toggle the navmesh overlay (same CVar the view-mode menu uses).
        if (const bool* bDraw = FConsoleRegistry::Get().TryGetAs<bool>("Nav.DrawDebug"))
        {
            const bool bShow = *bDraw;
            if (ImGui::Button(bShow ? LE_ICON_EYE : LE_ICON_EYE_OFF, ImVec2(ButtonSize, ButtonSize)))
            {
                FConsoleRegistry::Get().SetAs("Nav.DrawDebug", !bShow);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(bShow ? "Hide nav-mesh overlay" : "Show nav-mesh overlay");
            }
        }

        // Routine placement re-bakes automatically, so this is for when world geometry changed.
        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_REFRESH, ImVec2(ButtonSize, ButtonSize)))
        {
            auto View = World->View<SNavMeshComponent>();
            for (ECS::FEntity Entity : View)
            {
                View.Get<SNavMeshComponent>(Entity).bBakeRequested = true;
            }
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Rebuild navigation");
        }

        // Compact state readout for the first nav volume.
        auto View = World->View<SNavMeshComponent>();
        for (ECS::FEntity Entity : View)
        {
            const SNavMeshComponent& Nav = View.Get<SNavMeshComponent>(Entity);
            const bool bBaking = Nav.Runtime.State == ENavBakeState::Building || Nav.Runtime.State == ENavBakeState::Initializing;
            ImGui::SameLine();
            if (bBaking)
            {
                ImGui::TextColored(ImVec4(kBakingColor.x, kBakingColor.y, kBakingColor.z, 1.0f), "Baking...");
            }
            else if (Nav.Runtime.State == ENavBakeState::Failed)
            {
                ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "bake failed");
            }
            else if (Nav.Runtime.Mesh && Nav.Runtime.Mesh->IsReady())
            {
                const FNavDebugStats Stats = Nav.Runtime.Mesh->GetDebugStats();
                if (Stats.OffMeshLinks > 0)
                {
                    ImGui::TextDisabled("%d tiles | %d tris | %d links", Stats.LoadedTiles, Stats.Triangles, Stats.OffMeshLinks);
                }
                else
                {
                    ImGui::TextDisabled("%d tiles | %d tris", Stats.LoadedTiles, Stats.Triangles);
                }
            }
            break;
        }
    }
}
