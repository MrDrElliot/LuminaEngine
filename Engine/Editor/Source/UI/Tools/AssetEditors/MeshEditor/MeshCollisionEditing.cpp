#include "EditorPCH.h"
#include "MeshCollisionEditing.h"

#include "imgui_internal.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Package/Package.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiDragDrop.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Properties/PropertyTable.h"
#include "UI/Tools/Transactions/EditorTransaction.h"
#include "World/World.h"
#include "World/Entity/Components/CameraComponent.h"

namespace Lumina
{
    using Physics::CollisionGen::ECollisionGenerator;

    static const FVector4 ShapeColor    = FVector4(0.35f, 0.80f, 1.00f, 1.0f);
    static const FVector4 ShapeSelColor = FVector4(1.00f, 0.75f, 0.15f, 1.0f);
    static const FVector4 ShapeDropColor = FVector4(0.45f, 1.00f, 0.55f, 1.0f);
    static const FVector4 TriMeshColor  = FVector4(0.55f, 1.00f, 0.60f, 1.0f);

    // Light enough that the mesh and the outlines stay readable through the fill.
    static constexpr float SolidFillOpacity = 0.3f;

    // Drawn through the mesh like the outlines, since a collider usually sits inside the surface it wraps.
    static void DrawSolidMesh(FWorldDebugInterface& Debug, const TVector<FVector3>& Vertices, const TVector<uint32>& Indices,
                              const FMatrix4& Matrix, const FVector4& Color)
    {
        TVector<FSimpleElementVertex> Triangles;
        Triangles.Reserve(Indices.Num());
        const uint32 Packed = PackColor(Color);
        for (uint32 Index : Indices)
        {
            Triangles.Emplace(FVector3(Matrix * FVector4(Vertices[Index], 1.0f)), Packed);
        }
        Debug.DrawSolidTriangles(Move(Triangles), ESolidDrawMode::XRay);
    }

    static FVector4 FillColor(const FVector4& Color)
    {
        return FVector4(Color.x, Color.y, Color.z, Color.w * SolidFillOpacity);
    }

    // Names the palette on the drag-drop channel, so only collision targets react to these tiles.
    static const FName CollisionPaletteName("MeshCollision");

    struct FGeneratorInfo
    {
        ECollisionGenerator Generator;
        const char*         Icon;
        const char*         Name;
        const char*         Label;
        const char*         Tooltip;
    };

    static constexpr FGeneratorInfo GeneratorInfos[] =
    {
        { ECollisionGenerator::Box,             LE_ICON_CUBE_OUTLINE,     "Box",           LE_ICON_CUBE_OUTLINE " Box",
          "A box around the mesh. Drag it onto the viewport to place a smaller one where you drop it." },
        { ECollisionGenerator::Sphere,          LE_ICON_SPHERE,           "Sphere",        LE_ICON_SPHERE " Sphere",
          "A sphere around the mesh. Drag it onto the viewport to place a smaller one where you drop it." },
        { ECollisionGenerator::Capsule,         LE_ICON_PILL,             "Capsule",       LE_ICON_PILL " Capsule",
          "A capsule along the mesh's longest side, good for poles, barrels and characters. Drag it to place a smaller one." },
        { ECollisionGenerator::SingleHull,      LE_ICON_HEXAGON_OUTLINE,  "Convex Hull",   LE_ICON_HEXAGON_OUTLINE " Convex Hull",
          "One tight convex shell around the whole mesh. Works on moving objects, but fills in dents and holes." },
        { ECollisionGenerator::PerSurfaceHulls, LE_ICON_HEXAGON_MULTIPLE_OUTLINE, "Hull per Part", LE_ICON_HEXAGON_MULTIPLE_OUTLINE " Hull per Part",
          "One convex shell per mesh part, so a table's legs and top stay separate. Works on moving objects." },
        { ECollisionGenerator::TriangleMesh,    LE_ICON_VECTOR_TRIANGLE,  "Exact Mesh",    LE_ICON_VECTOR_TRIANGLE " Exact Mesh",
          "The mesh's own triangles. Exact, but only for objects that never simulate, and it replaces every other shape." },
    };

    static const FGeneratorInfo& GetGeneratorInfo(ECollisionGenerator Generator)
    {
        for (const FGeneratorInfo& Info : GeneratorInfos)
        {
            if (Info.Generator == Generator)
            {
                return Info;
            }
        }
        return GeneratorInfos[0];
    }

    static const char* PrimitiveIcon(ECollisionPrimitiveType Type)
    {
        switch (Type)
        {
        case ECollisionPrimitiveType::Sphere:     return LE_ICON_SPHERE;
        case ECollisionPrimitiveType::Capsule:    return LE_ICON_PILL;
        case ECollisionPrimitiveType::ConvexHull: return LE_ICON_HEXAGON_OUTLINE;
        default:                                  return LE_ICON_CUBE_OUTLINE;
        }
    }

    static const char* PrimitiveTypeName(ECollisionPrimitiveType Type)
    {
        switch (Type)
        {
        case ECollisionPrimitiveType::Sphere:     return "Sphere";
        case ECollisionPrimitiveType::Capsule:    return "Capsule";
        case ECollisionPrimitiveType::ConvexHull: return "Convex Hull";
        default:                                  return "Box";
        }
    }

    // A clickable, draggable card; true on the frame it is clicked rather than dragged.
    static bool DrawPaletteTile(const FGeneratorInfo& Info, const ImVec2& Size)
    {
        ImGui::PushID((int32)Info.Generator);

        const ImVec2 Min = ImGui::GetCursorScreenPos();
        const ImVec2 Max(Min.x + Size.x, Min.y + Size.y);
        const bool bClicked = ImGui::InvisibleButton("##Tile", Size);
        const bool bHovered = ImGui::IsItemHovered();
        const bool bHeld    = ImGui::IsItemActive();

        ImDrawList* Draw = ImGui::GetWindowDrawList();
        const float Rounding = ImGui::GetStyle().FrameRounding + 3.0f;
        const ImVec4 Fill = bHeld ? EditorColors::RowBgActive() : (bHovered ? EditorColors::RowBgHovered() : EditorColors::FrameBg());
        Draw->AddRectFilled(Min, Max, EditorColors::U32(Fill), Rounding);
        Draw->AddRect(Min, Max, EditorColors::U32(bHovered ? EditorColors::Accent() : EditorColors::Border()), Rounding, bHovered ? 1.5f : 1.0f);

        ImFont* Font = ImGui::GetFont();
        const float IconFontSize = ImGui::GetFontSize() * 1.9f;
        const ImVec2 IconSize = Font->CalcTextSizeA(IconFontSize, FLT_MAX, 0.0f, Info.Icon);
        Draw->AddText(Font, IconFontSize, ImVec2(Min.x + (Size.x - IconSize.x) * 0.5f, Min.y + Size.y * 0.40f - IconSize.y * 0.5f),
                      EditorColors::U32(bHovered ? EditorColors::Accent() : EditorColors::TextPrimary()), Info.Icon);

        const ImVec2 NameSize = ImGui::CalcTextSize(Info.Name);
        Draw->AddText(ImVec2(Min.x + (Size.x - NameSize.x) * 0.5f, Max.y - NameSize.y - Size.y * 0.12f),
                      EditorColors::U32(bHovered ? EditorColors::TextPrimary() : EditorColors::TextDim()), Info.Name);

        if (ImGui::BeginDragDropSource())
        {
            DragDrop::SetPaletteItemPayload(CollisionPaletteName, (int32)Info.Generator);
            ImGui::TextUnformatted(Info.Label);
            ImGui::EndDragDropSource();
        }
        else if (bHovered)
        {
            ImGuiX::TextTooltip("{}", Info.Tooltip);
        }

        ImGui::PopID();
        return bClicked;
    }

    FMeshCollisionEditing::FMeshCollisionEditing(FTransactionManager& InTransactions)
        : Transactions(InTransactions)
    {
        const auto WireTable = [this](FPropertyTable& Table)
        {
            Table.SetStartEditCallback([this](const FPropertyChangedEvent& Event) { BeginTransaction(Event.PropertyName); });
            Table.SetFinishEditCallback([this](const FPropertyChangedEvent&) { EndTransaction(); });
            Table.SetPostEditCallback([this](const FPropertyChangedEvent&)
            {
                RebuildHullWireframes();
                MarkChanged();
            });
        };

        DetailsTable = MakeUnique<FPropertyTable>();
        SettingsTable = MakeUnique<FPropertyTable>();
        WireTable(*DetailsTable);
        WireTable(*SettingsTable);
    }

    FMeshCollisionEditing::~FMeshCollisionEditing() = default;

    const char* FMeshCollisionEditing::GetGeneratorLabel(ECollisionGenerator Generator)
    {
        return GetGeneratorInfo(Generator).Label;
    }

    const char* FMeshCollisionEditing::GetGeneratorTooltip(ECollisionGenerator Generator)
    {
        return GetGeneratorInfo(Generator).Tooltip;
    }

    void FMeshCollisionEditing::DrawPalette()
    {
        const ImGuiStyle& Style = ImGui::GetStyle();
        const float FontSize    = ImGui::GetFontSize();

        // As many columns as keep the longest name inside its tile, so a narrow panel stacks instead of clipping.
        float NameWidth = 0.0f;
        for (const FGeneratorInfo& Info : GeneratorInfos)
        {
            NameWidth = Math::Max(NameWidth, ImGui::CalcTextSize(Info.Name).x);
        }

        const float MinWidth = Math::Max(NameWidth + FontSize * 1.5f, FontSize * 5.0f);
        const float Avail    = ImGui::GetContentRegionAvail().x;
        const int32 Columns  = Math::Clamp((int32)((Avail + Style.ItemSpacing.x) / (MinWidth + Style.ItemSpacing.x)), 1, 3);
        const ImVec2 TileSize(Math::Max((Avail - Style.ItemSpacing.x * (Columns - 1)) / Columns, MinWidth), FontSize * 4.4f);

        // Primitives can be dropped where they belong, while the fitted kinds always wrap the whole mesh.
        const auto DrawRow = [&](const char* Caption, int32 First, int32 Count)
        {
            ImGui::TextDisabled("%s", Caption);
            for (int32 i = 0; i < Count; ++i)
            {
                if (i % Columns != 0)
                {
                    ImGui::SameLine();
                }
                if (DrawPaletteTile(GeneratorInfos[First + i], TileSize))
                {
                    AddGenerated(GeneratorInfos[First + i].Generator);
                }
            }
        };

        DrawRow("Primitives, drag onto the mesh to place one", 0, 3);
        ImGui::Spacing();
        DrawRow("Fit to the whole mesh", 3, 3);
    }

    namespace
    {
        // Before and after images of only the mesh's collision, since a snapshot of the whole mesh would carry its geometry too.
        class FMeshCollisionSnapshotCommand final : public IUndoableCommand
        {
        public:

            FMeshCollisionSnapshotCommand(CStaticMesh* InMesh, FName InName)
                : Mesh(InMesh), Name(InName)
            {
                Capture(Before);
            }

            void Undo() override     { Restore(Before); }
            void Redo() override     { Restore(After); }
            void Finalize() override { Capture(After); }
            bool IsNoOp() const override { return Before == After; }
            FName GetName() const override { return Name; }

            void VisitObjectReferences(FObjectReferenceVisitor::FSlotFunc Func) override
            {
                Mesh = Cast<CStaticMesh>(Func(Mesh.Get()));
            }

        private:

            void Capture(TVector<uint8>& Out) const
            {
                Out.Reset();
                if (CStaticMesh* Target = Mesh.Get())
                {
                    FMemoryWriter Writer(Out);
                    FObjectProxyArchiver Ar(Writer, false);
                    SMeshCollision::StaticStruct()->SerializeProperties(Ar, &Target->Collision);
                }
            }

            void Restore(const TVector<uint8>& In) const
            {
                CStaticMesh* Target = Mesh.Get();
                if (Target == nullptr)
                {
                    return;
                }

                Target->Collision = SMeshCollision();
                FMemoryReader Reader(In);
                FObjectProxyArchiver Ar(Reader, true);
                SMeshCollision::StaticStruct()->SerializeProperties(Ar, &Target->Collision);

                if (CPackage* Package = Target->GetPackage())
                {
                    Package->MarkDirty();
                }
            }

            TStrongObjectPtr<CStaticMesh> Mesh;
            FName                         Name;
            TVector<uint8>                Before;
            TVector<uint8>                After;
        };
    }

    void FMeshCollisionEditing::SetMesh(CStaticMesh* InMesh)
    {
        if (Mesh.Get() == InMesh)
        {
            return;
        }

        CloseOpenDragTransaction();

        Mesh = InMesh;
        SelectedPrimitive = Constants::kIndexNone;
        DetailsTarget = nullptr;

        if (InMesh != nullptr)
        {
            SettingsTable->SetObject(&InMesh->Collision, SMeshCollision::StaticStruct());
            SettingsTable->MarkDirty();
        }

        RebuildHullWireframes();
    }

    SMeshCollision* FMeshCollisionEditing::GetCollision() const
    {
        CStaticMesh* Target = Mesh.Get();
        return Target != nullptr ? &Target->Collision : nullptr;
    }

    bool FMeshCollisionEditing::HasSelection() const
    {
        const SMeshCollision* Collision = GetCollision();
        return Collision != nullptr && SelectedPrimitive >= 0 && SelectedPrimitive < Collision->Primitives.Num();
    }

    void FMeshCollisionEditing::MarkChanged()
    {
        CStaticMesh* Target = Mesh.Get();
        if (Target != nullptr && Target->GetPackage() != nullptr)
        {
            Target->GetPackage()->MarkDirty();
        }
    }

    void FMeshCollisionEditing::BeginTransaction(FName Name)
    {
        if (!Mesh.IsValid())
        {
            return;
        }
        Transactions.BeginTransaction(Name);
        Transactions.Record(MakeUnique<FMeshCollisionSnapshotCommand>(Mesh.Get(), Name));
    }

    void FMeshCollisionEditing::EndTransaction()
    {
        Transactions.CommitTransaction();
    }

    void FMeshCollisionEditing::CloseOpenDragTransaction()
    {
        ShapeHandles::CloseOpenDragTransaction(*this, HandleState);
    }

    void FMeshCollisionEditing::RebuildHullWireframes()
    {
        HullDrawData.Reset();
        const SMeshCollision* Collision = GetCollision();
        if (Collision == nullptr)
        {
            return;
        }

        HullDrawData.SetNum(Collision->Primitives.Num());
        for (int32 i = 0; i < Collision->Primitives.Num(); ++i)
        {
            const SCollisionPrimitive& Primitive = Collision->Primitives[i];
            if (Primitive.Type == ECollisionPrimitiveType::ConvexHull)
            {
                FHullDrawData& Hull = HullDrawData[i];
                Physics::CollisionGen::BuildHullWireframe(Primitive.HullPoints, Hull.Vertices, Hull.Edges);
                Physics::CollisionGen::BuildHullTriangles(Primitive.HullPoints, Hull.SolidVertices, Hull.SolidIndices);
            }
        }
    }

    void FMeshCollisionEditing::OnUndoRedo()
    {
        // A restore rewrites the primitives wholesale, so the details pointer and selection go stale.
        DetailsTarget = nullptr;
        const SMeshCollision* Collision = GetCollision();
        if (Collision != nullptr && SelectedPrimitive >= Collision->Primitives.Num())
        {
            SelectedPrimitive = Constants::kIndexNone;
        }

        RebuildHullWireframes();
        SyncDetailsTable();
        SettingsTable->MarkDirty();
    }

    void FMeshCollisionEditing::AddGenerated(ECollisionGenerator Generator)
    {
        CStaticMesh* Target = Mesh.Get();
        if (Target == nullptr)
        {
            return;
        }
        SMeshCollision& Collision = Target->Collision;

        BeginTransaction(GetGeneratorInfo(Generator).Name);

        // An Exact Mesh cannot sit beside other shapes, so it replaces them, and adding to one replaces it in turn.
        const bool bReplaces = Generator == ECollisionGenerator::TriangleMesh || Collision.IsConcave();
        TVector<SCollisionPrimitive> Kept;
        if (!bReplaces)
        {
            Kept = Move(Collision.Primitives);
        }

        const bool bSuccess = Physics::CollisionGen::Generate(Generator, Target, Collision);
        const int32 FirstAdded = Kept.Num();
        if (!bReplaces)
        {
            Collision.Primitives.Insert(Kept, 0);
        }

        SelectedPrimitive = (bSuccess && !Collision.IsConcave() && FirstAdded < Collision.Primitives.Num()) ? FirstAdded : Constants::kIndexNone;
        RebuildHullWireframes();
        MarkChanged();
        EndTransaction();
        SettingsTable->MarkDirty();

        if (!bSuccess)
        {
            ImGuiX::Notifications::NotifyWarning("'{0}' produced no collision; the mesh may have no LOD 0 geometry.", Target->GetName());
        }
    }

    void FMeshCollisionEditing::AddPrimitive(const SCollisionPrimitive& Primitive)
    {
        SMeshCollision* Collision = GetCollision();
        if (Collision == nullptr)
        {
            return;
        }

        BeginTransaction("Add Collision Shape");

        // A hand-placed shape replaces a baked triangle mesh, since the two are alternatives rather than a stack.
        if (Collision->IsConcave())
        {
            Collision->Clear();
        }

        Collision->Primitives.Add(Primitive);
        SelectedPrimitive = Collision->Primitives.Num() - 1;

        RebuildHullWireframes();
        MarkChanged();
        EndTransaction();
        SettingsTable->MarkDirty();
    }

    void FMeshCollisionEditing::RemovePrimitiveAt(int32 Index)
    {
        SMeshCollision* Collision = GetCollision();
        if (Collision == nullptr || Index < 0 || Index >= Collision->Primitives.Num())
        {
            return;
        }

        BeginTransaction("Remove Collision Shape");

        Collision->Primitives.RemoveAt(Index);
        SelectedPrimitive = Constants::kIndexNone;

        RebuildHullWireframes();
        MarkChanged();
        EndTransaction();
    }

    void FMeshCollisionEditing::ClearAll()
    {
        SMeshCollision* Collision = GetCollision();
        if (Collision == nullptr)
        {
            return;
        }

        BeginTransaction("Clear Collision");
        Collision->Clear();
        SelectedPrimitive = Constants::kIndexNone;
        RebuildHullWireframes();
        MarkChanged();
        EndTransaction();
        SettingsTable->MarkDirty();
    }

    void FMeshCollisionEditing::DrawShapeList()
    {
        SMeshCollision* Collision = GetCollision();
        int32 PendingRemoval = Constants::kIndexNone;

        ImGui::BeginGroup();

        if (Collision == nullptr || (!Collision->IsConcave() && Collision->Primitives.IsEmpty()))
        {
            const ImVec2 Size(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight() * 2.0f);
            const ImVec2 Min = ImGui::GetCursorScreenPos();
            ImGui::Dummy(Size);
            ImGui::GetWindowDrawList()->AddRect(Min, ImVec2(Min.x + Size.x, Min.y + Size.y), EditorColors::U32(EditorColors::Border()),
                                                ImGui::GetStyle().FrameRounding + 3.0f, 1.0f);
            const char* Hint = "Click a shape above, or drag one here or onto the mesh";
            const ImVec2 HintSize = ImGui::CalcTextSize(Hint);
            ImGui::GetWindowDrawList()->AddText(ImVec2(Min.x + (Size.x - HintSize.x) * 0.5f, Min.y + (Size.y - HintSize.y) * 0.5f),
                                                EditorColors::U32(EditorColors::TextMuted()), Hint);
        }
        else if (Collision->IsConcave())
        {
            ImGui::TextColored(EditorColors::Success(), LE_ICON_VECTOR_TRIANGLE "  Exact Mesh");
            ImGui::SameLine();
            ImGui::TextDisabled("%d triangles", Collision->NumTriangles());
        }
        else
        {
            for (int32 i = 0; i < Collision->Primitives.Num(); ++i)
            {
                const SCollisionPrimitive& Primitive = Collision->Primitives[i];
                ImGui::PushID(i);

                const bool bSelected = (i == SelectedPrimitive);
                const float RowHeight = ImGui::GetFrameHeight();
                const float DeleteWidth = RowHeight;

                if (ImGui::Selectable("##Row", bSelected, ImGuiSelectableFlags_AllowOverlap, ImVec2(ImGui::GetContentRegionAvail().x - DeleteWidth, RowHeight)))
                {
                    SelectedPrimitive = i;
                }
                const bool bRowHovered = ImGui::IsItemHovered();

                if (ImGui::BeginPopupContextItem("##ShapeContext"))
                {
                    if (ImGui::MenuItem(LE_ICON_DELETE " Remove", "Del"))
                    {
                        PendingRemoval = i;
                    }
                    ImGui::EndPopup();
                }

                // Icon and name drawn over the row, so the selectable keeps the whole width for clicks.
                const ImVec2 RowMin = ImGui::GetItemRectMin();
                const float TextY = RowMin.y + (RowHeight - ImGui::GetTextLineHeight()) * 0.5f;
                ImDrawList* Draw = ImGui::GetWindowDrawList();
                Draw->AddText(ImVec2(RowMin.x + 6.0f, TextY), EditorColors::U32(bSelected ? EditorColors::Accent() : EditorColors::TextDim()),
                              PrimitiveIcon(Primitive.Type));
                Draw->AddText(ImVec2(RowMin.x + 6.0f + ImGui::GetFontSize() * 1.6f, TextY), EditorColors::U32(EditorColors::TextPrimary()),
                              PrimitiveTypeName(Primitive.Type));
                if (Primitive.Type == ECollisionPrimitiveType::ConvexHull)
                {
                    char Points[32];
                    snprintf(Points, sizeof(Points), "%d points", Primitive.HullPoints.Num());
                    const float NameEnd = RowMin.x + 6.0f + ImGui::GetFontSize() * 1.6f + ImGui::CalcTextSize(PrimitiveTypeName(Primitive.Type)).x;
                    Draw->AddText(ImVec2(NameEnd + ImGui::GetFontSize() * 0.75f, TextY), EditorColors::U32(EditorColors::TextMuted()), Points);
                }

                ImGui::SameLine(0.0f, 0.0f);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, (bRowHovered || bSelected) ? EditorColors::TextDim() : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                if (ImGui::Button(LE_ICON_DELETE, ImVec2(DeleteWidth, RowHeight)))
                {
                    PendingRemoval = i;
                }
                ImGui::PopStyleColor(2);
                ImGuiX::TextTooltip("{}", "Remove this shape");

                ImGui::PopID();
            }
        }

        ImGui::EndGroup();

        // The whole list takes palette drops, which add the same as clicking the tile.
        if (ImGui::BeginDragDropTarget())
        {
            int32 Dropped = 0;
            if (DragDrop::AcceptPaletteItem(CollisionPaletteName, Dropped))
            {
                AddGenerated((ECollisionGenerator)Dropped);
            }
            ImGui::EndDragDropTarget();
        }

        if (Collision != nullptr && Collision->HasCollision())
        {
            ImGui::Spacing();
            const int32 NumShapes = Collision->IsConcave() ? 1 : Collision->Primitives.Num();
            ImGui::TextDisabled("%d %s, %d hull points", NumShapes, NumShapes == 1 ? "shape" : "shapes", Collision->NumHullPoints());
            ImGui::SameLine();
            if (ImGui::SmallButton(LE_ICON_BROOM " Clear All"))
            {
                ClearAll();
            }
        }

        if (HasSelection() && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
        {
            PendingRemoval = SelectedPrimitive;
        }

        // Deferred, since erasing mid-iteration invalidates the loop and every row after it.
        if (PendingRemoval != Constants::kIndexNone)
        {
            RemovePrimitiveAt(PendingRemoval);
        }
    }

    void FMeshCollisionEditing::DrawPrimitiveDetails()
    {
        SyncDetailsTable();
        if (DetailsTarget != nullptr)
        {
            DetailsTable->DrawTree();
        }
        else
        {
            ImGui::TextDisabled("Select a shape in the list or the viewport.");
        }
    }

    void FMeshCollisionEditing::DrawSettings()
    {
        if (GetCollision() != nullptr)
        {
            SettingsTable->DrawTree();
        }
    }

    void FMeshCollisionEditing::SyncDetailsTable()
    {
        SMeshCollision* Collision = GetCollision();
        void* Target = HasSelection() ? &Collision->Primitives[SelectedPrimitive] : nullptr;

        // Comparing the address catches a vector reallocation as well as a selection change.
        if (Target != DetailsTarget)
        {
            DetailsTarget = Target;
            if (Target != nullptr)
            {
                DetailsTable->SetObject(Target, SCollisionPrimitive::StaticStruct());
                DetailsTable->MarkDirty();
            }
        }
    }

    void FMeshCollisionEditing::Tick(CWorld* World)
    {
        SMeshCollision* Collision = GetCollision();
        if (Collision == nullptr)
        {
            return;
        }

        SyncDetailsTable();

        if (bDrawShapes && World != nullptr)
        {
            DrawPrimitives(World);
        }
    }

    FMatrix4 FMeshCollisionEditing::GetPrimitiveMatrix(const SCollisionPrimitive& Primitive) const
    {
        return Math::Translate(FMatrix4(1.0f), Primitive.Center) * Math::ToMatrix4(FQuat(Math::Radians(Primitive.Rotation)));
    }

    void FMeshCollisionEditing::DrawPrimitive(CWorld* World, const SCollisionPrimitive& Primitive, int32 Index, const FVector4& Color, float Thickness)
    {
        const FMatrix4 Matrix = GetPrimitiveMatrix(Primitive);
        const FVector3 Center = Primitive.Center;
        FWorldDebugInterface& Debug = *World->GetDebugInterface();

        switch (Primitive.Type)
        {
        case ECollisionPrimitiveType::Box:
            {
                const FQuat Rotation(Math::Radians(Primitive.Rotation));
                Debug.DrawBox(Center, Primitive.HalfExtent, Rotation, Color, Thickness, false);
                if (bDrawSolid)
                {
                    Debug.DrawSolidBox(Center, Primitive.HalfExtent, Rotation, FillColor(Color), EDebugFillMode::XRay);
                }
            }
            break;

        case ECollisionPrimitiveType::Sphere:
            Debug.DrawSphere(Center, Primitive.Radius, Color, 16, Thickness, false);
            if (bDrawSolid)
            {
                Debug.DrawSolidSphere(Center, Primitive.Radius, FillColor(Color), EDebugFillMode::XRay);
            }
            break;

        case ECollisionPrimitiveType::Capsule:
            {
                const FVector3 Axis  = Math::Normalize(FVector3(Matrix[1]));
                const FVector3 Start = Center - Axis * Primitive.HalfHeight;
                const FVector3 End   = Center + Axis * Primitive.HalfHeight;
                Debug.DrawCapsule(Start, End, Primitive.Radius, Color, 16, Thickness, false);
                if (bDrawSolid)
                {
                    Debug.DrawSolidCapsule(Start, End, Primitive.Radius, FillColor(Color), EDebugFillMode::XRay);
                }
            }
            break;

        case ECollisionPrimitiveType::ConvexHull:
            {
                if (Index < 0 || Index >= HullDrawData.Num())
                {
                    break;
                }

                const FHullDrawData& Hull = HullDrawData[Index];
                for (int32 e = 0; e + 1 < Hull.Edges.Num(); e += 2)
                {
                    const FVector3 A = FVector3(Matrix * FVector4(Hull.Vertices[Hull.Edges[e]], 1.0f));
                    const FVector3 B = FVector3(Matrix * FVector4(Hull.Vertices[Hull.Edges[e + 1]], 1.0f));
                    Debug.DrawLine(A, B, Color, Thickness, false);
                }
                if (bDrawSolid)
                {
                    DrawSolidMesh(Debug, Hull.SolidVertices, Hull.SolidIndices, Matrix, FillColor(Color));
                }
            }
            break;
        }
    }

    void FMeshCollisionEditing::DrawPrimitives(CWorld* World)
    {
        SMeshCollision* Collision = GetCollision();
        // A baked triangle mesh replaces the primitives, so draw it alone to match what gets built.
        if (Collision->IsConcave())
        {
            for (int32 i = 0; i + 2 < Collision->TriangleIndices.Num(); i += 3)
            {
                const FVector3& A = Collision->TriangleVertices[Collision->TriangleIndices[i]];
                const FVector3& B = Collision->TriangleVertices[Collision->TriangleIndices[i + 1]];
                const FVector3& C = Collision->TriangleVertices[Collision->TriangleIndices[i + 2]];

                World->GetDebugInterface()->DrawLine(A, B, TriMeshColor, 1.0f, false);
                World->GetDebugInterface()->DrawLine(B, C, TriMeshColor, 1.0f, false);
                World->GetDebugInterface()->DrawLine(C, A, TriMeshColor, 1.0f, false);
            }
            if (bDrawSolid)
            {
                DrawSolidMesh(*World->GetDebugInterface(), Collision->TriangleVertices, Collision->TriangleIndices,
                              FMatrix4(1.0f), FillColor(TriMeshColor));
            }
            return;
        }

        for (int32 i = 0; i < Collision->Primitives.Num(); ++i)
        {
            const bool bSelected = (i == SelectedPrimitive);
            DrawPrimitive(World, Collision->Primitives[i], i, bSelected ? ShapeSelColor : ShapeColor, bSelected ? 3.0f : 1.5f);
        }
    }

    int32 FMeshCollisionEditing::PickPrimitive(const FVector3& RayOrigin, const FVector3& RayDirection)
    {
        SMeshCollision* Collision = GetCollision();
        if (Collision == nullptr || Collision->IsConcave())
        {
            return Constants::kIndexNone;
        }

        int32 Best = Constants::kIndexNone;
        float BestDistance = 1e30f;

        for (int32 i = 0; i < Collision->Primitives.Num(); ++i)
        {
            const SCollisionPrimitive& Primitive = Collision->Primitives[i];
            const FMatrix4 Matrix = GetPrimitiveMatrix(Primitive);

            float Distance = 0.0f;
            bool bHit = false;

            switch (Primitive.Type)
            {
            case ECollisionPrimitiveType::Sphere:
                bHit = ShapeHandles::RayHitsSphere(RayOrigin, RayDirection, Primitive.Center, Primitive.Radius, Distance);
                break;

            case ECollisionPrimitiveType::Capsule:
                {
                    const FVector3 Axis = Math::Normalize(FVector3(Matrix[1]));
                    bHit = ShapeHandles::RayHitsCapsule(RayOrigin, RayDirection, Primitive.Center - Axis * Primitive.HalfHeight,
                        Primitive.Center + Axis * Primitive.HalfHeight, Primitive.Radius, Distance);
                }
                break;

            case ECollisionPrimitiveType::ConvexHull:
                {
                    if (i >= HullDrawData.Num() || HullDrawData[i].Vertices.IsEmpty())
                    {
                        break;
                    }

                    FVector3 Min = HullDrawData[i].Vertices[0];
                    FVector3 Max = Min;
                    for (const FVector3& P : HullDrawData[i].Vertices)
                    {
                        Min = Math::Min(Min, P);
                        Max = Math::Max(Max, P);
                    }

                    const FMatrix4 HullMatrix = Matrix * Math::Translate(FMatrix4(1.0f), (Min + Max) * 0.5f);
                    bHit = ShapeHandles::RayHitsBox(RayOrigin, RayDirection, HullMatrix, (Max - Min) * 0.5f, Distance);
                }
                break;

            default:
                bHit = ShapeHandles::RayHitsBox(RayOrigin, RayDirection, Matrix, Primitive.HalfExtent, Distance);
                break;
            }

            if (bHit && Distance < BestDistance)
            {
                BestDistance = Distance;
                Best = i;
            }
        }

        return Best;
    }

    void FMeshCollisionEditing::GatherShapeHandles(TVector<FShapeHandle>& OutHandles)
    {
        SMeshCollision* Collision = GetCollision();
        OutHandles.Reset();
        if (!HasSelection())
        {
            return;
        }

        const SCollisionPrimitive& Primitive = Collision->Primitives[SelectedPrimitive];
        const FMatrix4 Matrix = GetPrimitiveMatrix(Primitive);

        const FVector3 Center = Primitive.Center;
        const FVector3 AxisX = Math::Normalize(FVector3(Matrix[0]));
        const FVector3 AxisY = Math::Normalize(FVector3(Matrix[1]));
        const FVector3 AxisZ = Math::Normalize(FVector3(Matrix[2]));

        switch (Primitive.Type)
        {
        case ECollisionPrimitiveType::Sphere:
            OutHandles.Emplace(EShapeHandle::Radius, Center + AxisX * Primitive.Radius, AxisX, Center);
            break;

        case ECollisionPrimitiveType::Capsule:
            OutHandles.Emplace(EShapeHandle::Radius,     Center + AxisX * Primitive.Radius,     AxisX, Center);
            OutHandles.Emplace(EShapeHandle::HalfHeight, Center + AxisY * Primitive.HalfHeight, AxisY, Center);
            break;

        case ECollisionPrimitiveType::Box:
            OutHandles.Emplace(EShapeHandle::ExtentX, Center + AxisX * Primitive.HalfExtent.x, AxisX, Center);
            OutHandles.Emplace(EShapeHandle::ExtentY, Center + AxisY * Primitive.HalfExtent.y, AxisY, Center);
            OutHandles.Emplace(EShapeHandle::ExtentZ, Center + AxisZ * Primitive.HalfExtent.z, AxisZ, Center);
            break;

        default:
            // A hull's shape is baked geometry; there is no dimension to drag.
            break;
        }
    }

    void FMeshCollisionEditing::ApplyShapeHandleDrag(const FShapeHandle& Handle, const FVector3& RayOrigin, const FVector3& RayDirection)
    {
        SMeshCollision* Collision = GetCollision();
        if (!HasSelection())
        {
            return;
        }

        float AxisDistance = 0.0f;
        if (!ShapeHandles::ClosestParamOnAxisToRay(Handle.Anchor, Handle.Axis, RayOrigin, RayDirection, AxisDistance))
        {
            return;
        }

        SCollisionPrimitive& Primitive = Collision->Primitives[SelectedPrimitive];
        const float Positive = Math::Max(AxisDistance, 0.001f);

        switch (Handle.Type)
        {
        case EShapeHandle::Radius:     Primitive.Radius = Positive; break;
        case EShapeHandle::HalfHeight: Primitive.HalfHeight = Math::Max(AxisDistance, 0.0f); break;
        case EShapeHandle::ExtentX:    Primitive.HalfExtent.x = Positive; break;
        case EShapeHandle::ExtentY:    Primitive.HalfExtent.y = Positive; break;
        case EShapeHandle::ExtentZ:    Primitive.HalfExtent.z = Positive; break;
        default: return;
        }

        MarkChanged();
    }

    void FMeshCollisionEditing::ApplyGizmoMatrix(const FMatrix4& NewMatrix)
    {
        SMeshCollision* Collision = GetCollision();
        if (!HasSelection())
        {
            return;
        }

        SCollisionPrimitive& Primitive = Collision->Primitives[SelectedPrimitive];
        Primitive.Center   = FVector3(NewMatrix[3]);
        Primitive.Rotation = Math::Degrees(Math::EulerAngles(Math::Normalize(Math::ToQuat(NewMatrix))));

        MarkChanged();
    }

    FMatrix4 FMeshCollisionEditing::GetGizmoMatrix()
    {
        SMeshCollision* Collision = GetCollision();
        return HasSelection() ? GetPrimitiveMatrix(Collision->Primitives[SelectedPrimitive]) : FMatrix4(1.0f);
    }

    void FMeshCollisionEditing::PickShapeAtRay(const FVector3& RayOrigin, const FVector3& RayDirection)
    {
        SelectedPrimitive = PickPrimitive(RayOrigin, RayDirection);
    }

    bool FMeshCollisionEditing::MakeDroppedPrimitive(CWorld* World, const ImVec2& ViewportOrigin, const ImVec2& ViewportSize,
                                                      ECollisionGenerator Generator, SCollisionPrimitive& OutPrimitive) const
    {
        ECollisionPrimitiveType Type;
        switch (Generator)
        {
        case ECollisionGenerator::Box:     Type = ECollisionPrimitiveType::Box;     break;
        case ECollisionGenerator::Sphere:  Type = ECollisionPrimitiveType::Sphere;  break;
        case ECollisionGenerator::Capsule: Type = ECollisionPrimitiveType::Capsule; break;
        default: return false;
        }

        FVector3 RayOrigin, RayDirection;
        if (!ShapeHandles::BuildViewportRay(World, ViewportOrigin, ViewportSize, ImGui::GetMousePos(), RayOrigin, RayDirection))
        {
            return false;
        }

        // The point under the cursor at the mesh's depth, so the shape lands over the part the cursor is on.
        const CStaticMesh* SourceMesh = Mesh.Get();
        const SAABB Bounds = (SourceMesh != nullptr && SourceMesh->GetAABB().IsValid()) ? SourceMesh->GetAABB() : SAABB(FVector3(-0.5f), FVector3(0.5f));
        const float Depth = Math::Max(Math::Dot(Bounds.GetCenter() - RayOrigin, RayDirection), 0.1f);
        const float Size  = Math::Max(Bounds.MaxScale() * 0.125f, 0.025f);

        OutPrimitive            = SCollisionPrimitive();
        OutPrimitive.Type       = Type;
        OutPrimitive.Center     = RayOrigin + RayDirection * Depth;
        OutPrimitive.HalfExtent = FVector3(Size);
        OutPrimitive.Radius     = Type == ECollisionPrimitiveType::Capsule ? Size * 0.5f : Size;
        OutPrimitive.HalfHeight = Size;
        return true;
    }

    void FMeshCollisionEditing::TickViewportDrop(CWorld* World, const ImVec2& ViewportOrigin, const ImVec2& ViewportSize)
    {
        const DragDrop::FPayload* Peek = DragDrop::PeekPayload();
        if (World == nullptr || Peek == nullptr || Peek->Kind != DragDrop::EPayloadKind::PaletteItem || Peek->PaletteName != CollisionPaletteName)
        {
            return;
        }

        const ImRect ViewportRect(ViewportOrigin, ImVec2(ViewportOrigin.x + ViewportSize.x, ViewportOrigin.y + ViewportSize.y));
        if (!ImGui::BeginDragDropTargetCustom(ViewportRect, ImGui::GetID("##CollisionDrop")))
        {
            return;
        }

        const ECollisionGenerator Generator = (ECollisionGenerator)Peek->PaletteIndex;
        SCollisionPrimitive Placed;
        const bool bPlaced = MakeDroppedPrimitive(World, ViewportOrigin, ViewportSize, Generator, Placed);
        if (bPlaced)
        {
            DrawPrimitive(World, Placed, Constants::kIndexNone, ShapeDropColor, 2.5f);
        }

        int32 Dropped = 0;
        if (DragDrop::AcceptPaletteItem(CollisionPaletteName, Dropped))
        {
            if (bPlaced)
            {
                AddPrimitive(Placed);
            }
            else
            {
                AddGenerated(Generator);
            }
        }

        ImGui::EndDragDropTarget();
    }

    void FMeshCollisionEditing::TickViewport(CWorld* World, const ImVec2& ViewportOrigin, const ImVec2& ViewportSize,
                                              bool bViewportHovered, bool bSuppressClicks)
    {
        SMeshCollision* Collision = GetCollision();
        TickViewportDrop(World, ViewportOrigin, ViewportSize);

        SCameraComponent* Camera = World != nullptr ? World->GetCamera()->GetActiveCamera() : nullptr;
        if (Camera == nullptr || Collision == nullptr || !bDrawShapes)
        {
            HandleState.ActiveHandle = EShapeHandle::None;
            return;
        }

        if (bViewportHovered && ImGui::IsKeyPressed(ImGuiKey_Space))
        {
            GizmoOp = (GizmoOp == ImGuizmo::TRANSLATE) ? ImGuizmo::ROTATE : ImGuizmo::TRANSLATE;
        }

        FMatrix4 Projection = Camera->GetProjectionMatrix();
        Projection[1][1] *= -1.0f;

        FShapeHandleFrame Frame;
        Frame.World            = World;
        Frame.ViewMatrix       = Camera->GetViewMatrix();
        Frame.Projection       = Projection;
        Frame.ViewportOrigin   = ViewportOrigin;
        Frame.ViewportSize     = ViewportSize;
        Frame.GizmoOp          = GizmoOp;
        Frame.bViewportHovered = bViewportHovered;
        Frame.bSuppressClicks  = bSuppressClicks;
        Frame.bHasGizmoTarget  = HasSelection();

        TickShapeHandles(*this, HandleState, Frame);
    }

    void FMeshCollisionEditing::DrawViewMenuItems()
    {
        ImGui::MenuItem(LE_ICON_CUBE_OUTLINE " Show Collision", nullptr, &bDrawShapes);
        ImGui::MenuItem(LE_ICON_CUBE " Solid Shapes", nullptr, &bDrawSolid);

        if (ImGui::MenuItem("Move Shape", "Space", GizmoOp == ImGuizmo::TRANSLATE))
        {
            GizmoOp = ImGuizmo::TRANSLATE;
        }
        if (ImGui::MenuItem("Rotate Shape", "Space", GizmoOp == ImGuizmo::ROTATE))
        {
            GizmoOp = ImGuizmo::ROTATE;
        }
    }
}
