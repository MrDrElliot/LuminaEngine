#include "MeshUVViewer.h"

#include <imgui_internal.h>

#include "Containers/HashTable.h"
#include "Containers/String.h"
#include "Core/Math/Math.h"
#include "Core/Math/Packing.h"
#include "Renderer/MeshData.h"
#include "Renderer/Vertex.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    namespace
    {
        // Per-surface colors, so a multi-material mesh shows which shell belongs to which slot.
        constexpr ImU32 GSurfaceColors[] =
        {
            IM_COL32(120, 200, 255, 200),
            IM_COL32(160, 240, 150, 200),
            IM_COL32(255, 200, 120, 200),
            IM_COL32(230, 150, 240, 200),
            IM_COL32(255, 140, 140, 200),
            IM_COL32(150, 230, 230, 200),
            IM_COL32(220, 220, 140, 200),
            IM_COL32(180, 180, 255, 200),
        };

        constexpr float MinZoom = 16.0f;
        constexpr float MaxZoom = 65536.0f;

        FORCEINLINE ImVec2 UnpackUV(uint32 Packed)
        {
            const FVector2 UV = Math::UnpackHalf2x16(Packed);
            return ImVec2(UV.x, UV.y);
        }

        FORCEINLINE bool IsOutsideTile(const ImVec2& UV)
        {
            return UV.x < 0.0f || UV.x > 1.0f || UV.y < 0.0f || UV.y > 1.0f;
        }

        // Highest LOD any surface supplies, so the picker never offers a level nothing has.
        uint32 GetMaxLODCount(const FMeshResource& Resource)
        {
            uint32 MaxLODs = 1;
            for (const FGeometrySurface& Surface : Resource.GeometrySurfaces)
            {
                MaxLODs = Math::Max(MaxLODs, Surface.NumLODs);
            }
            return MaxLODs;
        }
    }

    void FMeshUVViewer::Invalidate()
    {
        CacheKey = FCacheKey();
        Edges.clear();
    }

    void FMeshUVViewer::RebuildEdges(const FMeshResource& Resource)
    {
        Edges.clear();
        TriangleCount = 0;
        OutsideTriangleCount = 0;
        bTruncated = false;

        const FMeshletData& MeshletData = Resource.MeshletData;
        const bool bSkinned = Resource.IsSkinnedMesh();

        // Exactly one vertex stream is populated; both start with FMeshletVertex, so the UV read is common.
        const FMeshletVertex* Vertices = bSkinned
            ? static_cast<const FMeshletVertex*>(MeshletData.MeshletSkinnedVertices.data())
            : MeshletData.MeshletVertices.data();
        const size_t VertexStride = bSkinned ? sizeof(FMeshletSkinnedVertex) : sizeof(FMeshletVertex);
        const size_t NumVertices = bSkinned ? MeshletData.MeshletSkinnedVertices.size() : MeshletData.MeshletVertices.size();

        if (Vertices == nullptr || NumVertices == 0)
        {
            return;
        }

        auto VertexAt = [Vertices, VertexStride](size_t Index) -> const FMeshletVertex&
        {
            return *reinterpret_cast<const FMeshletVertex*>(reinterpret_cast<const uint8*>(Vertices) + Index * VertexStride);
        };

        // Interior edges are shared by two triangles, and meshlets duplicate vertices along their seams, so
        // the raw edge list is nearly twice the size it needs to be. Keying on the PACKED uv pair makes the
        // match exact (no float compare) and collapses the duplicates that meshlet splitting introduced.
        THashSet<uint64> SeenEdges;
        SeenEdges.reserve(MaxEdges);

        auto AddEdge = [&](uint32 PackedA, uint32 PackedB, const ImVec2& A, const ImVec2& B, uint16 Surface)
        {
            const uint64 Low  = Math::Min(PackedA, PackedB);
            const uint64 High = Math::Max(PackedA, PackedB);
            const uint64 Key  = (High << 32) | Low;
            if (!SeenEdges.insert(Key).second)
            {
                return;
            }

            FUVEdge& Edge = Edges.emplace_back();
            Edge.A = A;
            Edge.B = B;
            Edge.Surface = Surface;
            Edge.bOutside = IsOutsideTile(A) || IsOutsideTile(B);
        };

        for (size_t SurfaceIndex = 0; SurfaceIndex < Resource.GeometrySurfaces.size(); ++SurfaceIndex)
        {
            if (SurfaceFilter >= 0 && SurfaceIndex != (size_t)SurfaceFilter)
            {
                continue;
            }

            const FGeometrySurface& Surface = Resource.GeometrySurfaces[SurfaceIndex];

            // A surface can carry fewer levels than the mesh's deepest; clamp rather than skip, so a
            // one-LOD surface still draws while the rest of the mesh is inspected at depth.
            const uint32 SurfaceLOD = Math::Min((uint32)LODIndex, Surface.NumLODs > 0 ? Surface.NumLODs - 1 : 0u);
            const uint32 MeshletOffset = Surface.LODMeshletOffset[SurfaceLOD];
            const uint32 MeshletCount  = Surface.LODMeshletCount[SurfaceLOD];

            for (uint32 i = 0; i < MeshletCount && !bTruncated; ++i)
            {
                const size_t MeshletIndex = MeshletOffset + i;
                if (MeshletIndex >= MeshletData.Meshlets.size())
                {
                    break;
                }

                const FMeshlet& Meshlet = MeshletData.Meshlets[MeshletIndex];

                for (uint32 t = 0; t < Meshlet.TriangleCount; ++t)
                {
                    const size_t PackedIndex = Meshlet.TriangleOffset + t;
                    if (PackedIndex >= MeshletData.MeshletTriangles.size())
                    {
                        break;
                    }

                    // Three micro-indices per dword, each relative to the meshlet's vertex range.
                    const uint32 Packed = MeshletData.MeshletTriangles[PackedIndex];
                    const uint32 Local[3] =
                    {
                        (Packed      ) & 0xFFu,
                        (Packed >>  8) & 0xFFu,
                        (Packed >> 16) & 0xFFu,
                    };

                    const size_t V0 = Meshlet.VertexOffset + Local[0];
                    const size_t V1 = Meshlet.VertexOffset + Local[1];
                    const size_t V2 = Meshlet.VertexOffset + Local[2];
                    if (V0 >= NumVertices || V1 >= NumVertices || V2 >= NumVertices)
                    {
                        continue;
                    }

                    const uint32 Packed0 = VertexAt(V0).UV;
                    const uint32 Packed1 = VertexAt(V1).UV;
                    const uint32 Packed2 = VertexAt(V2).UV;

                    const ImVec2 UV0 = UnpackUV(Packed0);
                    const ImVec2 UV1 = UnpackUV(Packed1);
                    const ImVec2 UV2 = UnpackUV(Packed2);

                    ++TriangleCount;
                    if (IsOutsideTile(UV0) || IsOutsideTile(UV1) || IsOutsideTile(UV2))
                    {
                        ++OutsideTriangleCount;
                    }

                    AddEdge(Packed0, Packed1, UV0, UV1, (uint16)SurfaceIndex);
                    AddEdge(Packed1, Packed2, UV1, UV2, (uint16)SurfaceIndex);
                    AddEdge(Packed2, Packed0, UV2, UV0, (uint16)SurfaceIndex);

                    if (Edges.size() >= MaxEdges)
                    {
                        bTruncated = true;
                        break;
                    }
                }
            }
        }
    }

    void FMeshUVViewer::FitView(const ImVec2& CanvasSize)
    {
        const float Extent = Math::Min(CanvasSize.x, CanvasSize.y);
        Zoom = Math::Max(Extent * 0.85f, MinZoom);
        Pan  = ImVec2((CanvasSize.x - Zoom) * 0.5f, (CanvasSize.y - Zoom) * 0.5f);
    }

    void FMeshUVViewer::DrawToolbar(const FMeshResource& Resource)
    {
        const uint32 MaxLODs = GetMaxLODCount(Resource);
        const float Scale = ImGuiX::GetUIScale();

        ImGui::SetNextItemWidth(90.0f * Scale);
        FFixedString LODLabel;
        FormatTo(LODLabel, "LOD {}", LODIndex);
        if (ImGui::BeginCombo("##UVLOD", LODLabel.c_str()))
        {
            for (uint32 i = 0; i < MaxLODs; ++i)
            {
                FFixedString Entry;
                FormatTo(Entry, "LOD {}", i);
                if (ImGui::Selectable(Entry.c_str(), (uint32)LODIndex == i))
                {
                    LODIndex = (int32)i;
                }
            }
            ImGui::EndCombo();
        }
        ImGuiX::TextTooltip("{}", "Which LOD's unwrap to show. Higher levels are far cheaper to draw on dense meshes.");

        ImGui::SameLine();

        ImGui::SetNextItemWidth(180.0f * Scale);
        const char* SurfaceLabel = (SurfaceFilter < 0)
            ? "All Surfaces"
            : Resource.GeometrySurfaces[SurfaceFilter].ID.c_str();
        if (ImGui::BeginCombo("##UVSurface", SurfaceLabel))
        {
            if (ImGui::Selectable("All Surfaces", SurfaceFilter < 0))
            {
                SurfaceFilter = -1;
            }

            for (size_t i = 0; i < Resource.GeometrySurfaces.size(); ++i)
            {
                ImGui::PushID((int)i);
                if (ImGui::Selectable(Resource.GeometrySurfaces[i].ID.c_str(), SurfaceFilter == (int32)i))
                {
                    SurfaceFilter = (int32)i;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        ImGui::Checkbox("Grid", &bShowGrid);

        ImGui::SameLine();
        ImGui::Checkbox("Tiles", &bShowTiles);
        ImGuiX::TextTooltip("{}", "Draw the neighboring 0..1 tiles, so wrapped UVs read as repeats rather than as stray geometry.");

        ImGui::SameLine();
        ImGui::Checkbox("Points", &bShowVertices);

        ImGui::SameLine();
        ImGui::Checkbox("Overflow", &bHighlightOverflow);
        ImGuiX::TextTooltip("{}", "Color edges that leave the 0..1 tile. Intentional for tiling materials, a bug for atlased or lightmapped meshes.");

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_FIT_TO_PAGE_OUTLINE " Fit"))
        {
            Zoom = 0.0f;    // refit against the live canvas size
        }
    }

    void FMeshUVViewer::DrawCanvas(const FMeshResource& Resource)
    {
        ImDrawList* DrawList = ImGui::GetWindowDrawList();

        const ImVec2 CanvasSize = ImGui::GetContentRegionAvail();
        if (CanvasSize.x < 1.0f || CanvasSize.y < 1.0f)
        {
            return;
        }

        CanvasOrigin = ImGui::GetCursorScreenPos();
        const ImRect CanvasRect(CanvasOrigin, ImVec2(CanvasOrigin.x + CanvasSize.x, CanvasOrigin.y + CanvasSize.y));

        // An InvisibleButton rather than IsWindowHovered: this claims the canvas as an item, so the wheel
        // scrolls the view instead of the parent window and the drag never reaches the dock host.
        ImGui::InvisibleButton("##UVCanvas", CanvasSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
        const bool bHovered = ImGui::IsItemHovered();
        const bool bActive  = ImGui::IsItemActive();

        if (Zoom <= 0.0f)
        {
            FitView(CanvasSize);
        }

        if (bActive && (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)))
        {
            const ImVec2 Delta = ImGui::GetIO().MouseDelta;
            Pan.x += Delta.x;
            Pan.y += Delta.y;
        }

        if (bHovered && ImGui::GetIO().MouseWheel != 0.0f)
        {
            // Anchored at the cursor: the UV under the pointer stays under the pointer.
            const ImVec2 Cursor = ImGui::GetIO().MousePos;
            const ImVec2 AnchorUV = ToUV(Cursor);

            const float Factor = Math::Pow(1.15f, ImGui::GetIO().MouseWheel);
            Zoom = Math::Clamp(Zoom * Factor, MinZoom, MaxZoom);

            Pan.x = Cursor.x - CanvasOrigin.x - AnchorUV.x * Zoom;
            Pan.y = Cursor.y - CanvasOrigin.y - AnchorUV.y * Zoom;
        }

        DrawList->PushClipRect(CanvasRect.Min, CanvasRect.Max, true);
        DrawList->AddRectFilled(CanvasRect.Min, CanvasRect.Max, IM_COL32(18, 18, 22, 255));

        // Neighboring tiles first, dimmest, so wrapped shells sit in a readable frame of reference.
        if (bShowTiles)
        {
            for (int32 TileY = -1; TileY <= 1; ++TileY)
            {
                for (int32 TileX = -1; TileX <= 1; ++TileX)
                {
                    if (TileX == 0 && TileY == 0)
                    {
                        continue;
                    }

                    const ImVec2 Min = ToScreen(ImVec2((float)TileX, (float)TileY));
                    const ImVec2 Max = ToScreen(ImVec2((float)TileX + 1.0f, (float)TileY + 1.0f));
                    DrawList->AddRect(Min, Max, IM_COL32(70, 70, 80, 120));
                }
            }
        }

        const ImVec2 TileMin = ToScreen(ImVec2(0.0f, 0.0f));
        const ImVec2 TileMax = ToScreen(ImVec2(1.0f, 1.0f));

        if (bShowGrid)
        {
            // Subdivision count follows the zoom, so the grid stays legible instead of turning into a
            // solid block when zoomed out or vanishing when zoomed in.
            const int32 Divisions = (Zoom > 1200.0f) ? 16 : (Zoom > 500.0f ? 8 : 4);
            for (int32 i = 1; i < Divisions; ++i)
            {
                const float T = (float)i / (float)Divisions;
                const ImVec2 VLine = ToScreen(ImVec2(T, 0.0f));
                const ImVec2 HLine = ToScreen(ImVec2(0.0f, T));
                DrawList->AddLine(ImVec2(VLine.x, TileMin.y), ImVec2(VLine.x, TileMax.y), IM_COL32(60, 60, 70, 140));
                DrawList->AddLine(ImVec2(TileMin.x, HLine.y), ImVec2(TileMax.x, HLine.y), IM_COL32(60, 60, 70, 140));
            }
        }

        DrawList->AddRect(TileMin, TileMax, IM_COL32(190, 190, 200, 220));

        // Visible UV rectangle, padded a little, to cull segments without transforming them.
        const ImVec2 ViewMin = ToUV(CanvasRect.Min);
        const ImVec2 ViewMax = ToUV(CanvasRect.Max);

        const ImU32 OverflowColor = EditorColors::U32(EditorColors::Warning());
        const float PointRadius = Math::Max(1.0f, Zoom * 0.0015f);

        for (const FUVEdge& Edge : Edges)
        {
            if (Math::Max(Edge.A.x, Edge.B.x) < ViewMin.x || Math::Min(Edge.A.x, Edge.B.x) > ViewMax.x ||
                Math::Max(Edge.A.y, Edge.B.y) < ViewMin.y || Math::Min(Edge.A.y, Edge.B.y) > ViewMax.y)
            {
                continue;
            }

            const ImU32 Color = (bHighlightOverflow && Edge.bOutside)
                ? OverflowColor
                : GSurfaceColors[Edge.Surface % IM_ARRAYSIZE(GSurfaceColors)];

            const ImVec2 A = ToScreen(Edge.A);
            const ImVec2 B = ToScreen(Edge.B);
            DrawList->AddLine(A, B, Color);

            if (bShowVertices)
            {
                DrawList->AddCircleFilled(A, PointRadius, Color, 4);
            }
        }

        DrawList->PopClipRect();

        // Cursor readout, drawn last so it sits over the unwrap.
        if (bHovered)
        {
            const ImVec2 CursorUV = ToUV(ImGui::GetIO().MousePos);
            FFixedString Readout;
            FormatTo(Readout, "U {:.4f}   V {:.4f}", CursorUV.x, CursorUV.y);

            const ImVec2 TextSize = ImGui::CalcTextSize(Readout.c_str());
            const ImVec2 TextPos(CanvasRect.Min.x + 8.0f, CanvasRect.Max.y - TextSize.y - 8.0f);
            DrawList->AddRectFilled(ImVec2(TextPos.x - 4.0f, TextPos.y - 2.0f),
                                    ImVec2(TextPos.x + TextSize.x + 4.0f, TextPos.y + TextSize.y + 2.0f),
                                    IM_COL32(0, 0, 0, 160));
            DrawList->AddText(TextPos, IM_COL32(230, 230, 235, 255), Readout.c_str());
        }
    }

    void FMeshUVViewer::Draw(const FMeshResource& Resource)
    {
        const bool bSkinned = Resource.IsSkinnedMesh();
        const uint32 MeshletCount = (uint32)Resource.MeshletData.Meshlets.size();
        const uint32 VertexCount  = (uint32)(bSkinned
            ? Resource.MeshletData.MeshletSkinnedVertices.size()
            : Resource.MeshletData.MeshletVertices.size());

        if (MeshletCount == 0 || VertexCount == 0)
        {
            ImGui::TextColored(EditorColors::TextMuted(), "This mesh has no meshlet data to unwrap.");
            return;
        }

        LODIndex = Math::Clamp(LODIndex, 0, (int32)GetMaxLODCount(Resource) - 1);
        SurfaceFilter = Math::Min(SurfaceFilter, (int32)Resource.GeometrySurfaces.size() - 1);

        DrawToolbar(Resource);

        // The surface filter is not in the key: it changes which edges are collected, so fold it in by
        // rebuilding whenever it moves. Cheaper to compare than to store a second key.
        const FCacheKey Key{ &Resource, (uint32)LODIndex, MeshletCount, VertexCount };
        if (!(Key == CacheKey) || CachedSurfaceFilter != SurfaceFilter)
        {
            CacheKey = Key;
            CachedSurfaceFilter = SurfaceFilter;
            RebuildEdges(Resource);
        }

        FFixedString Status;
        FormatTo(Status, "{} tris  |  {} edges  |  {} outside 0..1 ({:.1f}%)",
            TriangleCount,
            (uint32)Edges.size(),
            OutsideTriangleCount,
            TriangleCount > 0 ? (100.0f * (float)OutsideTriangleCount / (float)TriangleCount) : 0.0f);

        ImGui::TextColored(EditorColors::TextDim(), "%s", Status.c_str());

        if (bTruncated)
        {
            ImGui::SameLine();
            ImGui::TextColored(EditorColors::Warning(), LE_ICON_ALERT_CIRCLE_OUTLINE " truncated");
            ImGuiX::WrappedTooltip("{}", "This LOD has more edges than the viewer draws in one frame, so the "
                                         "unwrap is incomplete. Pick a higher LOD, or a single surface, to see all of it.");
        }

        ImGui::Separator();

        DrawCanvas(Resource);
    }
}
