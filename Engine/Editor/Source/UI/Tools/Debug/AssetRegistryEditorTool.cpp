#include "AssetRegistryEditorTool.h"

#include <EASTL/sort.h>

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Mesh/Mesh.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectArray.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Core/Serialization/Archiver.h"
#include "FileSystem/FileSystem.h"
#include "UI/Tools/EditorToolContext.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina
{
    namespace
    {
        // Read-only archive: runs Serialize to harvest referenced CObjects without writing
        // (Serialize(void*,len) is a no-op). Like the package save ref builder, no same-package limit.
        class FReferenceCollectorArchive final : public FArchive
        {
        public:

            using FArchive::operator<<;

            explicit FReferenceCollectorArchive(THashSet<CObject*>& InCollected)
                : Collected(InCollected)
            {
                SetFlag(EArchiverFlags::Writing);
            }

            FArchive& operator<<(CObject*& Value) override
            {
                if (Value != nullptr)
                {
                    Collected.insert(Value);
                }
                return *this;
            }

            FArchive& operator<<(FObjectHandle& Value) override
            {
                if (CObject* Resolved = Value.Resolve())
                {
                    Collected.insert(Resolved);
                }
                return *this;
            }

        private:

            THashSet<CObject*>& Collected;
        };

        const char* StatusLabel(bool bLoaded)
        {
            return bLoaded ? LE_ICON_CHECK_CIRCLE " Loaded" : LE_ICON_CIRCLE_OUTLINE " Unloaded";
        }

        ImVec4 StatusColor(bool bLoaded)
        {
            return bLoaded ? ImVec4(0.45f, 0.85f, 0.5f, 1.0f) : ImVec4(0.55f, 0.55f, 0.58f, 1.0f);
        }

        ImVec4 CategoryColor(const FName& Class)
        {
            // Stable hue per class so categories are visually distinct.
            const uint32 Hash = Class.GetID();
            const float Hue = (Hash % 360) / 360.0f;
            ImVec4 Color;
            ImGui::ColorConvertHSVtoRGB(Hue, 0.5f, 0.95f, Color.x, Color.y, Color.z);
            Color.w = 1.0f;
            return Color;
        }
    }

    void FAssetRegistryEditorTool::OnInitialize()
    {
        CreateToolWindow("Asset Registry", [this](bool bIsFocused)
        {
            DrawWindow(bIsFocused);
        });
    }

    void FAssetRegistryEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
    }

    void FAssetRegistryEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("What this is",
            "Every asset the registry discovered on disk, grouped by class. Shows which are resident in "
            "memory right now versus only on disk, their live strong ref-count, an estimate of the CPU-side "
            "bulk data they hold, and their on-disk size.");
        DrawHelpTextRow("Loaded vs Unloaded",
            "An asset is Loaded when a CObject for its GUID exists in memory. Unloaded assets have no "
            "ref-count or CPU footprint until something requests them.");
        DrawHelpTextRow("Ref Count",
            "The object's strong ref-count: how many TObjectPtrs keep it alive (components, materials, the "
            "open editor, etc). Zero on a loaded asset means it is a candidate for unloading.");
        DrawHelpTextRow("Referenced By",
            "Selecting a loaded asset scans every live object for reflected references to it. This catches "
            "asset-to-asset links (a material's textures, a mesh's materials); ECS component references are "
            "not reflected objects, so they show up in the ref-count but not this list.");
        DrawHelpTextRow("Refresh",
            "Re-reads on-disk sizes and recomputes the referencer list. Everything else is live each frame.");
        DrawHelpTextRow("Open",
            "Double-click any row (or use the row's context menu) to open it in its asset editor.");
    }

    uint64 FAssetRegistryEditorTool::EstimateCpuBytes(CObject* Asset)
    {
        if (Asset == nullptr)
        {
            return 0;
        }

        if (const CTexture* Texture = Cast<CTexture>(Asset))
        {
            return Texture->TextureResource ? Texture->TextureResource->CalcTotalSizeBytes() : 0;
        }

        if (const CMesh* Mesh = Cast<CMesh>(Asset))
        {
            const FMeshResource& MR = Mesh->GetMeshResource();
            uint64 Bytes = 0;

            Bytes += MR.Positions.capacity()  * sizeof(FVector3);
            Bytes += MR.Normals.capacity()    * sizeof(uint32);
            Bytes += MR.Tangents.capacity()   * sizeof(uint32);
            Bytes += MR.UVs.capacity()        * sizeof(uint32);
            Bytes += MR.Colors.capacity()     * sizeof(uint32);
            Bytes += MR.JointIndices.capacity() * sizeof(FU8Vector4);
            Bytes += MR.JointWeights.capacity() * sizeof(FU8Vector4);
            Bytes += MR.Indices.capacity()    * sizeof(uint32);
            Bytes += MR.GeometrySurfaces.capacity() * sizeof(FGeometrySurface);

            Bytes += MR.MeshletData.Meshlets.capacity()              * sizeof(FMeshlet);
            Bytes += MR.MeshletData.MeshletVertices.capacity()       * sizeof(FMeshletVertex);
            Bytes += MR.MeshletData.MeshletSkinnedVertices.capacity()* sizeof(FMeshletSkinnedVertex);
            Bytes += MR.MeshletData.MeshletTriangles.capacity()      * sizeof(uint32);
            Bytes += MR.MeshletData.MeshletSpheres.capacity()        * sizeof(FMeshletSphere);
            Bytes += MR.MeshletData.MeshletCones.capacity()          * sizeof(FMeshletCone);

            return Bytes;
        }

        if (const CMaterial* Material = Cast<CMaterial>(Asset))
        {
            uint64 Bytes = 0;
            Bytes += Material->PixelShaderBinaries.capacity()              * sizeof(uint32);
            Bytes += Material->VertexShaderBinaries.capacity()             * sizeof(uint32);
            Bytes += Material->Parameters.capacity()                       * sizeof(FMaterialParameter);
            return Bytes;
        }

        return 0;
    }

    void FAssetRegistryEditorTool::RebuildReferencers(CObject* Target)
    {
        Referencers.clear();
        CachedReferencerTarget = Target ? Target->GetGUID() : FGuid();

        if (Target == nullptr)
        {
            return;
        }

        THashSet<CObject*> Collected;
        GObjectArray.ForEachObject([&](CObjectBase* Base, int32)
        {
            CObject* Candidate = static_cast<CObject*>(Base);
            if (Candidate == Target || Candidate->GetClass() == nullptr)
            {
                return;
            }

            Collected.clear();
            FReferenceCollectorArchive Archive(Collected);
            Candidate->Serialize(Archive);

            if (Collected.find(Target) != Collected.end())
            {
                FReferencer Ref;
                Ref.Name    = Candidate->GetName();
                Ref.Class   = Candidate->GetClass() ? Candidate->GetClass()->GetName() : FName("None");
                Ref.Package = Candidate->GetPackage() ? Candidate->GetPackage()->GetName() : FName("None");
                Referencers.push_back(Ref);
            }
        });

        eastl::sort(Referencers.begin(), Referencers.end(), [](const FReferencer& A, const FReferencer& B)
        {
            return A.Name.ToString() < B.Name.ToString();
        });
    }

    bool FAssetRegistryEditorTool::PassesFilter(const FAssetRow& Row) const
    {
        if (bShowLoadedOnly && Row.Loaded == nullptr)
        {
            return false;
        }

        if (!CategoryFilter.empty() && Row.Class.ToString() != CategoryFilter)
        {
            return false;
        }

        if (!SearchFilter.empty())
        {
            FString Name = Row.Name.ToString();
            eastl::transform(Name.begin(), Name.end(), Name.begin(), [](char C){ return (char)tolower(C); });
            FString Needle = SearchFilter;
            eastl::transform(Needle.begin(), Needle.end(), Needle.begin(), [](char C){ return (char)tolower(C); });
            if (Name.find(Needle) == FString::npos)
            {
                return false;
            }
        }

        return true;
    }

    void FAssetRegistryEditorTool::DrawWindow(bool bIsFocused)
    {
        // Resolve current registry state into rows once per frame.
        TVector<FAssetRow> Rows;
        {
            const FAssetDataMap& Assets = FAssetRegistry::Get().GetAssets();
            Rows.reserve(Assets.size());

            for (const TUniquePtr<FAssetData>& Data : Assets)
            {
                FAssetRow Row;
                Row.GUID   = Data->AssetGUID;
                Row.Name   = Data->AssetName;
                Row.Class  = Data->AssetClass;
                Row.Path   = Data->Path;
                Row.Loaded = FindObject<CObject>(Data->AssetGUID);
                Row.RefCount = Row.Loaded ? Row.Loaded->GetStrongRefCount() : 0;
                Row.CpuBytes = EstimateCpuBytes(Row.Loaded);

                auto It = DiskSizeCache.find(Row.GUID);
                if (It == DiskSizeCache.end())
                {
                    Row.DiskBytes = VFS::Size(Row.Path);
                    DiskSizeCache.emplace(Row.GUID, Row.DiskBytes);
                }
                else
                {
                    Row.DiskBytes = It->second;
                }

                Rows.push_back(Row);
            }
        }

        DrawStatsBar(Rows);
        ImGui::Spacing();
        DrawFilterBar();
        ImGui::Spacing();

        ImGui::BeginChild("##Body", ImVec2(0, 0), false);
        {
            const float DetailsWidth = 340.0f;
            ImGui::BeginChild("##TablePane", ImVec2(ImGui::GetContentRegionAvail().x - DetailsWidth, 0), false);
            {
                DrawAssetTable(Rows);
            }
            ImGui::EndChild();

            ImGui::SameLine();

            ImGui::BeginChild("##DetailsPane", ImVec2(0, 0), true);
            {
                DrawDetailsPanel(Rows);
            }
            ImGui::EndChild();
        }
        ImGui::EndChild();
    }

    void FAssetRegistryEditorTool::DrawStatsBar(const TVector<FAssetRow>& Rows)
    {
        uint32 Loaded = 0;
        uint64 TotalCpu = 0;
        uint64 TotalDisk = 0;
        for (const FAssetRow& Row : Rows)
        {
            if (Row.Loaded)
            {
                ++Loaded;
                TotalCpu += Row.CpuBytes;
            }
            TotalDisk += Row.DiskBytes;
        }
        const uint32 Total = (uint32)Rows.size();

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.14f, 0.18f, 1.0f));
        ImGui::BeginChild("##StatsBar", ImVec2(0, 64.0f), true, ImGuiWindowFlags_NoScrollbar);
        {
            auto Stat = [](const char* Label, const ImVec4& Color, const FString& Value)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, Color);
                ImGui::TextUnformatted(Label);
                ImGui::PopStyleColor();
                ImGui::Text("%s", Value.c_str());
            };

            ImGui::Columns(5, nullptr, false);
            Stat("TOTAL ASSETS",  ImVec4(0.7f, 0.8f, 1.0f, 1.0f), eastl::to_string(Total));
            ImGui::NextColumn();
            Stat("LOADED",        ImVec4(0.45f, 0.85f, 0.5f, 1.0f), eastl::to_string(Loaded));
            ImGui::NextColumn();
            Stat("UNLOADED",      ImVec4(0.65f, 0.65f, 0.68f, 1.0f), eastl::to_string(Total - Loaded));
            ImGui::NextColumn();
            Stat("CPU MEMORY",    ImVec4(1.0f, 0.75f, 0.4f, 1.0f), ImGuiX::FormatSize(TotalCpu));
            ImGui::NextColumn();
            Stat("ON DISK",       ImVec4(0.7f, 0.7f, 0.9f, 1.0f), ImGuiX::FormatSize(TotalDisk));
            ImGui::Columns(1);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    void FAssetRegistryEditorTool::DrawFilterBar()
    {
        if (ImGui::Button(LE_ICON_REFRESH " Refresh"))
        {
            DiskSizeCache.clear();
            CachedReferencerTarget = FGuid();
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::InputTextWithHint("##Search", LE_ICON_MAGNIFY " Search assets...", SearchBuffer, IM_ARRAYSIZE(SearchBuffer)))
        {
            SearchFilter = SearchBuffer;
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0f);
        const char* Preview = CategoryFilter.empty() ? "All Categories" : CategoryFilter.c_str();
        if (ImGui::BeginCombo("##Category", Preview))
        {
            if (ImGui::Selectable("All Categories", CategoryFilter.empty()))
            {
                CategoryFilter.clear();
            }

            // Gather distinct categories from the registry.
            TVector<FString> Categories;
            for (const TUniquePtr<FAssetData>& Data : FAssetRegistry::Get().GetAssets())
            {
                FString Class = Data->AssetClass.ToString();
                if (eastl::find(Categories.begin(), Categories.end(), Class) == Categories.end())
                {
                    Categories.push_back(Class);
                }
            }
            eastl::sort(Categories.begin(), Categories.end());

            for (const FString& Class : Categories)
            {
                if (ImGui::Selectable(Class.c_str(), CategoryFilter == Class))
                {
                    CategoryFilter = Class;
                }
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        ImGui::Checkbox("Group by Category", &bGroupByCategory);
        ImGui::SameLine();
        ImGui::Checkbox("Loaded Only", &bShowLoadedOnly);
    }

    void FAssetRegistryEditorTool::DrawAssetTableRows(const TVector<const FAssetRow*>& Rows)
    {
        ImGuiListClipper Clipper;
        Clipper.Begin((int)Rows.size());
        while (Clipper.Step())
        {
            for (int i = Clipper.DisplayStart; i < Clipper.DisplayEnd; ++i)
            {
                const FAssetRow& Row = *Rows[i];
                ImGui::TableNextRow();
                ImGui::PushID(i);

                ImGui::TableSetColumnIndex(0);
                const bool bSelected = (SelectedGUID == Row.GUID);
                if (ImGui::Selectable(Row.Name.c_str(), bSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
                {
                    SelectedGUID = Row.GUID;
                    if (ImGui::IsMouseDoubleClicked(0) && ToolContext)
                    {
                        ToolContext->OpenAssetEditor(Row.GUID);
                    }
                }

                if (ImGui::BeginPopupContextItem("##RowCtx"))
                {
                    if (ImGui::MenuItem(LE_ICON_FILE " Open Asset"))
                    {
                        if (ToolContext)
                        {
                            ToolContext->OpenAssetEditor(Row.GUID);
                        }
                    }
                    if (ImGui::MenuItem("Copy Name"))
                    {
                        ImGui::SetClipboardText(Row.Name.c_str());
                    }
                    if (ImGui::MenuItem("Copy Path"))
                    {
                        ImGui::SetClipboardText(Row.Path.c_str());
                    }
                    if (ImGui::MenuItem("Copy GUID"))
                    {
                        ImGui::SetClipboardText(Row.GUID.ToString().c_str());
                    }
                    ImGui::EndPopup();
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::PushStyleColor(ImGuiCol_Text, CategoryColor(Row.Class));
                ImGui::TextUnformatted(Row.Class.c_str());
                ImGui::PopStyleColor();

                ImGui::TableSetColumnIndex(2);
                ImGui::PushStyleColor(ImGuiCol_Text, StatusColor(Row.Loaded != nullptr));
                ImGui::TextUnformatted(StatusLabel(Row.Loaded != nullptr));
                ImGui::PopStyleColor();

                ImGui::TableSetColumnIndex(3);
                if (Row.Loaded)
                {
                    const ImVec4 RefColor = Row.RefCount > 0 ? ImVec4(0.9f, 0.9f, 0.9f, 1.0f) : ImVec4(0.9f, 0.6f, 0.3f, 1.0f);
                    ImGui::PushStyleColor(ImGuiCol_Text, RefColor);
                    ImGui::Text("%d", Row.RefCount);
                    ImGui::PopStyleColor();
                }
                else
                {
                    ImGui::TextDisabled("-");
                }

                ImGui::TableSetColumnIndex(4);
                if (Row.Loaded)
                {
                    ImGui::TextUnformatted(ImGuiX::FormatSize(Row.CpuBytes).c_str());
                }
                else
                {
                    ImGui::TextDisabled("-");
                }

                ImGui::TableSetColumnIndex(5);
                ImGui::TextUnformatted(ImGuiX::FormatSize(Row.DiskBytes).c_str());

                ImGui::PopID();
            }
        }
    }

    void FAssetRegistryEditorTool::DrawAssetTable(const TVector<FAssetRow>& Rows)
    {
        // Base flags for both layouts. ScrollY only on the flat table; grouped tables auto-size
        // and let the outer pane scroll, else the first table fills the region and shoves the rest.
        constexpr ImGuiTableFlags TableFlags =
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_SizingStretchProp;

        auto SetupColumns = [](bool bFreezeHeader)
        {
            ImGui::TableSetupColumn("Name",     ImGuiTableColumnFlags_WidthStretch, 0.32f);
            ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthStretch, 0.20f);
            ImGui::TableSetupColumn("Status",   ImGuiTableColumnFlags_WidthStretch, 0.16f);
            ImGui::TableSetupColumn("Refs",     ImGuiTableColumnFlags_WidthStretch, 0.08f);
            ImGui::TableSetupColumn("CPU Mem",  ImGuiTableColumnFlags_WidthStretch, 0.12f);
            ImGui::TableSetupColumn("On Disk",  ImGuiTableColumnFlags_WidthStretch, 0.12f);
            if (bFreezeHeader)
            {
                ImGui::TableSetupScrollFreeze(0, 1);
            }
            ImGui::TableHeadersRow();
        };

        if (bGroupByCategory)
        {
            // Bucket filtered rows by category.
            THashMap<FString, TVector<const FAssetRow*>> Buckets;
            for (const FAssetRow& Row : Rows)
            {
                if (PassesFilter(Row))
                {
                    Buckets[Row.Class.ToString()].push_back(&Row);
                }
            }

            TVector<FString> Order;
            for (auto& Pair : Buckets)
            {
                Order.push_back(Pair.first);
            }
            eastl::sort(Order.begin(), Order.end());

            for (const FString& Category : Order)
            {
                TVector<const FAssetRow*>& Bucket = Buckets[Category];

                uint32 LoadedInCat = 0;
                uint64 CpuInCat = 0;
                for (const FAssetRow* R : Bucket)
                {
                    if (R->Loaded)
                    {
                        ++LoadedInCat;
                        CpuInCat += R->CpuBytes;
                    }
                }

                eastl::sort(Bucket.begin(), Bucket.end(), [](const FAssetRow* A, const FAssetRow* B)
                {
                    return A->Name.ToString() < B->Name.ToString();
                });

                ImGui::PushStyleColor(ImGuiCol_Text, CategoryColor(FName(Category)));
                FString Header = Category + "  (" + eastl::to_string(LoadedInCat) + "/" +
                    eastl::to_string(Bucket.size()) + " loaded, " + FString(ImGuiX::FormatSize(CpuInCat).c_str()) + ")";
                const bool bOpen = ImGui::CollapsingHeader(Header.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
                ImGui::PopStyleColor();

                if (bOpen)
                {
                    FString TableID = "##Table_" + Category;
                    if (ImGui::BeginTable(TableID.c_str(), 6, TableFlags))
                    {
                        SetupColumns(false);
                        DrawAssetTableRows(Bucket);
                        ImGui::EndTable();
                    }
                    ImGui::Spacing();
                }
            }
        }
        else
        {
            TVector<const FAssetRow*> Filtered;
            for (const FAssetRow& Row : Rows)
            {
                if (PassesFilter(Row))
                {
                    Filtered.push_back(&Row);
                }
            }
            eastl::sort(Filtered.begin(), Filtered.end(), [](const FAssetRow* A, const FAssetRow* B)
            {
                return A->Name.ToString() < B->Name.ToString();
            });

            if (ImGui::BeginTable("##AssetTable", 6, TableFlags | ImGuiTableFlags_ScrollY, ImVec2(0, 0)))
            {
                SetupColumns(true);
                DrawAssetTableRows(Filtered);
                ImGui::EndTable();
            }
        }
    }

    void FAssetRegistryEditorTool::DrawDetailsPanel(const TVector<FAssetRow>& Rows)
    {
        const FAssetRow* Selected = nullptr;
        for (const FAssetRow& Row : Rows)
        {
            if (Row.GUID == SelectedGUID)
            {
                Selected = &Row;
                break;
            }
        }

        if (Selected == nullptr)
        {
            ImGui::TextDisabled("Select an asset to view details");
            return;
        }

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.9f, 1.0f, 1.0f));
        ImGui::TextUnformatted(LE_ICON_DATABASE " Asset Details");
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Spacing();

        auto Field = [](const char* Label, const FString& Value, const ImVec4& Color = ImVec4(0.85f, 0.85f, 0.85f, 1.0f))
        {
            ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.68f, 1.0f), "%s", Label);
            ImGui::PushStyleColor(ImGuiCol_Text, Color);
            ImGui::TextWrapped("%s", Value.c_str());
            ImGui::PopStyleColor();
            ImGui::Spacing();
        };

        Field("Name",  Selected->Name.ToString());
        Field("Class", Selected->Class.ToString(), CategoryColor(Selected->Class));
        Field("Path",  FString(Selected->Path.c_str()));
        Field("GUID",  Selected->GUID.ToString(), ImVec4(0.55f, 0.55f, 0.55f, 1.0f));

        const bool bLoaded = Selected->Loaded != nullptr;
        Field("Status", StatusLabel(bLoaded), StatusColor(bLoaded));
        Field("On Disk", ImGuiX::FormatSize(Selected->DiskBytes).c_str());

        if (bLoaded)
        {
            Field("CPU Memory", ImGuiX::FormatSize(Selected->CpuBytes).c_str(), ImVec4(1.0f, 0.75f, 0.4f, 1.0f));
            Field("Ref Count", eastl::to_string(Selected->RefCount),
                Selected->RefCount > 0 ? ImVec4(0.85f, 0.85f, 0.85f, 1.0f) : ImVec4(0.9f, 0.6f, 0.3f, 1.0f));

            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.9f, 1.0f, 1.0f));
            ImGui::TextUnformatted(LE_ICON_LINK " Referenced By");
            ImGui::PopStyleColor();
            ImGui::Separator();

            if (CachedReferencerTarget != Selected->GUID)
            {
                RebuildReferencers(Selected->Loaded);
            }

            if (Referencers.empty())
            {
                ImGui::TextDisabled("No reflected object references.");
                ImGui::TextDisabled("(ECS components still count toward ref-count.)");
            }
            else if (ImGui::BeginTable("##Referencers", 2,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
                ImVec2(0, 0)))
            {
                ImGui::TableSetupColumn("Object");
                ImGui::TableSetupColumn("Class");
                ImGui::TableHeadersRow();

                for (const FReferencer& Ref : Referencers)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(Ref.Name.c_str());
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("Package: %s", Ref.Package.c_str());
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::PushStyleColor(ImGuiCol_Text, CategoryColor(Ref.Class));
                    ImGui::TextUnformatted(Ref.Class.c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::EndTable();
            }
        }
        else
        {
            ImGui::Spacing();
            ImGui::TextDisabled("Not resident in memory.");
            ImGui::TextDisabled("No ref-count, footprint, or referencers");
            ImGui::TextDisabled("until something loads it.");
        }
    }
}
