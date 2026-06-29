#include "NavMeshEditMode.h"

#include "AI/Navigation/NavMesh.h"
#include "Core/Console/ConsoleVariable.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "World/Entity/Components/NavMeshComponent.h"
#include "World/World.h"

namespace Lumina
{
    namespace
    {
        const FVector4 kVolumeColor(0.95f, 0.65f, 0.10f, 1.0f);
        const FVector4 kBakedColor (0.30f, 0.85f, 0.45f, 1.0f);
        const FVector4 kBakingColor(0.85f, 0.85f, 0.20f, 1.0f);
    }

    void FNavigationEditMode::DrawOverlay(CWorld* World, ImVec2, ImVec2, const SCameraComponent&)
    {
        if (!World)
        {
            return;
        }

        auto View = World->View<SNavMeshComponent>();
        for (entt::entity Entity : View)
        {
            const SNavMeshComponent& Nav = View.get<SNavMeshComponent>(Entity);
            const bool bBaking = Nav.Runtime.State == ENavBakeState::Building || Nav.Runtime.State == ENavBakeState::Initializing;
            const FVector4 Color = bBaking ? kBakingColor : (Nav.HasBakedData() ? kBakedColor : kVolumeColor);
            // Duration -1 = single frame; the overlay re-emits each tick so the wireframe is always present.
            World->DrawBox(Nav.Center, Nav.GetWorldExtents(), FQuat(1.0f, 0.0f, 0.0f, 0.0f), Color, 1.5f, true, -1.0f);
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

        // Manual rebuild: forces every nav volume to re-bake (use when world geometry changed but the
        // bounds did not). Routine placement/move re-bakes automatically via SNavMeshComponent::bAutoBake.
        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_REFRESH, ImVec2(ButtonSize, ButtonSize)))
        {
            auto View = World->View<SNavMeshComponent>();
            for (entt::entity Entity : View)
            {
                View.get<SNavMeshComponent>(Entity).bBakeRequested = true;
            }
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Rebuild navigation");
        }

        // Compact state readout for the first nav volume.
        auto View = World->View<SNavMeshComponent>();
        for (entt::entity Entity : View)
        {
            const SNavMeshComponent& Nav = View.get<SNavMeshComponent>(Entity);
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
                ImGui::TextDisabled("%d tiles | %d tris", Stats.LoadedTiles, Stats.Triangles);
            }
            break;
        }
    }
}
