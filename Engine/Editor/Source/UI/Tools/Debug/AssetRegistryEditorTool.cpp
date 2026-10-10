#include "AssetRegistryEditorTool.h"
#include "Core/CoreEditorDelegates.h"


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
#include "Platform/Time/PlatformTime.h"
#include "TaskSystem/ParallelSort.h"
#include "UI/Tools/EditorToolContext.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    namespace
    {
        // Runs Serialize to harvest referenced CObjects without writing, and takes no same-package limit.
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
            const uint32 Hash = (uint32)Class.GetStableHash();
            const float Hue = (Hash % 360) / 360.0f;
            ImVec4 Color;
            ImGui::ColorConvertHSVtoRGB(Hue, 0.5f, 0.95f, Color.x, Color.y, Color.z);
            Color.w = 1.0f;
            return Color;
        }

        bool NameLess(const FName& A, const FName& B)
        {
            return strcmp(A.c_str(), B.c_str()) < 0;
        }

        constexpr double kRowsRebuildIntervalSeconds = 1.0;
        constexpr uint32 kLiveRowsPerFrame           = 4096;
        constexpr uint32 kFilterRowsPerTask          = 8192;
    }

    void FAssetRegistryEditorTool::OnInitialize()
    {
        CreateToolWindow("Asset Registry", [this](bool bIsFocused)
        {
            DrawWindow(bIsFocused);
        });

        RegistryUpdatedHandle = FAssetRegistry::Get().GetOnAssetRegistryUpdated().AddLambda([this]()
        {
            bRowsStale.store(true, std::memory_order_release);
        });
    }

    void FAssetRegistryEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
        FAssetRegistry::Get().GetOnAssetRegistryUpdated().Remove(RegistryUpdatedHandle);
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
            "The object's strong ref-count: how many TStrongObjectPtrs keep it alive (components, materials, the "
            "open editor, etc). Zero on a loaded asset means it is a candidate for unloading.");
        DrawHelpTextRow("Referenced By",
            "The saved packages that import the selected asset, read from the registry, so it works for "
            "unloaded assets too. For a loaded asset, Scan Live Objects instead searches every object in memory "
            "for reflected references, which also finds unsaved ones but takes a noticeable moment.");
        DrawHelpTextRow("Refresh",
            "Re-reads the registry and recomputes the referencer list. The list also follows registry changes "
            "on its own, and loaded state and ref-counts are rechecked continuously a slice at a time.");
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
            Bytes += MR.MeshletData.VertexPositions.capacity()       * sizeof(FMeshVertexPosition);
            Bytes += MR.MeshletData.VertexAttributes.capacity()      * sizeof(FMeshVertexAttributes);
            Bytes += MR.MeshletData.VertexUV1s.capacity()            * sizeof(uint32);
            Bytes += MR.MeshletData.VertexColors.capacity()          * sizeof(uint32);
            Bytes += MR.MeshletData.MeshletVertexRefs.capacity()     * sizeof(uint32);
            Bytes += MR.MeshletData.MeshletSkinnedVertices.capacity()* sizeof(FMeshletSkinnedVertex);
            Bytes += MR.MeshletData.MeshletTriangles.capacity()      * sizeof(uint32);
            Bytes += MR.MeshletData.MeshletSpheres.capacity()        * sizeof(FMeshletSphere);
            Bytes += MR.MeshletData.MeshletCones.capacity()          * sizeof(FMeshletCone);

            return Bytes;
        }

        if (const CMaterial* Material = Cast<CMaterial>(Asset))
        {
            uint64 Bytes = 0;
            for (const FMaterialStageBlob& Blob : Material->Stages)
            {
                Bytes += Blob.Spirv.capacity() * sizeof(uint32);
            }
            Bytes += Material->Parameters.capacity() * sizeof(FMaterialParameter);
            return Bytes;
        }

        return 0;
    }

    void FAssetRegistryEditorTool::RebuildRegistryReferencers(const FGuid& Target)
    {
        Referencers.clear();
        CachedReferencerTarget   = Target;
        bReferencersFromLiveScan = false;

        for (const FAssetData* Data : FAssetRegistry::Get().GetReferencersOf(Target))
        {
            FReferencer Ref;
            Ref.Name    = Data->AssetName;
            Ref.Class   = Data->AssetClass;
            Ref.Package = FName(Data->Path.c_str());
            Referencers.push_back(Ref);
        }

        Algo::Sort(Referencers, [](const FReferencer& A, const FReferencer& B)
        {
            return NameLess(A.Name, B.Name);
        });
    }

    void FAssetRegistryEditorTool::RebuildLiveReferencers(CObject* Target)
    {
        LUMINA_PROFILE_SCOPE();

        Referencers.clear();
        CachedReferencerTarget   = Target ? Target->GetGUID() : FGuid();
        bReferencersFromLiveScan = true;

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

        Algo::Sort(Referencers, [](const FReferencer& A, const FReferencer& B)
        {
            return NameLess(A.Name, B.Name);
        });
    }

    bool FAssetRegistryEditorTool::PassesFilter(const FAssetRow& Row) const
    {
        if (bShowLoadedOnly && !Row.bLoaded)
        {
            return false;
        }

        // Only explicitly unticked classes are stored, so a new asset type is never silently hidden.
        auto TypeIt = TypeVisibility.find(Row.Class);
        if (TypeIt != TypeVisibility.end() && !TypeIt->second)
        {
            return false;
        }

        if (!SearchFilter.empty() && !ImGuiX::PassSearchFilter(FStringView(SearchFilter.c_str(), SearchFilter.size()), FStringView(Row.Name.c_str())))
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

    void FAssetRegistryEditorTool::RebuildRows()
    {
        LUMINA_PROFILE_SCOPE();

        Rows.clear();
        RowIndexByGUID.clear();
        AssetTypes.clear();
        LoadedCount     = 0;
        TotalCpuBytes   = 0;
        TotalDiskBytes  = 0;
        LiveSweepCursor = 0;

        THashSet<FName> Types;
        {
            const FAssetDataMap& Assets = FAssetRegistry::Get().GetAssets();
            Rows.reserve(Assets.size());
            RowIndexByGUID.reserve(Assets.size());

            for (const TUniquePtr<FAssetData>& Data : Assets)
            {
                FAssetRow Row;
                Row.GUID      = Data->AssetGUID;
                Row.Name      = Data->AssetName;
                Row.Class     = Data->AssetClass;
                Row.DiskBytes = Data->FileSize;

                TotalDiskBytes += Row.DiskBytes;
                Types.insert(Row.Class);
                RowIndexByGUID.emplace(Row.GUID, (uint32)Rows.size());
                Rows.push_back(Row);
            }
        }

        AssetTypes.assign(Types.begin(), Types.end());
        Algo::Sort(AssetTypes, NameLess);

        // Resolved once, so the sort compares raw strings instead of going through the name table each time.
        TVector<const char*> RowNames(Rows.size());
        RowsByName.resize(Rows.size());
        for (uint32 Index = 0; Index < (uint32)Rows.size(); ++Index)
        {
            RowNames[Index]   = Rows[Index].Name.c_str();
            RowsByName[Index] = Index;
        }
        Task::ParallelSort(RowsByName.begin(), RowsByName.end(), [&RowNames](uint32 A, uint32 B)
        {
            return strcmp(RowNames[A], RowNames[B]) < 0;
        });

        // A counting sort by class over the name order, which keeps each class already sorted by name.
        THashMap<FName, uint32> ClassSlots;
        ClassSlots.reserve(AssetTypes.size());
        for (uint32 Slot = 0; Slot < (uint32)AssetTypes.size(); ++Slot)
        {
            ClassSlots.emplace(AssetTypes[Slot], Slot);
        }

        TVector<uint32> RowSlots(Rows.size());
        TVector<uint32> SlotStarts(AssetTypes.size() + 1, 0);
        for (uint32 Index = 0; Index < (uint32)Rows.size(); ++Index)
        {
            RowSlots[Index] = ClassSlots[Rows[Index].Class];
            ++SlotStarts[RowSlots[Index] + 1];
        }
        for (size_t Slot = 1; Slot < SlotStarts.size(); ++Slot)
        {
            SlotStarts[Slot] += SlotStarts[Slot - 1];
        }

        RowsByCategory.resize(Rows.size());
        for (uint32 Index : RowsByName)
        {
            RowsByCategory[SlotStarts[RowSlots[Index]]++] = Index;
        }

        bVisibleStale = true;
    }

    void FAssetRegistryEditorTool::StepLiveState()
    {
        LUMINA_PROFILE_SCOPE();

        const uint32 NumRows = (uint32)Rows.size();
        const uint32 End = LiveSweepCursor + kLiveRowsPerFrame < NumRows ? LiveSweepCursor + kLiveRowsPerFrame : NumRows;

        for (uint32 Index = LiveSweepCursor; Index < End; ++Index)
        {
            FAssetRow& Row = Rows[Index];
            const bool   bWasLoaded = Row.bLoaded;
            const uint64 OldCpu     = Row.CpuBytes;

            CObject* Object = FindObject<CObject>(Row.GUID);
            Row.bLoaded  = Object != nullptr;
            Row.RefCount = Object != nullptr ? Object->GetStrongRefCount() : 0;
            Row.CpuBytes = EstimateCpuBytes(Object);

            LoadedCount   = LoadedCount + (Row.bLoaded ? 1u : 0u) - (bWasLoaded ? 1u : 0u);
            TotalCpuBytes = TotalCpuBytes + Row.CpuBytes - OldCpu;
            bSweepChangedLoaded |= bWasLoaded != Row.bLoaded;
        }

        LiveSweepCursor = End;
        if (LiveSweepCursor < NumRows)
        {
            return;
        }
        LiveSweepCursor = 0;

        if (bShowLoadedOnly && bSweepChangedLoaded)
        {
            bVisibleStale = true;
        }
        bSweepChangedLoaded = false;

        // Membership is unchanged, so only the per-group totals in the headers need recounting.
        for (FRowGroup& Group : VisibleGroups)
        {
            Group.Loaded   = 0;
            Group.CpuBytes = 0;
            for (uint32 Index = Group.Start; Index < Group.Start + Group.Count; ++Index)
            {
                if (VisibleRows[Index]->bLoaded)
                {
                    ++Group.Loaded;
                    Group.CpuBytes += VisibleRows[Index]->CpuBytes;
                }
            }
        }
    }

    void FAssetRegistryEditorTool::BuildVisibleRows()
    {
        LUMINA_PROFILE_SCOPE();

        VisibleRows.clear();
        VisibleGroups.clear();
        bVisibleStale = false;

        // Both orders are presorted, so a filter change is one parallel test and one pass with no sorting.
        const TVector<uint32>& Order = bGroupByCategory ? RowsByCategory : RowsByName;
        TVector<uint8> Passes(Order.size(), 0);
        Task::ParallelFor((uint32)Order.size(), [&](uint32 Index)
        {
            Passes[Index] = PassesFilter(Rows[Order[Index]]) ? 1 : 0;
        }, kFilterRowsPerTask);

        VisibleRows.reserve(Order.size());
        for (size_t OrderIndex = 0; OrderIndex < Order.size(); ++OrderIndex)
        {
            if (Passes[OrderIndex] == 0)
            {
                continue;
            }
            const FAssetRow& Row = Rows[Order[OrderIndex]];

            if (bGroupByCategory)
            {
                if (VisibleGroups.empty() || VisibleGroups.back().Category != Row.Class)
                {
                    FRowGroup Group;
                    Group.Category = Row.Class;
                    Group.Start    = (uint32)VisibleRows.size();
                    VisibleGroups.push_back(Group);
                }

                FRowGroup& Group = VisibleGroups.back();
                ++Group.Count;
                if (Row.bLoaded)
                {
                    ++Group.Loaded;
                    Group.CpuBytes += Row.CpuBytes;
                }
            }

            VisibleRows.push_back(&Row);
        }
    }

    void FAssetRegistryEditorTool::HandleSelectionShortcuts()
    {
        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        {
            return;
        }

        // Ctrl+A in the search box means select the text, so stealing it would be a nasty surprise.
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
            RangeAnchor = Constants::kIndexNone;
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

        if (IO.KeyShift && RangeAnchor != Constants::kIndexNone && (uint32)RangeAnchor < (uint32)VisibleRows.size())
        {
            // Range replaces the selection like every file browser, and Ctrl+Shift extends instead.
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
        const double Now = PlatformTime::Seconds();

        // Throttled, since an import can update the registry many times a second.
        if (Now >= NextRowsRebuildSeconds && bRowsStale.exchange(false, std::memory_order_acquire))
        {
            NextRowsRebuildSeconds = Now + kRowsRebuildIntervalSeconds;
            RebuildRows();
        }
        StepLiveState();

        // The Resave button needs the visible count and Ctrl+A needs something to select against.
        if (bVisibleStale)
        {
            BuildVisibleRows();
        }
        HandleSelectionShortcuts();

        DrawStatsBar();
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
                DrawDetailsPanel();
            }
            ImGui::EndChild();
        }
        ImGui::EndChild();
    }

    void FAssetRegistryEditorTool::DrawStatsBar()
    {
        const uint32 Total     = (uint32)Rows.size();
        const uint32 Loaded    = LoadedCount;
        const uint64 TotalCpu  = TotalCpuBytes;
        const uint64 TotalDisk = TotalDiskBytes;

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
            Stat("TOTAL ASSETS",  ImVec4(0.7f, 0.8f, 1.0f, 1.0f), Format("{}", Total));
            ImGui::NextColumn();
            Stat("LOADED",        ImVec4(0.45f, 0.85f, 0.5f, 1.0f), Format("{}", Loaded));
            ImGui::NextColumn();
            Stat("UNLOADED",      ImVec4(0.65f, 0.65f, 0.68f, 1.0f), Format("{}", Total - Loaded));
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
        // Taken from the registry rather than TypeVisibility, so an untouched type still appears.
        const TVector<FName>& Types = AssetTypes;

        if (ImGui::MenuItem("Show All"))
        {
            TypeVisibility.clear();
            bVisibleStale = true;
        }
        if (ImGui::MenuItem("Hide All"))
        {
            for (const FName& Type : Types)
            {
                TypeVisibility.insert_or_assign(Type, false);
            }
            bVisibleStale = true;
        }

        ImGui::Separator();

        for (const FName& Type : Types)
        {
            auto It = TypeVisibility.find(Type);
            bool bVisible = (It == TypeVisibility.end()) || It->second;

            if (ImGui::Checkbox(Type.c_str(), &bVisible))
            {
                bVisibleStale = true;
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
            bRowsStale.store(true, std::memory_order_release);
            NextRowsRebuildSeconds = 0.0;
            CachedReferencerTarget = FGuid();
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::InputTextWithHint("##Search", LE_ICON_MAGNIFY " Search assets...", SearchBuffer, IM_ARRAYSIZE(SearchBuffer)))
        {
            SearchFilter  = SearchBuffer;
            bVisibleStale = true;
        }

        ImGui::SameLine();
        const uint32 Hidden = CountHiddenTypes();
        FFixedString TypeLabel;
        if (Hidden > 0)
        {
            FormatTo(TypeLabel, LE_ICON_FILTER " Types ({} hidden)", Hidden);
        }
        else
        {
            TypeLabel = LE_ICON_FILTER " Types";
        }

        // A plain toolbar row, and BeginMenu outside a menu bar renders with no button chrome.
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
        bVisibleStale |= ImGui::Checkbox("Group by Category", &bGroupByCategory);
        ImGui::SameLine();
        bVisibleStale |= ImGui::Checkbox("Loaded Only", &bShowLoadedOnly);

        // The fallback to everything visible is what makes filter-then-resave a one-click migration.
        const uint32 SelectedCount = (uint32)SelectedGUIDs.size();
        const uint32 TargetCount   = SelectedCount > 0 ? SelectedCount : (uint32)VisibleRows.size();

        ImGui::SameLine();
        ImGui::BeginDisabled(TargetCount == 0);

        FFixedString ResaveLabel;
        FormatTo(ResaveLabel, LE_ICON_CONTENT_SAVE_ALL " Resave ({})", TargetCount);
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

        // VisibleRows points into a per-frame array and the selection can change while the modal is up.
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

                    // Disk sizes just changed under the snapshot.
                    bRowsStale.store(true, std::memory_order_release);
                    return true;
                }
                return false;
            }
            }
        });
    }

    void FAssetRegistryEditorTool::TickResave()
    {
        // Small, since one package save can be tens of milliseconds once loading is counted.
        constexpr uint32 kPackagesPerFrame = 4;

        uint32 Processed = 0;
        while (Processed < kPackagesPerFrame && ResaveIndex < ResaveQueue.size())
        {
            const FGuid GUID = ResaveQueue[ResaveIndex];
            ++ResaveIndex;

            // Counted per queue entry, since the LoadObject is the expensive half and runs regardless.
            ++Processed;

            // An unloaded package cannot be rewritten, and the point is to rewrite unchanged assets.
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

            // One save per package, or the same file is rewritten once per export it holds.
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

    void FAssetRegistryEditorTool::DrawAssetTableRows(const FAssetRow* const* GroupRows, uint32 Count, uint32 BaseIndex)
    {
        ImGuiListClipper Clipper;
        Clipper.Begin((int)Count);
        while (Clipper.Step())
        {
            for (int i = Clipper.DisplayStart; i < Clipper.DisplayEnd; ++i)
            {
                const FAssetRow& Row = *GroupRows[i];
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
                    // Right-clicking outside the selection retargets it, so the menu acts on what is visible.
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
                        FormatTo(ResaveLabel, LE_ICON_CONTENT_SAVE_ALL " Resave {} Selected", (uint32)SelectedGUIDs.size());
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
                        if (const FAssetData* Data = FAssetRegistry::Get().GetAssetByGUID(Row.GUID))
                        {
                            ImGui::SetClipboardText(Data->Path.c_str());
                        }
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
                ImGui::PushStyleColor(ImGuiCol_Text, StatusColor(Row.bLoaded));
                ImGui::TextUnformatted(StatusLabel(Row.bLoaded));
                ImGui::PopStyleColor();

                ImGui::TableSetColumnIndex(3);
                if (Row.bLoaded)
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
                if (Row.bLoaded)
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
        // ScrollY only on the flat table, since grouped tables auto-size and let the outer pane scroll.
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

        // A row's index in VisibleRows is its index on screen, which shift-range and Ctrl+A rely on.
        if (bGroupByCategory)
        {
            for (const FRowGroup& Group : VisibleGroups)
            {
                // The triple hash keeps the header's open state stable while its counts change.
                FFixedString Header;
                FormatTo(Header, "{}  ({}/{} loaded, {})###{}", Group.Category.c_str(), Group.Loaded, Group.Count,
                    ImGuiX::FormatSize(Group.CpuBytes).c_str(), Group.Category.c_str());

                ImGui::PushStyleColor(ImGuiCol_Text, CategoryColor(Group.Category));
                const bool bOpen = ImGui::CollapsingHeader(Header.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
                ImGui::PopStyleColor();

                if (bOpen)
                {
                    FFixedString TableID;
                    FormatTo(TableID, "##Table_{}", Group.Category.c_str());
                    if (ImGui::BeginTable(TableID.c_str(), 6, TableFlags))
                    {
                        SetupColumns(false);
                        DrawAssetTableRows(VisibleRows.data() + Group.Start, Group.Count, Group.Start);
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
                DrawAssetTableRows(VisibleRows.data(), (uint32)VisibleRows.size(), 0);
                ImGui::EndTable();
            }
        }
    }

    void FAssetRegistryEditorTool::DrawDetailsPanel()
    {
        auto Found = RowIndexByGUID.find(SelectedGUID);
        const FAssetRow* Selected = Found != RowIndexByGUID.end() ? &Rows[Found->second] : nullptr;

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
        const FAssetData* SelectedData = FAssetRegistry::Get().GetAssetByGUID(Selected->GUID);
        Field("Path",  SelectedData != nullptr ? FString(SelectedData->Path.c_str()) : FString());
        Field("GUID",  Selected->GUID.ToString(), ImVec4(0.55f, 0.55f, 0.55f, 1.0f));

        // Looked up live, since the referencer scan dereferences it and the row may not have been rechecked yet.
        CObject* LoadedObject = FindObject<CObject>(Selected->GUID);
        const bool bLoaded = LoadedObject != nullptr;
        Field("Status", StatusLabel(bLoaded), StatusColor(bLoaded));
        Field("On Disk", ImGuiX::FormatSize(Selected->DiskBytes).c_str());

        if (bLoaded)
        {
            Field("CPU Memory", ImGuiX::FormatSize(Selected->CpuBytes).c_str(), ImVec4(1.0f, 0.75f, 0.4f, 1.0f));
            Field("Ref Count", Format("{}", Selected->RefCount),
                Selected->RefCount > 0 ? ImVec4(0.85f, 0.85f, 0.85f, 1.0f) : ImVec4(0.9f, 0.6f, 0.3f, 1.0f));
        }
        else
        {
            ImGui::TextDisabled("Not resident in memory, so no ref-count");
            ImGui::TextDisabled("or footprint until something loads it.");
            ImGui::Spacing();
        }

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.9f, 1.0f, 1.0f));
        ImGui::TextUnformatted(LE_ICON_LINK " Referenced By");
        ImGui::PopStyleColor();
        ImGui::Separator();

        if (CachedReferencerTarget != Selected->GUID)
        {
            RebuildRegistryReferencers(Selected->GUID);
        }

        if (bLoaded && ImGui::SmallButton(LE_ICON_MAGNIFY " Scan Live Objects"))
        {
            RebuildLiveReferencers(LoadedObject);
        }
        ImGui::TextDisabled("%s", bReferencersFromLiveScan ? "From a scan of objects in memory." : "From saved packages.");

        if (Referencers.empty())
        {
            ImGui::TextDisabled("No references found.");
            if (bReferencersFromLiveScan)
            {
                ImGui::TextDisabled("(ECS components still count toward ref-count.)");
            }
        }
        else if (ImGui::BeginTable("##Referencers", 2,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
            ImVec2(0, 0)))
        {
            ImGui::TableSetupColumn("Object");
            ImGui::TableSetupColumn("Class");
            ImGui::TableHeadersRow();

            ImGuiListClipper Clipper;
            Clipper.Begin((int)Referencers.size());
            while (Clipper.Step())
            {
                for (int Index = Clipper.DisplayStart; Index < Clipper.DisplayEnd; ++Index)
                {
                    const FReferencer& Ref = Referencers[Index];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(Ref.Name.c_str());
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("Package %s", Ref.Package.c_str());
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::PushStyleColor(ImGuiCol_Text, CategoryColor(Ref.Class));
                    ImGui::TextUnformatted(Ref.Class.c_str());
                    ImGui::PopStyleColor();
                }
            }
            ImGui::EndTable();
        }
    }
}
