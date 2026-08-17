#include "SkeletalMeshEditorTool.h"

#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Core/Object/Cast.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include <UI/Tools/AssetEditors/AssetEditorTool.h>
#include <UI/Tools/EditorTool.h>
#include <Lumina.h>
#include <Containers/Array.h>
#include <Containers/String.h>
#include <Core/Math/AABB.h>
#include <Core/Math/Color.h>
#include <Core/Math/Math.h>
#include <Core/Math/Transform.h>
#include <Core/Object/Object.h>
#include <Core/Object/ObjectCore.h>
#include <Platform/GenericPlatform.h>
#include <Renderer/MeshData.h>
#include <Renderer/MeshQuantization.h>
#include <Renderer/Vertex.h>
#include <Tools/UI/ImGui/ImGuiDesignIcons.h>
#include <World/Entity/Components/TransformComponent.h>
#include <World/World.h>
#include <EASTL/string.h>
#include <imgui.h>
#include <ImGuizmo.h>
#include <imgui_internal.h>
#include "Renderer/ImmediateLineRenderer.h"
#include "Renderer/SkeletonResource.h"


namespace Lumina
{
    // Bounded by Meshlets.size() so a malformed asset can't overrun during UI rendering.
    static uint32 SumSkinnedTrianglesInRange(const TVector<FMeshlet>& Meshlets, uint32 Offset, uint32 Count)
    {
        uint32 Tris = 0;
        const uint32 End = Math::Min(Offset + Count, (uint32)Meshlets.size());
        for (uint32 m = Offset; m < End; ++m)
        {
            Tris += Meshlets[m].TriangleCount;
        }
        return Tris;
    }

    static FORCEINLINE uint32 JointAt(uint32 Packed, uint32 Slot) { return (Packed >> (Slot * 8u)) & 0xFFu; }

    // Walks each surface's LOD 0 meshlet range and hands every vertex to Visit(Meshlet, Vertex).
    template <typename TVisitor>
    static void ForEachLOD0SkinnedVertex(const FMeshResource& Resource, TVisitor&& Visit)
    {
        const FMeshletData& MD = Resource.MeshletData;

        for (const FGeometrySurface& Surface : Resource.GeometrySurfaces)
        {
            const uint32 End = Math::Min(Surface.LODMeshletOffset[0] + Surface.LODMeshletCount[0],
                                         (uint32)MD.Meshlets.size());

            for (uint32 m = Surface.LODMeshletOffset[0]; m < End; ++m)
            {
                const FMeshlet& Meshlet = MD.Meshlets[m];
                const uint32 VertEnd = Math::Min(Meshlet.VertexOffset + Meshlet.VertexCount,
                                                 (uint32)MD.MeshletSkinnedVertices.size());

                for (uint32 v = Meshlet.VertexOffset; v < VertEnd; ++v)
                {
                    Visit(Meshlet, MD.MeshletSkinnedVertices[v]);
                }
            }
        }
    }

    FSkeletalMeshEditorTool::FSkeletalMeshEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset, NewObject<CWorld>())
    {
    }

    void FSkeletalMeshEditorTool::RebuildSkinningAnalysis()
    {
        CSkeletalMesh* SkeletalMesh = Cast<CSkeletalMesh>(Asset.Get());
        if (SkeletalMesh == nullptr)
        {
            Skinning = FSkinningAnalysis();
            return;
        }

        CSkeleton* Skeleton = SkeletalMesh->Skeleton.Get();

        const bool bStillCurrent = Skinning.bValid
                                && Skinning.AnalyzedMesh == SkeletalMesh
                                && Skinning.AnalyzedSkeleton == Skeleton;
        if (bStillCurrent)
        {
            return;
        }

        Skinning = FSkinningAnalysis();
        Skinning.AnalyzedMesh     = SkeletalMesh;
        Skinning.AnalyzedSkeleton = Skeleton;
        Skinning.bValid           = true;

        const FMeshResource& Resource = SkeletalMesh->GetMeshResource();
        const FSkeletonResource* SkeletonResource = (Skeleton != nullptr) ? Skeleton->GetSkeletonResource() : nullptr;
        const uint32 BoneCount = (SkeletonResource != nullptr) ? (uint32)SkeletonResource->GetNumBones() : 0u;

        Skinning.PerBone.resize(BoneCount);

        // Every LOD: a coarse level can weight to a bone the base mesh never touches, and the GPU fetches it.
        for (const FMeshletSkinnedVertex& V : Resource.MeshletData.MeshletSkinnedVertices)
        {
            for (uint32 b = 0; b < 4u; ++b)
            {
                if (JointAt(V.JointWeights, b) != 0u)
                {
                    Skinning.RequiredBones = Math::Max(Skinning.RequiredBones, JointAt(V.JointIndices, b) + 1u);
                }
            }
        }

        ForEachLOD0SkinnedVertex(Resource, [&](const FMeshlet&, const FMeshletSkinnedVertex& V)
        {
            ++Skinning.LOD0Vertices;

            uint32 Influences  = 0;
            uint32 WeightTotal = 0;
            bool   bOutOfRange = false;

            for (uint32 b = 0; b < 4u; ++b)
            {
                const uint32 Weight = JointAt(V.JointWeights, b);
                if (Weight == 0u)
                {
                    continue;
                }

                ++Influences;
                WeightTotal += Weight;

                const uint32 Joint = JointAt(V.JointIndices, b);
                if (Joint >= BoneCount)
                {
                    bOutOfRange = true;
                    continue;
                }

                FBoneInfluenceStats& Stats = Skinning.PerBone[Joint];
                const float Normalized = (float)Weight / 255.0f;
                ++Stats.VertexCount;
                Stats.TotalWeight += Normalized;
                Stats.MaxWeight = Math::Max(Stats.MaxWeight, Normalized);
            }

            Skinning.InfluenceHistogram[Math::Min(Influences, 4u)] += 1u;
            Skinning.OutOfRangeVertices += bOutOfRange ? 1u : 0u;
            Skinning.UnnormalizedVertices += (WeightTotal != 255u && Influences > 0u) ? 1u : 0u;

            // Exactly what PackSkinWeights collapses an empty weight set to, so it reads as "no weights".
            if (Influences == 1u && JointAt(V.JointWeights, 0) == 255u && JointAt(V.JointIndices, 0) == 0u)
            {
                ++Skinning.RigidToRootVertices;
            }
        });

        for (const FBoneInfluenceStats& Stats : Skinning.PerBone)
        {
            Skinning.UnusedBones += (Stats.VertexCount == 0u) ? 1u : 0u;
        }

        PointsCachedForBone = -1;
    }

    void FSkeletalMeshEditorTool::RebuildSelectedBonePoints()
    {
        if (PointsCachedForBone == SelectedBoneIndex)
        {
            return;
        }

        SelectedBonePoints.clear();
        SelectedBoneTotalPoints = 0;
        PointsCachedForBone     = SelectedBoneIndex;

        CSkeletalMesh* SkeletalMesh = Cast<CSkeletalMesh>(Asset.Get());
        if (SkeletalMesh == nullptr || SelectedBoneIndex < 0)
        {
            return;
        }

        const uint32 Bone = (uint32)SelectedBoneIndex;

        ForEachLOD0SkinnedVertex(SkeletalMesh->GetMeshResource(),
            [&](const FMeshlet& Meshlet, const FMeshletSkinnedVertex& V)
        {
            for (uint32 b = 0; b < 4u; ++b)
            {
                if (JointAt(V.JointWeights, b) == 0u || JointAt(V.JointIndices, b) != Bone)
                {
                    continue;
                }

                ++SelectedBoneTotalPoints;
                if (SelectedBonePoints.size() < kMaxInfluencePoints)
                {
                    SelectedBonePoints.push_back(DecodeMeshletPosition(Meshlet, V));
                }
                return;
            }
        });
    }

    void FSkeletalMeshEditorTool::BuildBonePoseTransforms(TVector<FMatrix4>& OutTransforms) const
    {
        OutTransforms.clear();

        CSkeletalMesh* SkeletalMesh = Cast<CSkeletalMesh>(Asset.Get());
        if (SkeletalMesh == nullptr || !SkeletalMesh->Skeleton.IsValid() || MeshEntity == entt::null)
        {
            return;
        }

        FSkeletonResource* SkeletonResource = SkeletalMesh->Skeleton->GetSkeletonResource();
        if (SkeletonResource == nullptr)
        {
            return;
        }

        const FMatrix4 EntityMatrix = World->GetComponent<STransformComponent>(MeshEntity).GetWorldMatrix();
        OutTransforms.resize(SkeletonResource->GetNumBones());

        // Bones[] is parents-before-children, so one linear pass resolves the whole hierarchy.
        for (int32 i = 0; i < SkeletonResource->GetNumBones(); ++i)
        {
            const FSkeletonResource::FBoneInfo& Bone = SkeletonResource->GetBone(i);
            OutTransforms[i] = (Bone.ParentIndex == INDEX_NONE)
                ? EntityMatrix * Bone.LocalTransform
                : OutTransforms[Bone.ParentIndex] * Bone.LocalTransform;
        }
    }

    float FSkeletalMeshEditorTool::GetBoneDrawRadius() const
    {
        CSkeletalMesh* SkeletalMesh = Cast<CSkeletalMesh>(Asset.Get());
        if (SkeletalMesh == nullptr)
        {
            return 0.02f;
        }

        // Proportional to the mesh, or the markers swallow a prop and vanish on a giant rig.
        const FVector3 Size = SkeletalMesh->GetAABB().GetSize();
        const float Extent = Math::Max(Math::Max(Size.x, Size.y), Size.z);
        return Math::Clamp(Extent * 0.012f, 0.004f, 0.5f);
    }

    void FSkeletalMeshEditorTool::DrawSkinningPanel()
    {
        CSkeletalMesh* SkeletalMesh = Cast<CSkeletalMesh>(Asset.Get());
        if (SkeletalMesh == nullptr)
        {
            return;
        }

        RebuildSkinningAnalysis();

        CSkeleton* Skeleton = SkeletalMesh->Skeleton.Get();
        const FSkeletonResource* SkeletonResource = (Skeleton != nullptr) ? Skeleton->GetSkeletonResource() : nullptr;
        const uint32 BoneCount = (SkeletonResource != nullptr) ? (uint32)SkeletonResource->GetNumBones() : 0u;

        ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
        ImGui::SeparatorText("Binding");
        ImGuiX::Font::PopFont();

        ImGui::Spacing();

        if (SkeletonResource == nullptr)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                LE_ICON_ALERT_CIRCLE_OUTLINE " No skeleton assigned.");
            ImGui::TextWrapped("Joint indices address a skeleton's bone array directly, so without one this "
                               "mesh cannot be posed and the renderer holds it back. Assign one under Asset "
                               "Details, or reimport with a Target Skeleton set.");
            return;
        }

        ImGui::Text("Skeleton: %s", Skeleton->GetName().c_str());
        ImGui::Text("Bones: %u", BoneCount);
        ImGui::Text("Bones this mesh needs: %u", Skinning.RequiredBones);

        ImGui::Spacing();

        if (Skinning.RequiredBones > BoneCount)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                LE_ICON_ALERT_CIRCLE_OUTLINE " Mesh references %u bones but the skeleton provides %u.",
                Skinning.RequiredBones, BoneCount);
            ImGui::TextWrapped("The GPU bone fetch is unbounded, so vertices weighted past the end read whatever "
                               "follows this instance's bone slice and fly across the level. Reimport the mesh "
                               "against this skeleton.");
        }
        else if (Skinning.OutOfRangeVertices > 0)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                LE_ICON_ALERT_CIRCLE_OUTLINE " %u vertices reference a bone this skeleton does not have.",
                Skinning.OutOfRangeVertices);
        }
        else
        {
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1.0f),
                LE_ICON_CHECK_CIRCLE_OUTLINE " Every weighted joint lands inside the skeleton.");
        }

        if (Skinning.RigidToRootVertices > 0)
        {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                LE_ICON_ALERT_CIRCLE_OUTLINE " %u vertices are rigid to bone 0 (%.1f%%).",
                Skinning.RigidToRootVertices,
                Skinning.LOD0Vertices > 0 ? 100.0f * Skinning.RigidToRootVertices / Skinning.LOD0Vertices : 0.0f);
            ImGuiX::TextTooltip("{}", "The shape an empty weight set collapses to. A large share means the "
                                      "source had no usable skin weights and the mesh piles up at the root.");
        }

        ImGui::Spacing();
        ImGui::Spacing();

        ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
        ImGui::SeparatorText("Influences");
        ImGuiX::Font::PopFont();

        ImGui::Spacing();
        ImGui::TextDisabled("Counted over %u LOD 0 meshlet vertices.", Skinning.LOD0Vertices);
        ImGui::Spacing();

        if (ImGui::BeginTable("##Influences", 3,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Bones per vertex", ImGuiTableColumnFlags_WidthFixed, 140.0f);
            ImGui::TableSetupColumn("Vertices", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Share", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (uint32 i = 0; i <= 4u; ++i)
            {
                if (Skinning.InfluenceHistogram[i] == 0u)
                {
                    continue;
                }

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (i == 0)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "0 (unweighted)");
                }
                else
                {
                    ImGui::Text("%u", i);
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%u", Skinning.InfluenceHistogram[i]);

                ImGui::TableSetColumnIndex(2);
                const float Share = Skinning.LOD0Vertices > 0
                    ? (float)Skinning.InfluenceHistogram[i] / (float)Skinning.LOD0Vertices : 0.0f;
                ImGui::ProgressBar(Share, ImVec2(-1.0f, 0.0f));
            }

            ImGui::EndTable();
        }

        if (Skinning.UnnormalizedVertices > 0)
        {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                LE_ICON_ALERT_CIRCLE_OUTLINE " %u vertices have weights that do not sum to 1.",
                Skinning.UnnormalizedVertices);
            ImGuiX::TextTooltip("{}", "A shortfall drags the vertex toward the mesh origin once posed. "
                                      "Normalize the weights in the DCC and reimport.");
        }

        ImGui::Spacing();
        ImGui::Spacing();

        ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
        ImGui::SeparatorText("Bones");
        ImGuiX::Font::PopFont();

        ImGui::Spacing();

        if (Skinning.UnusedBones > 0)
        {
            ImGui::TextDisabled("%u of %u bones drive no vertex of this mesh.", Skinning.UnusedBones, BoneCount);
            ImGuiX::TextTooltip("{}", "Normal for a shared skeleton: a modular piece only skins to the bones "
                                      "it covers. Suspicious when the whole mesh should be weighted.");
            ImGui::Spacing();
        }

        if (ImGui::Button("Clear Selection"))
        {
            SelectedBoneIndex = -1;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Select a bone to highlight what it drives.");

        ImGui::Spacing();

        if (ImGui::BeginTable("##BoneInfluence", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_SizingFixedFit,
            ImVec2(0, 320)))
        {
            ImGui::TableSetupColumn("Index",    ImGuiTableColumnFlags_WidthFixed, 50.0f);
            ImGui::TableSetupColumn("Bone",     ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Vertices", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Max Wt",   ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableHeadersRow();

            ImGuiListClipper Clipper;
            Clipper.Begin((int)Skinning.PerBone.size());

            while (Clipper.Step())
            {
                for (int i = Clipper.DisplayStart; i < Clipper.DisplayEnd; ++i)
                {
                    const FBoneInfluenceStats& Stats = Skinning.PerBone[i];
                    const bool bUnused = Stats.VertexCount == 0;

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);

                    ImGui::PushID(i);
                    if (ImGui::Selectable("##row", SelectedBoneIndex == i,
                                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                    {
                        SelectedBoneIndex = (SelectedBoneIndex == i) ? -1 : i;
                    }
                    ImGui::PopID();

                    ImGui::SameLine();
                    ImGui::Text("%d", i);

                    ImGui::TableSetColumnIndex(1);
                    if (bUnused)
                    {
                        ImGui::TextDisabled("%s", SkeletonResource->GetBone(i).Name.c_str());
                    }
                    else
                    {
                        ImGui::TextUnformatted(SkeletonResource->GetBone(i).Name.c_str());
                    }

                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%u", Stats.VertexCount);

                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%.2f", Stats.MaxWeight);
                }
            }

            ImGui::EndTable();
        }

        if (SelectedBoneIndex >= 0 && SelectedBoneIndex < (int32)Skinning.PerBone.size())
        {
            RebuildSelectedBonePoints();

            ImGui::Spacing();
            ImGui::Text("'%s' drives %u vertices.",
                        SkeletonResource->GetBone(SelectedBoneIndex).Name.c_str(), SelectedBoneTotalPoints);

            if (SelectedBoneTotalPoints > (uint32)SelectedBonePoints.size())
            {
                ImGui::TextDisabled("Overlay shows the first %zu.", SelectedBonePoints.size());
            }
        }
    }

    void FSkeletalMeshEditorTool::OnInitialize()
    {
        // Scrolling disabled: the UV canvas claims the wheel for zoom, and a scrollable host would fight it.
        CreateToolWindow(UVViewerName, [&](bool bFocused)
        {
            if (CSkeletalMesh* SkeletalMesh = Cast<CSkeletalMesh>(Asset.Get()))
            {
                UVViewer.Draw(SkeletalMesh->GetMeshResource());
            }
        }, ImVec2(-1, -1), true);

        CreateToolWindow(SkinningName, [&](bool bFocused)
        {
            DrawSkinningPanel();
        });

        CreateToolWindow(MeshPropertiesName, [&](bool bFocused)
        {
            CSkeletalMesh* SkeletalMesh = CastAsserted<CSkeletalMesh>(Asset.Get());

            FMeshResource& Resource = SkeletalMesh->GetMeshResource();
            const FMeshletData& MD = Resource.MeshletData;
            const FAABB& BoundingBox = SkeletalMesh->GetAABB();

            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::SeparatorText("Mesh Statistics");
            ImGuiX::Font::PopFont();

            ImGui::Spacing();

            if (ImGui::BeginTable("##MeshStats", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 180.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                auto PropertyRow = [](const char* label, const FString& value, const ImVec4* color = nullptr)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(label);
                    ImGui::TableSetColumnIndex(1);
                    if (color)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, *color);
                    }
                    ImGui::TextUnformatted(value.c_str());
                    if (color)
                    {
                        ImGui::PopStyleColor();
                    }
                };

                // Measured off meshlets: GenerateGPUBuffers drops Positions and Indices once the mesh loads.
                uint32 LOD0Triangles = 0;
                uint32 MaxLODsAcrossSurfaces = 0;
                for (const FGeometrySurface& Surface : Resource.GeometrySurfaces)
                {
                    MaxLODsAcrossSurfaces = Math::Max(MaxLODsAcrossSurfaces, Surface.NumLODs);
                    LOD0Triangles += SumSkinnedTrianglesInRange(MD.Meshlets, Surface.LODMeshletOffset[0], Surface.LODMeshletCount[0]);
                }

                PropertyRow("Meshlet Vertices", eastl::to_string(MD.MeshletSkinnedVertices.size()));
                PropertyRow("Triangles (LOD 0)", eastl::to_string(LOD0Triangles));
                PropertyRow("Meshlets", eastl::to_string(MD.Meshlets.size()));
                PropertyRow("Surfaces", eastl::to_string(Resource.GetNumSurfaces()));
                PropertyRow("LOD Levels", eastl::to_string(MaxLODsAcrossSurfaces));

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Dummy(ImVec2(0, 4));

                const float vertexSizeKB  = (MD.MeshletSkinnedVertices.size() * sizeof(FMeshletSkinnedVertex)) / 1024.0f;
                const float meshletSizeKB = (MD.Meshlets.size() * sizeof(FMeshlet)
                                           + MD.MeshletSpheres.size() * sizeof(FMeshletSphere)
                                           + MD.MeshletCones.size() * sizeof(FMeshletCone)
                                           + MD.MeshletTriangles.size() * sizeof(uint32)) / 1024.0f;
                const float totalSizeKB   = vertexSizeKB + meshletSizeKB;

                PropertyRow("Vertex Buffer", eastl::to_string(static_cast<int>(vertexSizeKB)) + " KB");
                PropertyRow("Meshlet Data",  eastl::to_string(static_cast<int>(meshletSizeKB)) + " KB");

                ImVec4 totalColor = totalSizeKB > 1024 ? ImVec4(1.0f, 0.7f, 0.3f, 1.0f) : ImVec4(0.7f, 1.0f, 0.7f, 1.0f);
                PropertyRow("Total Memory", eastl::to_string(static_cast<int>(totalSizeKB)) + " KB", &totalColor);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Dummy(ImVec2(0, 4));

                PropertyRow("Bounds Min", Math::ToString(BoundingBox.Min).c_str());
                PropertyRow("Bounds Max", Math::ToString(BoundingBox.Max).c_str());

                FVector3 extents = BoundingBox.Max - BoundingBox.Min;
                PropertyRow("Bounds Extents", Math::ToString(extents).c_str());

                ImGui::EndTable();
            }

            ImGui::Spacing();
            ImGui::Spacing();

            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::SeparatorText("Levels of Detail");
            ImGuiX::Font::PopFont();

            ImGui::Spacing();

            {
                uint32 AvailableLODs = 1;
                for (const FGeometrySurface& Surface : Resource.GeometrySurfaces)
                {
                    AvailableLODs = Math::Max(AvailableLODs, Surface.NumLODs);
                }
                AvailableLODs = Math::Clamp(AvailableLODs, 1u, (uint32)MAX_MESH_LODS);

                char        LODNames[MAX_MESH_LODS][24];
                const char* PreviewItems[MAX_MESH_LODS + 1];
                PreviewItems[0] = "Automatic (distance)";
                for (uint32 i = 0; i < AvailableLODs; ++i)
                {
                    if (i == 0)
                    {
                        snprintf(LODNames[i], sizeof(LODNames[i]), "LOD %u (full detail)", i);
                    }
                    else
                    {
                        snprintf(LODNames[i], sizeof(LODNames[i]), "LOD %u", i);
                    }
                    PreviewItems[i + 1] = LODNames[i];
                }

                // A stale selection from a mesh with more LODs would otherwise index past the list.
                PreviewLODIndex = Math::Min(PreviewLODIndex, (int32)AvailableLODs - 1);

                int PreviewItem = (PreviewLODIndex < 0) ? 0 : PreviewLODIndex + 1;
                ImGui::SetNextItemWidth(220.0f);
                if (ImGui::Combo("Preview LOD", &PreviewItem, PreviewItems, (int)AvailableLODs + 1))
                {
                    PreviewLODIndex = (PreviewItem == 0) ? -1 : PreviewItem - 1;
                }
                ImGuiX::TextTooltip("Force the viewport mesh to a specific LOD regardless of camera distance.");

                if (AvailableLODs == 1)
                {
                    ImGui::TextDisabled("This mesh has one LOD, so there is nothing to switch between.");
                }
            }

            ImGui::Spacing();

            uint32 LODAggMeshlets[MAX_MESH_LODS]  = { 0 };
            uint32 LODAggTriangles[MAX_MESH_LODS] = { 0 };
            uint32 LODAggSurfaces[MAX_MESH_LODS]  = { 0 };
            for (const FGeometrySurface& Surface : Resource.GeometrySurfaces)
            {
                for (uint32 lod = 0; lod < Surface.NumLODs; ++lod)
                {
                    const uint32 MCnt = Surface.LODMeshletCount[lod];
                    LODAggMeshlets[lod]  += MCnt;
                    LODAggSurfaces[lod]  += (MCnt > 0) ? 1u : 0u;
                    LODAggTriangles[lod] += SumSkinnedTrianglesInRange(MD.Meshlets, Surface.LODMeshletOffset[lod], MCnt);
                }
            }

            const uint32 LOD0Tris = LODAggTriangles[0];

            if (ImGui::BeginTable("##LODSummary", 5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("LOD",       ImGuiTableColumnFlags_WidthFixed, 50.0f);
                ImGui::TableSetupColumn("Surfaces",  ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableSetupColumn("Meshlets",  ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Triangles", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableSetupColumn("vs LOD 0",  ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                for (uint32 lod = 0; lod < MAX_MESH_LODS; ++lod)
                {
                    if (LODAggSurfaces[lod] == 0)
                    {
                        continue;
                    }

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("%u", lod);
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%u", LODAggSurfaces[lod]);
                    ImGui::TableSetColumnIndex(2); ImGui::Text("%u", LODAggMeshlets[lod]);
                    ImGui::TableSetColumnIndex(3); ImGui::Text("%u", LODAggTriangles[lod]);
                    ImGui::TableSetColumnIndex(4);
                    if (LOD0Tris > 0)
                    {
                        ImGui::Text("%.1f%%", 100.0f * (float)LODAggTriangles[lod] / (float)LOD0Tris);
                    }
                    else
                    {
                        ImGui::TextDisabled("--");
                    }
                }
                ImGui::EndTable();
            }

            ImGui::Spacing();
            ImGui::Spacing();

            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::SeparatorText("Geometry Surfaces");
            ImGuiX::Font::PopFont();

            ImGui::Spacing();

            if (Resource.GeometrySurfaces.empty())
            {
                ImGui::TextDisabled("No surfaces defined");
            }
            else
            {
                for (size_t i = 0; i < Resource.GeometrySurfaces.size(); ++i)
                {
                    const FGeometrySurface& Surface = Resource.GeometrySurfaces[i];
                    ImGui::PushID(static_cast<int>(i));

                    const bool bSelected = ((int32)i == SelectedSurfaceIndex);

                    FString headerLabel = "Surface " + eastl::to_string(i) + ": " + Surface.ID.ToString();
                    const bool bOpen = ImGui::CollapsingHeader(headerLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen);

                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                    {
                        SelectedSurfaceIndex = bSelected ? -1 : (int32)i;
                    }
                    ImGuiX::TextTooltip("{}", "Right-click to outline this surface in the viewport.");

                    if (bOpen)
                    {
                        ImGui::Indent(16.0f);

                        if (ImGui::BeginTable("##SurfaceDetails", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
                        {
                            ImGui::TableSetupColumn("##Label", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                            ImGui::TableSetupColumn("##Value", ImGuiTableColumnFlags_WidthStretch);

                            auto DetailRow = [](const char* label, const FString& value)
                            {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", label);
                                ImGui::TableSetColumnIndex(1);
                                ImGui::TextUnformatted(value.c_str());
                            };

                            const uint32 SurfaceTris = SumSkinnedTrianglesInRange(MD.Meshlets, Surface.LODMeshletOffset[0],
                                                                          Surface.LODMeshletCount[0]);

                            DetailRow("Material Index:", eastl::to_string(Surface.MaterialIndex));
                            DetailRow("LOD Levels:", eastl::to_string(Surface.NumLODs));
                            DetailRow("Meshlets (LOD 0):", eastl::to_string(Surface.LODMeshletCount[0]));
                            DetailRow("Triangles (LOD 0):", eastl::to_string(SurfaceTris));

                            if (CMaterialInterface* Material = SkeletalMesh->GetMaterialAtSlot(Surface.MaterialIndex))
                            {
                                DetailRow("Material:", FString(Material->GetName().c_str()));
                            }

                            ImGui::EndTable();
                        }

                        ImGui::Unindent(16.0f);
                    }

                    ImGui::PopID();

                    if (i < Resource.GeometrySurfaces.size() - 1)
                    {
                        ImGui::Spacing();
                    }
                }

                if (SelectedSurfaceIndex >= (int32)Resource.GeometrySurfaces.size())
                {
                    SelectedSurfaceIndex = -1;
                }
            }

            ImGui::Spacing();
            ImGui::Spacing();

            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::SeparatorText("Asset Details");
            ImGuiX::Font::PopFont();

            ImGui::Spacing();
            PropertyTable.DrawTree();
        });
    }

    void FSkeletalMeshEditorTool::SetupWorldForTool()
    {
        FEditorTool::SetupWorldForTool();

        DirectionalLightEntity = World->ConstructEntity("Directional Light");
        World->EmplaceComponent<SDirectionalLightComponent>(DirectionalLightEntity);
        World->EmplaceComponent<SEnvironmentComponent>(DirectionalLightEntity);
        World->EmplaceComponent<SSkyLightComponent>(DirectionalLightEntity);

        CSkeletalMesh* SkeletalMesh = Cast<CSkeletalMesh>(Asset.Get());

        CameraState.Speed = 5.0f;

        MeshEntity = World->ConstructEntity("MeshEntity");
        SSkeletalMeshComponent& MeshComponent = World->EmplaceComponent<SSkeletalMeshComponent>(MeshEntity);
        MeshComponent.SetSkeletalMesh(SkeletalMesh);

        if (SkeletalMesh->Skeleton.IsValid())
        {
            SkeletalMesh->Skeleton->ComputeBindPoseSkinningMatrices(MeshComponent.BoneTransforms);
            MeshComponent.bRenderBonesDirty = true;
        }

        STransformComponent& MeshTransform = World->GetComponent<STransformComponent>(MeshEntity);

        // Framed off bounds, or anything but a human-sized rig opens inside the mesh or too far to see.
        const FAABB Bounds = SkeletalMesh->GetAABB();
        CreateFloorPlane(MeshTransform.GetLocation().y + Bounds.Min.y);

        const FVector3 Center = MeshTransform.GetLocation() + Bounds.GetCenter();
        const float Radius = Math::Max(Math::Length(Bounds.GetSize() * 0.5f), 0.5f);
        SetOrbitTarget(Center, Radius * 3.0f);
        SetCameraMode(EEditorCameraMode::Orbit);
    }

    void FSkeletalMeshEditorTool::Update(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::Update(UpdateContext);

        if (!World.IsValid() || MeshEntity == entt::null)
        {
            return;
        }

        if (World->GetRenderer())
        {
            World->GetRenderer()->GetSceneRenderSettings().bDrawBillboards = false;
        }

        SSkeletalMeshComponent& MeshComponent = World->GetComponent<SSkeletalMeshComponent>(MeshEntity);
        STransformComponent&    Transform     = World->GetComponent<STransformComponent>(MeshEntity);

        // A direct write is not enough: the retained scene only re-reads primitives that report a change.
        if (MeshComponent.ForcedLODIndex != PreviewLODIndex)
        {
            MeshComponent.ForcedLODIndex = PreviewLODIndex;
            MeshComponent.MarkRenderStateDirty();
        }

        const FMatrix4 EntityMatrix = Transform.GetWorldMatrix();

        if (bShowAABB)
        {
            FAABB AABB = MeshComponent.GetAABB().ToWorld(EntityMatrix);
            World->DrawBox(AABB.GetCenter(), AABB.GetSize() * 0.5f, FQuat(1, 0, 0, 0), FColor::Green);
        }

        CSkeletalMesh* SkeletalMesh = Cast<CSkeletalMesh>(Asset.Get());
        if (!IsValid(SkeletalMesh))
        {
            return;
        }

        if (SelectedSurfaceIndex >= 0)
        {
            const FMeshResource& Resource = SkeletalMesh->GetMeshResource();
            if (SelectedSurfaceIndex < (int32)Resource.GeometrySurfaces.size())
            {
                const FGeometrySurface& Surface = Resource.GeometrySurfaces[SelectedSurfaceIndex];
                const FMeshletData&     MD      = Resource.MeshletData;

                const uint32 OverlayLOD = (PreviewLODIndex >= 0 && Surface.NumLODs > 0)
                    ? (uint32)Math::Min((int32)Surface.NumLODs - 1, PreviewLODIndex)
                    : 0u;

                const uint32 Offset = Surface.LODMeshletOffset[OverlayLOD];
                const uint32 End    = Math::Min(Offset + Surface.LODMeshletCount[OverlayLOD], (uint32)MD.MeshletSpheres.size());

                if (End > Offset)
                {
                    FVector3 Lo( FLT_MAX);
                    FVector3 Hi(-FLT_MAX);
                    for (uint32 m = Offset; m < End; ++m)
                    {
                        const FMeshletSphere& B = MD.MeshletSpheres[m];
                        Lo = Math::Min(Lo, B.Center - FVector3(B.Radius));
                        Hi = Math::Max(Hi, B.Center + FVector3(B.Radius));
                    }

                    FAABB SurfaceAABB;
                    SurfaceAABB.Min = Lo;
                    SurfaceAABB.Max = Hi;
                    SurfaceAABB     = SurfaceAABB.ToWorld(EntityMatrix);
                    World->DrawBox(SurfaceAABB.GetCenter(), SurfaceAABB.GetSize() * 0.5f, FQuat(1, 0, 0, 0), FColor::Yellow, 2.0f);
                }
            }
        }

        // One resolved bind pose, shared by the bone, socket and influence overlays.
        TVector<FMatrix4> BoneWorld;
        BuildBonePoseTransforms(BoneWorld);

        if (BoneWorld.empty())
        {
            return;
        }

        FSkeletonResource* SkeletonResource = SkeletalMesh->Skeleton->GetSkeletonResource();
        const float BoneRadius = GetBoneDrawRadius();

        if (bShowBones)
        {
            for (int32 i = 0; i < (int32)BoneWorld.size(); ++i)
            {
                const FSkeletonResource::FBoneInfo& Bone = SkeletonResource->GetBone(i);
                const FVector3 BonePosition = FVector3(BoneWorld[i][3]);

                World->DrawSphere(BonePosition, BoneRadius, FColor::Red, 8);

                if (Bone.ParentIndex != INDEX_NONE)
                {
                    World->DrawLine(FVector3(BoneWorld[Bone.ParentIndex][3]), BonePosition, FColor::Green);
                }
            }
        }

        if (SelectedBoneIndex >= 0 && SelectedBoneIndex < (int32)BoneWorld.size())
        {
            const FMatrix4& Selected = BoneWorld[SelectedBoneIndex];
            const FVector3  Position = FVector3(Selected[3]);
            const float     AxisLength = BoneRadius * 6.0f;

            World->DrawSphere(Position, BoneRadius * 1.8f, FVector4(1.0f, 0.85f, 0.2f, 1.0f), 12, 2.5f, false);
            World->DrawLine(Position, Position + Math::Normalize(FVector3(Selected[0])) * AxisLength, FVector4(1.0f, 0.2f, 0.2f, 1.0f), 3.0f, false);
            World->DrawLine(Position, Position + Math::Normalize(FVector3(Selected[1])) * AxisLength, FVector4(0.2f, 1.0f, 0.2f, 1.0f), 3.0f, false);
            World->DrawLine(Position, Position + Math::Normalize(FVector3(Selected[2])) * AxisLength, FVector4(0.2f, 0.4f, 1.0f, 1.0f), 3.0f, false);

            RebuildSelectedBonePoints();

            if (FImmediateLineRenderer* Lines = World->GetImmediateLines())
            {
                const uint32 Color = PackColor(FVector4(1.0f, 0.85f, 0.2f, 1.0f));
                const float  Tick  = BoneRadius * 0.5f;

                for (const FVector3& Local : SelectedBonePoints)
                {
                    const FVector3 P = FVector3(EntityMatrix * FVector4(Local, 1.0f));
                    Lines->Line(P - FVector3(Tick, 0, 0), P + FVector3(Tick, 0, 0), Color);
                    Lines->Line(P - FVector3(0, Tick, 0), P + FVector3(0, Tick, 0), Color);
                    Lines->Line(P - FVector3(0, 0, Tick), P + FVector3(0, 0, Tick), Color);
                }
            }
        }

        if (bShowSockets)
        {
            for (const FMeshSocket& Socket : SkeletalMesh->Skeleton->Sockets)
            {
                const int32 BoneIndex = SkeletonResource->FindBoneIndex(Socket.BoneName);
                if (BoneIndex == INDEX_NONE || BoneIndex >= (int32)BoneWorld.size())
                {
                    continue;
                }

                const FMatrix4 SocketMatrix = BoneWorld[BoneIndex] * Socket.RelativeTransform.GetMatrix();
                const FVector3 Position = FVector3(SocketMatrix[3]);
                const float AxisLength = BoneRadius * 5.0f;

                World->DrawSphere(Position, BoneRadius * 0.9f, FVector4(1.0f, 0.82f, 0.4f, 1.0f), 12, 2.5f, false);
                World->DrawLine(Position, Position + Math::Normalize(FVector3(SocketMatrix[0])) * AxisLength, FVector4(1.0f, 0.2f, 0.2f, 1.0f), 3.0f, false);
                World->DrawLine(Position, Position + Math::Normalize(FVector3(SocketMatrix[1])) * AxisLength, FVector4(0.2f, 1.0f, 0.2f, 1.0f), 3.0f, false);
                World->DrawLine(Position, Position + Math::Normalize(FVector3(SocketMatrix[2])) * AxisLength, FVector4(0.2f, 0.4f, 1.0f, 1.0f), 3.0f, false);
            }
        }
    }

    void FSkeletalMeshEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
    }

    void FSkeletalMeshEditorTool::DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize)
    {
        DrawCameraModeSelector();
    }

    void FSkeletalMeshEditorTool::OnAssetLoadFinished()
    {
    }

    void FSkeletalMeshEditorTool::OnAssetDataChangedExternally()
    {
        FAssetEditorTool::OnAssetDataChangedExternally();

        // Every one of these indexes geometry that no longer exists.
        UVViewer.Invalidate();
        Skinning = FSkinningAnalysis();
        SelectedBoneIndex = -1;
        PointsCachedForBone = -1;
        SelectedBonePoints.clear();
        SelectedSurfaceIndex = -1;
        PreviewLODIndex = -1;
    }

    void FSkeletalMeshEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("Skinning",
            "The Skinning panel checks the mesh against its skeleton: whether every weighted joint has a "
            "bone, how many bones each vertex blends, and which bones drive nothing. Select a bone to "
            "highlight the vertices it moves.");
        DrawHelpTextRow("Skeleton",
            "Joint indices address the skeleton's bone array by position, so a mesh only works with the "
            "skeleton it was imported against. To bind it to another, reimport with that Target Skeleton "
            "set; the importer rewrites the indices by bone name.");
        DrawHelpTextRow("LODs",
            "The Preview LOD picker forces a level so you can inspect its geometry; Automatic returns to "
            "distance-based selection.");
        DrawHelpTextRow("Visualizers",
            "Show Bones, Sockets and AABB live in the View menu. Right-click a surface header to outline "
            "it in the viewport.");
        DrawHelpTextRow("Reimport",
            "File > Reimport refreshes from disk, preserving material overrides and rebinding the new "
            "geometry to this mesh's existing skeleton by bone name.");
    }

    void FSkeletalMeshEditorTool::DrawToolMenu(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::DrawToolMenu(UpdateContext);

        // Gizmo Control Dropdown
        if (ImGui::BeginMenu(LE_ICON_MOVE_RESIZE " Gizmo Control"))
        {
            const char* operations[] = { "Translate", "Rotate", "Scale" };
            static int currentOp = 0;

            if (ImGui::Combo("##", &currentOp, operations, IM_ARRAYSIZE(operations)))
            {
                switch (currentOp)
                {
                case 0: GuizmoOp = ImGuizmo::TRANSLATE; break;
                case 1: GuizmoOp = ImGuizmo::ROTATE;    break;
                case 2: GuizmoOp = ImGuizmo::SCALE;     break;
                }
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(LE_ICON_BONE " Skeleton"))
        {
            CSkeletalMesh* SkeletalMesh = Cast<CSkeletalMesh>(Asset.Get());
            CSkeleton* Skeleton = (SkeletalMesh != nullptr) ? SkeletalMesh->Skeleton.Get() : nullptr;

            if (Skeleton != nullptr && Skeleton->GetSkeletonResource() != nullptr)
            {
                ImGuiX::Text("{0}", Skeleton->GetName().c_str());
                ImGuiX::Text("{0} bones, {1} sockets",
                             Skeleton->GetSkeletonResource()->GetNumBones(), (int32)Skeleton->Sockets.size());
            }
            else
            {
                ImGui::TextDisabled("No skeleton assigned.");
            }

            ImGui::Separator();

            if (ImGui::MenuItem(LE_ICON_REFRESH " Re-analyze Skinning"))
            {
                Skinning = FSkinningAnalysis();
                PointsCachedForBone = -1;
            }
            ImGuiX::TextTooltip("{}", "Rebuild the Skinning panel's per-bone counts from the current geometry.");

            ImGui::EndMenu();
        }
    }

    void FSkeletalMeshEditorTool::DrawViewModeExtraItems()
    {
        ImGui::Separator();
        ImGui::MenuItem(LE_ICON_CUBE_OUTLINE " Show AABB", nullptr, &bShowAABB);
        ImGui::MenuItem(LE_ICON_BONE " Show Bones", nullptr, &bShowBones);
        ImGui::MenuItem(LE_ICON_LINK " Show Sockets", nullptr, &bShowSockets);

        if (ImGui::MenuItem(LE_ICON_RELOAD " Reload Mesh Buffers"))
        {
            Asset->PostLoad();
        }
    }

    void FSkeletalMeshEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);

        ImGuiID leftDockID = 0, rightDockID = 0;
        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Right, 0.3f, &rightDockID, &leftDockID);

        ImGui::DockBuilderDockWindow(GetToolWindowName(ViewportWindowName).c_str(), leftDockID);
        // Tabbed with the viewport rather than split off it: the unwrap wants the same large canvas, and
        // it is an inspect-one-then-the-other workflow, not a side-by-side one.
        ImGui::DockBuilderDockWindow(GetToolWindowName(UVViewerName.data()).c_str(), leftDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(MeshPropertiesName.data()).c_str(), rightDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(SkinningName.data()).c_str(), rightDockID);
    }
}
