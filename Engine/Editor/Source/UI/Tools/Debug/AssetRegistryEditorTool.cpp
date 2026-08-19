#include "AssetRegistryEditorTool.h"
#include "Core/CoreEditorDelegates.h"

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
#include "Tools/UI/ImGui/EditorColors.h"
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
        DrawHelpTextRow("Selection",
            "Click selects, Ctrl+click toggles, Shift+click selects a range (across category groups), "
            "Ctrl+A selects everything currently visible, Escape clears. The range and Ctrl+A both "
            "respect the active filters -- they only ever touch what you can see.");
        DrawHelpTextRow("Types filter",
            "Per-class checkboxes. A class you have never touched is visible, so newly imported asset "
            "types are never silently hidden.");
        DrawHelpTextRow("Resave",
            "Rewrites the selected assets' packages -- or, with nothing selected, everything matching the "
            "current filters. Unlike File > Save All this saves packages whether or not they are dirty, "
            "and loads anything not in memory, which is what upgrades unchanged assets to a new "
            "serialization format (e.g. splitting texture mips for streaming). It runs a few packages "
            "per frame so the editor stays responsive, and can be stopped part-way.");
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
            Bytes += MR.JointIndices.capacity() * sizeof(FU16Vector4);
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

        // Absent == visible: only classes the user has explicitly unticked are stored, so an asset type
        // that appears after the filter was last touched is not silently hidden.
        auto TypeIt = TypeVisibility.find(Row.Class);
        if (TypeIt != TypeVisibility.end() && !TypeIt->second)
        {
            return false;
        }

        if (!SearchFilter.empty() && !ImGuiX::PassSearchFilter(SearchFilter, Row.Name.ToString()))
        {
            return false;
        }

        return true;
    }

    uint32 FAssetRegistryEditorTool::CountHiddenTypes() const
    {
        uint32 Hidden = 0;
        for (const auto& Pair : TypeVisibility)
        {
            if (!Pair.second)
            {
                ++Hidden;
            }
        }
        return Hidden;
    }

    void FAssetRegistryEditorTool::BuildVisibleRows(const TVector<FAssetRow>& Rows)
    {
        VisibleRows.clear();
        VisibleGroups.clear();

        auto ByName = [](const FAssetRow* A, const FAssetRow* B)
        {
            return A->Name.ToString() < B->Name.ToString();
        };

        if (bGroupByCategory)
        {
            THashMap<FString, TVector<const FAssetRow*>> Buckets;
            for (const FAssetRow& Row : Rows)
            {
                if (PassesFilter(Row))
                {
                    Buckets[Row.Class.ToString()].push_back(&Row);
                }
            }

            TVector<FString> Order;
            Order.reserve(Buckets.size());
            for (auto& Pair : Buckets)
            {
                Order.push_back(Pair.first);
            }
            eastl::sort(Order.begin(), Order.end());

            // Flattened in the exact order the groups are drawn, so a VisibleRows index means the same
            // thing to shift-range as it does on screen.
            for (const FString& Category : Order)
            {
                TVector<const FAssetRow*>& Bucket = Buckets[Category];
                eastl::sort(Bucket.begin(), Bucket.end(), ByName);

                FRowGroup Group;
                Group.Category = Category;
                Group.Start    = (uint32)VisibleRows.size();
                Group.Count    = (uint32)Bucket.size();
                VisibleGroups.push_back(Move(Group));

                VisibleRows.insert(VisibleRows.end(), Bucket.begin(), Bucket.end());
            }
        }
        else
        {
            for (const FAssetRow& Row : Rows)
            {
                if (PassesFilter(Row))
                {
                    VisibleRows.push_back(&Row);
                }
            }
            eastl::sort(VisibleRows.begin(), VisibleRows.end(), ByName);
        }
    }

    void FAssetRegistryEditorTool::HandleSelectionShortcuts()
    {
        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        {
            return;
        }

        // IsAnyItemActive covers the search box: Ctrl+A there means "select the text", and stealing it
        // to select every asset in the project would be a nasty surprise.
        if (ImGui::IsAnyItemActive())
        {
            return;
        }

        const ImGuiIO& IO = ImGui::GetIO();

        if (IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false))
        {
            SelectedGUIDs.clear();
            SelectedGUIDs.reserve(VisibleRows.size());
            for (const FAssetRow* Row : VisibleRows)
            {
                SelectedGUIDs.insert(Row->GUID);
            }

            if (!VisibleRows.empty())
            {
                SelectedGUID = VisibleRows.front()->GUID;
                RangeAnchor  = 0;
            }
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !SelectedGUIDs.empty())
        {
            SelectedGUIDs.clear();
            RangeAnchor = INDEX_NONE;
        }
    }

    void FAssetRegistryEditorTool::ApplyRowClick(uint32 VisibleIndex)
    {
        if (VisibleIndex >= (uint32)VisibleRows.size())
        {
            return;
        }

        const ImGuiIO& IO  = ImGui::GetIO();
        const FGuid    GUID = VisibleRows[VisibleIndex]->GUID;

        if (IO.KeyShift && RangeAnchor != INDEX_NONE && (uint32)RangeAnchor < (uint32)VisibleRows.size())
        {
            // Range replaces the selection rather than adding to it, matching every file browser; hold
            // Ctrl+Shift to extend instead.
            if (!IO.KeyCtrl)
            {
                SelectedGUIDs.clear();
            }

            const uint32 Low  = (uint32)RangeAnchor < VisibleIndex ? (uint32)RangeAnchor : VisibleIndex;
            const uint32 High = (uint32)RangeAnchor < VisibleIndex ? VisibleIndex : (uint32)RangeAnchor;
            for (uint32 i = Low; i <= High; ++i)
            {
                SelectedGUIDs.insert(VisibleRows[i]->GUID);
            }
        }
        else if (IO.KeyCtrl)
        {
            if (SelectedGUIDs.find(GUID) != SelectedGUIDs.end())
            {
                SelectedGUIDs.erase(GUID);
            }
            else
            {
                SelectedGUIDs.insert(GUID);
            }
            RangeAnchor = (int32)VisibleIndex;
        }
        else
        {
            SelectedGUIDs.clear();
            SelectedGUIDs.insert(GUID);
            RangeAnchor = (int32)VisibleIndex;
        }

        SelectedGUID = GUID;
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

        // Before anything draws: the filter bar's Resave button needs the visible count, and Ctrl+A needs
        // something to select against.
        BuildVisibleRows(Rows);
        HandleSelectionShortcuts();

        DrawStatsBar(Rows);
        ImGui::Spacing();
        DrawFilterBar();
        ImGui::Spacing();

        ImGui::BeginChild("##Body", ImVec2(0, 0), false);
        {
            const float DetailsWidth = 340.0f;
            ImGui::BeginChild("##TablePane", ImVec2(ImGui::GetContentRegionAvail().x - DetailsWidth, 0), false);
            {
                DrawAssetTable();
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

    void FAssetRegistryEditorTool::DrawTypeFilterMenu()
    {
        // Distinct classes, from the registry rather than from TypeVisibility, so a type nobody has
        // touched still appears in the menu.
        TVector<FName> Types;
        for (const TUniquePtr<FAssetData>& Data : FAssetRegistry::Get().GetAssets())
        {
            if (eastl::find(Types.begin(), Types.end(), Data->AssetClass) == Types.end())
            {
                Types.push_back(Data->AssetClass);
            }
        }
        eastl::sort(Types.begin(), Types.end(), [](const FName& A, const FName& B)
        {
            return A.ToString() < B.ToString();
        });

        if (ImGui::MenuItem("Show All"))
        {
            TypeVisibility.clear();
        }
        if (ImGui::MenuItem("Hide All"))
        {
            for (const FName& Type : Types)
            {
                TypeVisibility.insert_or_assign(Type, false);
            }
        }

        ImGui::Separator();

        for (const FName& Type : Types)
        {
            auto It = TypeVisibility.find(Type);
            bool bVisible = (It == TypeVisibility.end()) || It->second;

            if (ImGui::Checkbox(Type.c_str(), &bVisible))
            {
                if (bVisible)
                {
                    // Erase rather than store true, so "absent == visible" stays the only rule.
                    TypeVisibility.erase(Type);
                }
                else
                {
                    TypeVisibility.insert_or_assign(Type, false);
                }
            }
        }
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
        const uint32 Hidden = CountHiddenTypes();
        FFixedString TypeLabel;
        if (Hidden > 0)
        {
            TypeLabel.sprintf(LE_ICON_FILTER " Types (%u hidden)", Hidden);
        }
        else
        {
            TypeLabel = LE_ICON_FILTER " Types";
        }

        // A Button + popup rather than BeginMenu: this is a plain toolbar row, not a menu bar, and
        // BeginMenu outside one renders as a bare menu item with no button chrome.
        if (Hidden > 0)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::AccentAlt());
        }
        if (ImGui::Button(TypeLabel.c_str()))
        {
            ImGui::OpenPopup("##TypeFilter");
        }
        if (Hidden > 0)
        {
            ImGui::PopStyleColor();
        }

        if (ImGui::BeginPopup("##TypeFilter"))
        {
            DrawTypeFilterMenu();
            ImGui::EndPopup();
        }

        ImGui::SameLine();
        ImGui::Checkbox("Group by Category", &bGroupByCategory);
        ImGui::SameLine();
        ImGui::Checkbox("Loaded Only", &bShowLoadedOnly);

        // Resave acts on the selection, falling back to everything currently visible. That fallback is
        // what makes "filter to Texture, resave" a one-click migration without selecting anything.
        const uint32 SelectedCount = (uint32)SelectedGUIDs.size();
        const uint32 TargetCount   = SelectedCount > 0 ? SelectedCount : (uint32)VisibleRows.size();

        ImGui::SameLine();
        ImGui::BeginDisabled(TargetCount == 0);

        FFixedString ResaveLabel;
        ResaveLabel.sprintf(LE_ICON_CONTENT_SAVE_ALL " Resave (%u)", TargetCount);
        if (ImGui::Button(ResaveLabel.c_str()))
        {
            OpenResaveModal();
        }
        ImGui::EndDisabled();

        ImGuiX::TextTooltip("{}", SelectedCount > 0
            ? "Rewrite every selected asset's package, dirty or not. Loads anything not in memory."
            : "Nothing selected -- this rewrites every asset matching the current filters. "
              "Ctrl+A selects the visible list.");

        if (SelectedCount > 0)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%u selected", SelectedCount);
        }
    }

    void FAssetRegistryEditorTool::OpenResaveModal()
    {
        ResaveQueue.clear();
        ResavedPackages.clear();
        ResaveIndex   = 0;
        ResaveSaved   = 0;
        ResaveFailed  = 0;
        ResaveCurrent = FName();
        ResavePhase   = EResavePhase::Confirm;

        // Snapshot the GUIDs now: VisibleRows points into a per-frame array and the selection can change
        // while the modal is up.
        if (!SelectedGUIDs.empty())
        {
            ResaveQueue.reserve(SelectedGUIDs.size());
            for (const FGuid& GUID : SelectedGUIDs)
            {
                ResaveQueue.push_back(GUID);
            }
        }
        else
        {
            ResaveQueue.reserve(VisibleRows.size());
            for (const FAssetRow* Row : VisibleRows)
            {
                ResaveQueue.push_back(Row->GUID);
            }
        }

        if (ToolContext == nullptr || ResaveQueue.empty())
        {
            return;
        }

        ToolContext->PushModal("Resave Assets", ImVec2(520.0f, 260.0f), [this]() -> bool
        {
            switch (ResavePhase)
            {
            case EResavePhase::Confirm:
            {
                ImGui::TextWrapped("Rewrite %u asset(s)?", (uint32)ResaveQueue.size());
                ImGui::Spacing();
                ImGui::TextWrapped(
                    "Every package is saved whether or not it is dirty, and anything not currently in "
                    "memory is loaded first. This is what upgrades assets to a new serialization format "
                    "(for example splitting texture mips for streaming).");
                ImGui::Spacing();
                ImGui::TextDisabled("A project-wide resave loads the entire project and can take a while.");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
                {
                    return true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Resave", ImVec2(120.0f, 0.0f)))
                {
                    ResavePhase = EResavePhase::Running;
                }
                return false;
            }

            case EResavePhase::Running:
            {
                TickResave();

                const float Progress = ResaveQueue.empty()
                    ? 1.0f
                    : (float)((double)ResaveIndex / (double)ResaveQueue.size());

                ImGui::Text("Resaving... %u / %u", ResaveIndex, (uint32)ResaveQueue.size());
                ImGui::Spacing();
                ImGui::ProgressBar(Progress, ImVec2(-1.0f, 0.0f));
                ImGui::Spacing();
                ImGui::TextDisabled("%s", ResaveCurrent.IsNone() ? "" : ResaveCurrent.c_str());

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Stops issuing more saves; the ones already committed stay committed.
                if (ImGui::Button("Stop", ImVec2(120.0f, 0.0f)))
                {
                    ResavePhase = EResavePhase::Done;
                }

                if (ResaveIndex >= ResaveQueue.size())
                {
                    ResavePhase = EResavePhase::Done;
                }
                return false;
            }

            case EResavePhase::Done:
            default:
            {
                ImGui::Text("Resaved %u package(s).", ResaveSaved);
                if (ResaveFailed > 0)
                {
                    ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.35f, 1.0f),
                        "%u asset(s) failed -- see the log.", ResaveFailed);
                }
                if (ResaveIndex < ResaveQueue.size())
                {
                    ImGui::TextDisabled("Stopped with %u remaining.", (uint32)ResaveQueue.size() - ResaveIndex);
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (ImGui::Button("Close", ImVec2(120.0f, 0.0f)))
                {
                    if (ResaveFailed > 0)
                    {
                        ImGuiX::Notifications::NotifyError("Resave: {0} package(s) saved, {1} asset(s) failed.",
                            ResaveSaved, ResaveFailed);
                    }
                    else
                    {
                        ImGuiX::Notifications::NotifySuccess("Resave: {0} package(s) saved.", ResaveSaved);
                    }

                    // Disk sizes just changed under the cache.
                    DiskSizeCache.clear();
                    return true;
                }
                return false;
            }
            }
        });
    }

    void FAssetRegistryEditorTool::TickResave()
    {
        // Bounded per frame so the editor keeps drawing and the progress bar actually moves. Small,
        // because a single package save can be tens of milliseconds once loading is counted.
        constexpr uint32 kPackagesPerFrame = 4;

        uint32 Processed = 0;
        while (Processed < kPackagesPerFrame && ResaveIndex < ResaveQueue.size())
        {
            const FGuid GUID = ResaveQueue[ResaveIndex];
            ++ResaveIndex;

            // Counted per queue entry, not per package actually written: the LoadObject below is the
            // expensive half, and it runs even for an entry whose package a sibling export already saved.
            ++Processed;

            // Loads the asset if it is not resident: an unloaded package cannot be rewritten, and the
            // whole point of this is to rewrite assets that have NOT changed.
            CObject* Asset = LoadObject<CObject>(GUID);
            if (Asset == nullptr)
            {
                LOG_ERROR("Resave: could not load asset {}", GUID.ToString());
                ++ResaveFailed;
                continue;
            }

            ResaveCurrent = Asset->GetName();

            CPackage* Package = Asset->GetPackage();
            if (Package == nullptr || Package->IsTransientPackage())
            {
                ++ResaveFailed;
                continue;
            }

            // One save per package however many exports it holds -- saving it again per export would
            // rewrite the same file N times and count N successes for one file.
            if (ResavedPackages.find(Package) != ResavedPackages.end())
            {
                continue;
            }
            ResavedPackages.insert(Package);

            FCoreEditorDelegates::OnAssetPreSave.Broadcast(Asset);

            if (CPackage::SavePackage(Package, Package->GetPackagePath()))
            {
                FAssetRegistry::Get().AssetSaved(Asset);
                FCoreEditorDelegates::OnAssetSaved.Broadcast(Asset);
                ++ResaveSaved;
            }
            else
            {
                LOG_ERROR("Resave: failed to save package {}", Package->GetName());
                ++ResaveFailed;
            }
        }
    }

    void FAssetRegistryEditorTool::DrawAssetTableRows(const TVector<const FAssetRow*>& Rows, uint32 BaseIndex)
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
                const bool bSelected = (SelectedGUIDs.find(Row.GUID) != SelectedGUIDs.end());
                if (ImGui::Selectable(Row.Name.c_str(), bSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
                {
                    ApplyRowClick(BaseIndex + (uint32)i);

                    if (ImGui::IsMouseDoubleClicked(0) && ToolContext)
                    {
                        ToolContext->OpenAssetEditor(Row.GUID);
                    }
                }

                if (ImGui::BeginPopupContextItem("##RowCtx"))
                {
                    // Right-clicking outside the selection retargets it, so the menu never acts on
                    // something the user cannot see is selected.
                    if (SelectedGUIDs.find(Row.GUID) == SelectedGUIDs.end())
                    {
                        SelectedGUIDs.clear();
                        SelectedGUIDs.insert(Row.GUID);
                        SelectedGUID = Row.GUID;
                        RangeAnchor  = (int32)(BaseIndex + (uint32)i);
                    }

                    if (ImGui::MenuItem(LE_ICON_FILE " Open Asset"))
                    {
                        if (ToolContext)
                        {
                            ToolContext->OpenAssetEditor(Row.GUID);
                        }
                    }

                    {
                        FFixedString ResaveLabel;
                        ResaveLabel.sprintf(LE_ICON_CONTENT_SAVE_ALL " Resave %u Selected", (uint32)SelectedGUIDs.size());
                        if (ImGui::MenuItem(ResaveLabel.c_str()))
                        {
                            OpenResaveModal();
                        }
                    }

                    ImGui::Separator();
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

    void FAssetRegistryEditorTool::DrawAssetTable()
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

        // Both layouts draw out of VisibleRows, which BuildVisibleRows already filtered and ordered --
        // so a row's index there is its index on screen, which is what shift-range and Ctrl+A rely on.
        if (bGroupByCategory)
        {
            for (const FRowGroup& Group : VisibleGroups)
            {
                TVector<const FAssetRow*> Bucket(
                    VisibleRows.begin() + Group.Start,
                    VisibleRows.begin() + Group.Start + Group.Count);

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

                ImGui::PushStyleColor(ImGuiCol_Text, CategoryColor(FName(Group.Category)));
                FString Header = Group.Category + "  (" + eastl::to_string(LoadedInCat) + "/" +
                    eastl::to_string(Bucket.size()) + " loaded, " + FString(ImGuiX::FormatSize(CpuInCat).c_str()) + ")";
                const bool bOpen = ImGui::CollapsingHeader(Header.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
                ImGui::PopStyleColor();

                if (bOpen)
                {
                    FString TableID = "##Table_" + Group.Category;
                    if (ImGui::BeginTable(TableID.c_str(), 6, TableFlags))
                    {
                        SetupColumns(false);
                        DrawAssetTableRows(Bucket, Group.Start);
                        ImGui::EndTable();
                    }
                    ImGui::Spacing();
                }
            }
        }
        else
        {
            if (ImGui::BeginTable("##AssetTable", 6, TableFlags | ImGuiTableFlags_ScrollY, ImVec2(0, 0)))
            {
                SetupColumns(true);
                DrawAssetTableRows(VisibleRows, 0);
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
