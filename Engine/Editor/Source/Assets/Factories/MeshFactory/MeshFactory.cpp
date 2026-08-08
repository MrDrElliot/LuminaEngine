#include "EditorPCH.h"
#include "MeshFactory.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Containers/Array.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Threading/Thread.h"
#include "TaskSystem/Future.h"
#include "Tools/Import/MaterialImport.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "assets/assettypes/mesh/skeleton/skeleton.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Assets/Factories/TextureFactory/TextureFactory.h"
#include "Core/Object/Package/Package.h"
#include "Core/Progress/SlowTask.h"
#include "Core/Utils/Defer.h"
#include "Renderer/RendererUtils.h"
#include "FileSystem/FileSystem.h"
#include "Paths/Paths.h"
#include "TaskSystem/TaskSystem.h"
#include "TaskSystem/ThreadedCallback.h"
#include "Tools/Import/ImportHelpers.h"
#include "Tools/Import/MeshFormatImport.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Log/Log.h"
#include "Renderer/SkeletonResource.h"


namespace Lumina
{
    namespace
    {
        using namespace Import::Mesh;

        // Neutral-options preview parse; transforms and heavy passes deferred to commit time.
        bool PreviewParse(const FFixedString& RawPath, FMeshImportData& Out, FScopedSlowTask* Progress)
        {
            FMeshImportOptions PreviewOptions;
            PreviewOptions.bOptimize         = false;
            PreviewOptions.bMergeMeshes      = false;
            PreviewOptions.bFlipNormals      = false;
            PreviewOptions.bFlipUVs          = false;
            PreviewOptions.Scale             = 1.0f;
            PreviewOptions.bSkipFinalization = true;

            const FName Ext = VFS::Extension(RawPath);
            TExpected<FMeshImportData, FString> Result;
            if (Ext == ".obj")
            {
                Result = OBJ::ImportOBJ(PreviewOptions, RawPath, Progress);
            }
            else if (Ext == ".gltf" || Ext == ".glb")
            {
                Result = GLTF::ImportGLTF(PreviewOptions, RawPath, Progress);
            }
            else if (Ext == ".fbx")
            {
                Result = FBX::ImportFBX(PreviewOptions, RawPath, Progress);
            }

            if (!Result)
            {
                LOG_ERROR("Encountered problem importing source file: {0}", Result.Error());
                return false;
            }

            Out = Move(Result.Value());
            return true;
        }
        
        void BuildPreviewThumbnails(const FFixedString& RawPath, FMeshImportData& Data, FScopedSlowTask& Progress)
        {
            if (Data.Textures.empty())
            {
                return;
            }

            Progress.UpdateMessage("Generating thumbnails...");
            
            TVector<FMeshImportImage*> Images;
            Images.reserve(Data.Textures.size());
            for (const FMeshImportImage& Texture : Data.Textures)
            {
                Images.push_back(const_cast<FMeshImportImage*>(&Texture));
            }

            Task::ParallelFor((uint32)Images.size(), [&](uint32 Index)
            {
                FMeshImportImage& Texture = *Images[Index];
                if (Texture.DisplayImage.IsValid())
                {
                    return;
                }
                if (Texture.IsBytes())
                {
                    Texture.DisplayImage = RenderUtils::CreateImageFromPixels(Texture.Bytes, true, FUIntVector2(128, 128));
                }
                else
                {
                    FFixedString FullPath = Paths::Combine(VFS::Parent(RawPath), Texture.RelativePath);
                    Texture.DisplayImage = Import::Textures::CreateTextureFromImport(FullPath, true, FUIntVector2(128, 128));
                }
            });
        }

        constexpr float kLabelColumnWidth = 180.0f;

        bool BeginPropertyTable(const char* Id)
        {
            if (!ImGui::BeginTable(Id, 2, ImGuiTableFlags_PadOuterX | ImGuiTableFlags_SizingFixedFit))
            {
                return false;
            }
            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, kLabelColumnWidth);
            ImGui::TableSetupColumn("##editor", ImGuiTableColumnFlags_WidthStretch);
            return true;
        }

        void PropertyLabel(const char* Label, const char* Tooltip)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(Label);
            if (Tooltip && ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", Tooltip);
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
        }

        void CheckboxRow(const char* Label, const char* Tooltip, bool& Value)
        {
            PropertyLabel(Label, Tooltip);
            ImGui::PushID(Label);
            ImGui::Checkbox("##v", &Value);
            ImGui::PopID();
        }

        void DragFloatRow(const char* Label, const char* Tooltip, float& Value, float Min, float Max, const char* Fmt)
        {
            PropertyLabel(Label, Tooltip);
            ImGui::PushID(Label);
            ImGui::DragFloat("##v", &Value, 0.001f, Min, Max, Fmt);
            ImGui::PopID();
        }

        void DragIntRow(const char* Label, const char* Tooltip, uint32& Value, int32 Min, int32 Max)
        {
            PropertyLabel(Label, Tooltip);
            ImGui::PushID(Label);
            int32 Temp = (int32)Value;
            if (ImGui::DragInt("##v", &Temp, 1.0f, Min, Max))
            {
                Value = (uint32)eastl::clamp(Temp, Min, Max);
            }
            ImGui::PopID();
        }

        void DrawDistanceFieldSection(SDistanceFieldBuildSettings& Settings)
        {
            if (!ImGui::CollapsingHeader("Distance Field"))
            {
                return;
            }

            ImGui::TextWrapped(
                "Bakes a signed distance field volume from the mesh. Materials sample it through the "
                "Distance Field nodes for self-occlusion, thickness, soft masks and ray marching. This is "
                "the most expensive step of an import and can be added later from the mesh editor.");
            ImGui::Spacing();

            if (BeginPropertyTable("DistanceFieldTable"))
            {
                CheckboxRow("Generate Distance Field",
                            "Voxelize the mesh into a signed distance field. Off leaves the mesh without "
                            "one; every Distance Field material node then reports invalid.",
                            Settings.bEnabled);

                if (Settings.bEnabled)
                {
                    DragIntRow("Resolution",
                               "Voxels along the mesh's longest axis. Build time and memory both scale with "
                               "the cube of this: 32 is coarse and near-free, 48 suits most props, 96+ is for "
                               "hero assets whose creases matter.",
                               Settings.Resolution, 4, 256);

                    DragFloatRow("Narrow Band Scale",
                                 "Width of the accurate band as a fraction of the mesh's longest extent. "
                                 "Distances saturate past it, so raise it for effects that need to reach far "
                                 "from the surface and lower it for finer precision close to it.",
                                 Settings.NarrowBandScale, 0.01f, 1.0f, "%.3f");

                    CheckboxRow("Two Sided",
                                "For thin or open geometry (foliage cards, cloth, anything not closed). "
                                "Stores unsigned distance, skipping the inside/outside test that produces "
                                "speckle on non-closed meshes -- and builds much faster.",
                                Settings.bTwoSided);

                    DragIntRow("Source LOD",
                               "Which baked LOD supplies the triangles. A distance field is low-frequency, so "
                               "a coarser level usually voxelizes the same for a fraction of the cost.",
                               Settings.SourceLOD, 0, 3);

                    // The single number that decides whether this import takes a moment or a coffee break,
                    // so it is shown rather than left to be discovered after committing.
                    const uint32 R = eastl::clamp(Settings.Resolution, 4u, 256u);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted("Worst Case Size");
                    ImGui::TableSetColumnIndex(1);
                    ImGuiX::Text("{0} per mesh", ImGuiX::FormatSize((size_t)R * R * R));
                }

                ImGui::EndTable();
            }
            ImGui::Spacing();
        }

        void DrawOptionsSection(FMeshImportOptions& Options)
        {
            if (!ImGui::CollapsingHeader("Import Options", ImGuiTreeNodeFlags_DefaultOpen))
            {
                return;
            }

            if (BeginPropertyTable("OptionsTable"))
            {
                CheckboxRow("Import Meshes",     "Import skeletal and static meshes from the source file.", Options.bImportMeshes);
                CheckboxRow("Import Skeletons",  "Import skeleton hierarchies and bone data.",              Options.bImportSkeleton);
                CheckboxRow("Import Animations", "Import skeletal and morph target animations.",            Options.bImportAnimations);
                CheckboxRow("Import Materials",  "Import material definitions and create material assets.", Options.bImportMaterials);
                if (!Options.bImportMaterials)
                {
                    CheckboxRow("Import Textures", "Import texture files referenced by the source.", Options.bImportTextures);
                }

                DragFloatRow("Scale", "Uniform scale factor applied to all imported geometry.",
                             Options.Scale, 0.001f, 100.0f, "%.3f");
                CheckboxRow("Flip UVs",     "Flip UV coordinates vertically (1 - V).",                   Options.bFlipUVs);
                CheckboxRow("Flip U",       "Flip UV coordinates horizontally (1 - U); for sources with mirrored UVs (backwards text).", Options.bFlipU);
                CheckboxRow("Flip Normals", "Invert mesh normals (useful for inside-out geometry).",     Options.bFlipNormals);

                CheckboxRow("Optimize Mesh",
                            "Optimize vertex cache locality and reduce overdraw for better runtime performance.",
                            Options.bOptimize);
                CheckboxRow("Merge Meshes",
                            "Combine every mesh in the source file into a single asset. Primitives "
                            "that share a source material are folded onto the same material slot.",
                            Options.bMergeMeshes);

                ImGui::EndTable();
            }
            ImGui::Spacing();

            DrawDistanceFieldSection(Options.DistanceField);
        }

        void DrawMeshStats(const FMeshImportData& Data)
        {
            if (Data.Resources.empty())
            {
                return;
            }

            FFixedString Header(FFixedString::CtorSprintf(), "Meshes (%zu)###MeshStats", Data.Resources.size());
            if (!ImGui::CollapsingHeader(Header.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            {
                return;
            }

            constexpr ImGuiTableFlags Flags =
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInner |
                ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;

            const float Height = eastl::min<float>(180.0f, (Data.Resources.size() + 1) * ImGui::GetTextLineHeightWithSpacing() + 8.0f);
            if (ImGui::BeginTable("MeshStatsTable", 6, Flags, ImVec2(0, Height)))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Name",     ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Verts",    ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("Indices",  ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("Surfaces", ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("Overdraw", ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("V-Fetch",  ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableHeadersRow();

                for (size_t i = 0; i < Data.Resources.size(); ++i)
                {
                    const FMeshResource& R = *Data.Resources[i];
                    const auto& Overdraw = Data.MeshStatistics.OverdrawStatics[i];
                    const auto& Fetch    = Data.MeshStatistics.VertexFetchStatics[i];

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(R.Name.c_str());
                    ImGui::TableNextColumn(); ImGuiX::Text("{0}", ImGuiX::FormatSize(R.GetNumVertices()));
                    ImGui::TableNextColumn(); ImGuiX::Text("{0}", ImGuiX::FormatSize(R.Indices.size()));
                    ImGui::TableNextColumn(); ImGuiX::Text("{0}", R.GeometrySurfaces.size());

                    ImGui::TableNextColumn();
                    if (Overdraw.overdraw > 2.0f) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.55f, 0.45f, 1));
                    ImGuiX::Text("{:.2f}", Overdraw.overdraw);
                    if (Overdraw.overdraw > 2.0f) ImGui::PopStyleColor();

                    ImGui::TableNextColumn();
                    if (Fetch.overfetch > 2.0f) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.55f, 0.45f, 1));
                    ImGuiX::Text("{:.2f}", Fetch.overfetch);
                    if (Fetch.overfetch > 2.0f) ImGui::PopStyleColor();
                }

                ImGui::EndTable();
            }
            ImGui::Spacing();
        }

        void DrawTexturesPreview(const FMeshImportData& Data)
        {
            if (Data.Textures.empty())
            {
                return;
            }

            FFixedString Header(FFixedString::CtorSprintf(), "Textures (%zu)###Textures", Data.Textures.size());
            if (!ImGui::CollapsingHeader(Header.c_str()))
            {
                return;
            }

            TVector<FMeshImportImage> Images;
            Images.assign(Data.Textures.begin(), Data.Textures.end());

            constexpr float ThumbSize = 64.0f;
            constexpr ImGuiTableFlags Flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInner;
            if (ImGui::BeginTable("TextureTable", 2, Flags))
            {
                ImGui::TableSetupColumn("##thumb", ImGuiTableColumnFlags_WidthFixed, ThumbSize + 8);
                ImGui::TableSetupColumn("Path",    ImGuiTableColumnFlags_WidthStretch);

                ImGuiListClipper Clipper;
                Clipper.Begin((int)Images.size(), ThumbSize + 8);
                while (Clipper.Step())
                {
                    for (int i = Clipper.DisplayStart; i < Clipper.DisplayEnd; ++i)
                    {
                        const FMeshImportImage& Img = Images[i];
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (Img.DisplayImage.IsValid())
                        {
                            ImGui::Image(ImGuiX::ToImTextureRef(Img.DisplayImage), ImVec2(ThumbSize, ThumbSize));
                        }
                        ImGui::TableNextColumn();
                        ImGui::AlignTextToFramePadding();
                        ImGuiX::TextWrapped("{0}", Img.RelativePath);
                    }
                }
                ImGui::EndTable();
            }
            ImGui::Spacing();
        }

        void DrawSkeletonsPreview(FMeshImportData& Data)
        {
            if (Data.Skeletons.empty())
            {
                return;
            }

            FFixedString Header(FFixedString::CtorSprintf(), "Skeletons (%zu)###Skeletons", Data.Skeletons.size());
            if (!ImGui::CollapsingHeader(Header.c_str()))
            {
                return;
            }

            for (TUniquePtr<FSkeletonResource>& Skeleton : Data.Skeletons)
            {
                ImGui::PushID(Skeleton.get());

                bool bImport = Skeleton->bShouldImport;
                if (ImGui::Checkbox("##import", &bImport))
                {
                    Skeleton->bShouldImport = bImport;
                }
                ImGui::SameLine();
                if (ImGui::TreeNodeEx(Skeleton->Name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth))
                {
                    auto DrawBone = [&](int32 BoneIdx, auto& Self) -> void
                    {
                        const FSkeletonResource::FBoneInfo& Bone = Skeleton->Bones[BoneIdx];
                        TVector<int32> Children = Skeleton->GetChildBones(BoneIdx);
                        const ImGuiTreeNodeFlags Flags = Children.empty()
                            ? ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen
                            : ImGuiTreeNodeFlags_None;
                        if (ImGui::TreeNodeEx(Bone.Name.c_str(), Flags))
                        {
                            for (int32 ChildIdx : Children)
                            {
                                Self(ChildIdx, Self);
                            }
                            if (!Children.empty())
                            {
                                ImGui::TreePop();
                            }
                        }
                    };
                    for (int32 RootIdx : Skeleton->GetRootBones())
                    {
                        DrawBone(RootIdx, DrawBone);
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::Spacing();
        }

        void DrawAnimationsPreview(const FMeshImportData& Data)
        {
            if (Data.Animations.empty())
            {
                return;
            }

            FFixedString Header(FFixedString::CtorSprintf(), "Animations (%zu)###Animations", Data.Animations.size());
            if (!ImGui::CollapsingHeader(Header.c_str()))
            {
                return;
            }

            for (size_t i = 0; i < Data.Animations.size(); ++i)
            {
                const FAnimationResource& Anim = *Data.Animations[i];
                ImGui::PushID((int)i);
                if (ImGui::TreeNodeEx(Anim.Name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth))
                {
                    ImGui::TextDisabled("Duration: %.2fs   Channels: %zu", Anim.Duration, Anim.Channels.size());
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::Spacing();
        }

    }

    void CMeshFactory::PrepareImportAsync(const FFixedString& RawPath, const FFixedString& DestinationPath, FImportPrepareCallback OnReady)
    {
        using namespace Import::Mesh;
        
        Task::AsyncTask(1, 1, [RawPath, OnReady = Move(OnReady)](uint32, uint32, uint32) mutable
        {
            FScopedSlowTask SlowTask(1.0f, "Reading Mesh", "Parsing source file...");

            auto Data = MakeUnique<FMeshImportData>();
            const bool bOk = PreviewParse(RawPath, *Data, &SlowTask);
            
            if (bOk)
            {
                BuildPreviewThumbnails(RawPath, *Data, SlowTask);
            }

            // Hand the fully-prepared result back to the main thread to open the dialog.
            MainThread::Enqueue([OnReady = Move(OnReady), Data = Move(Data), bOk]() mutable
            {
                if (bOk)
                {
                    OnReady(Move(Data));
                }
                else
                {
                    OnReady(nullptr);
                }
            });
        });
    }

    namespace
    {
        // UI state for the options widgets, distinct from the parsed source data. Still a static, so
        // it carries between files -- which is what a batch will want, but it means two importers
        // cannot be open at once. The import window enforces that today.
        Import::Mesh::FMeshImportOptions GMeshImportOptions;
    }

    void CMeshFactory::DrawImportSettings(const FFixedString& RawPath, Import::FImportSettings& Settings)
    {
        using namespace Import::Mesh;

        // Settings arrive fully parsed: PrepareImportAsync ran the source-file parse off-thread and
        // the window is only shown once the result (and thumbnails) landed.
        FMeshImportData& ImportedData = static_cast<FMeshImportData&>(Settings);

        // Each section is a collapsing header; without a gap between them they read as one wall of
        // controls rather than five things you can consider separately.
        auto Section = [](auto&& Draw)
        {
            Draw();
            ImGui::Spacing();
            ImGui::Spacing();
        };

        Section([&] { DrawOptionsSection(GMeshImportOptions); });
        Section([&] { DrawMeshStats(ImportedData); });
        Section([&] { DrawTexturesPreview(ImportedData); });
        Section([&] { DrawSkeletonsPreview(ImportedData); });
        Section([&] { DrawAnimationsPreview(ImportedData); });
    }

    void CMeshFactory::CommitImportSettings(Import::FImportSettings& Settings)
    {
        static_cast<Import::Mesh::FMeshImportData&>(Settings).CommitOptions = GMeshImportOptions;
    }

    bool CMeshFactory::CanReimport(const CStruct* AssetClass) const
    {
        return AssetClass != nullptr && AssetClass->IsChildOf(CMesh::StaticClass());
    }

    FString CMeshFactory::GetReimportSourcePath(const CObject* Asset) const
    {
        const CMesh* Mesh = Cast<CMesh>(Asset);
        return Mesh != nullptr ? Mesh->SourcePath : FString();
    }

    namespace
    {
        // Which parsed sub-mesh replaces the asset. A source file routinely holds several meshes, so this
        // prefers the one whose internal name matches the asset (survives reordered exports) and falls back
        // to the first (covers the single-mesh file, and the case where the artist renamed the mesh).
        int32 FindResourceForAsset(const Import::Mesh::FMeshImportData& Data, const FName& AssetName, bool bWantSkinned)
        {
            int32 FirstCompatible = INDEX_NONE;

            const FString TargetName = Import::SanitizeAssetName(AssetName.c_str());

            for (size_t i = 0; i < Data.Resources.size(); ++i)
            {
                const TUniquePtr<FMeshResource>& Resource = Data.Resources[i];
                if (!Resource)
                {
                    continue;
                }

                // A skinned resource cannot stand in for a CStaticMesh (or the reverse): the vertex stream
                // it populates is the other one, so the swap would leave every reader looking at an empty
                // buffer rather than at wrong-but-present data.
                if (Resource->bSkinnedMesh != bWantSkinned)
                {
                    continue;
                }

                if (FirstCompatible == INDEX_NONE)
                {
                    FirstCompatible = (int32)i;
                }

                // Compared sanitized, because the asset name went through the same cleanup on the way in.
                if (Import::SanitizeAssetName(Resource->Name.c_str()) == TargetName)
                {
                    return (int32)i;
                }
            }

            return FirstCompatible;
        }

        // Carries the old material assignments onto the new slots. Keyed off surface names rather than raw
        // slot index, so a source whose materials were reordered keeps its overrides; index is the fallback
        // for surfaces that were renamed or added.
        void RemapMaterialSlots(const FMeshResource& OldResource,
                                const TVector<TObjectPtr<CMaterialInterface>>& OldMaterials,
                                const FMeshResource& NewResource,
                                TVector<TObjectPtr<CMaterialInterface>>& OutMaterials)
        {
            size_t SlotCount = 0;
            bool bAnyExplicitMaterial = false;
            for (const FGeometrySurface& Surface : NewResource.GeometrySurfaces)
            {
                if (Surface.MaterialIndex >= 0)
                {
                    bAnyExplicitMaterial = true;
                    SlotCount = eastl::max(SlotCount, (size_t)Surface.MaterialIndex + 1);
                }
            }
            if (!bAnyExplicitMaterial)
            {
                SlotCount = NewResource.GeometrySurfaces.size();
            }

            OutMaterials.clear();
            OutMaterials.resize(SlotCount);

            TVector<bool> bSlotResolved;
            bSlotResolved.resize(SlotCount, false);

            for (const FGeometrySurface& NewSurface : NewResource.GeometrySurfaces)
            {
                const int32 NewSlot = NewSurface.MaterialIndex;
                if (NewSlot < 0 || (size_t)NewSlot >= SlotCount || bSlotResolved[NewSlot])
                {
                    continue;
                }

                for (const FGeometrySurface& OldSurface : OldResource.GeometrySurfaces)
                {
                    if (OldSurface.ID != NewSurface.ID)
                    {
                        continue;
                    }

                    if (OldSurface.MaterialIndex >= 0 && (size_t)OldSurface.MaterialIndex < OldMaterials.size())
                    {
                        OutMaterials[NewSlot] = OldMaterials[OldSurface.MaterialIndex];
                        bSlotResolved[NewSlot] = true;
                    }
                    break;
                }
            }

            // Slots no surface name accounted for keep whatever sat at the same index before.
            for (size_t Slot = 0; Slot < SlotCount; ++Slot)
            {
                if (!bSlotResolved[Slot] && Slot < OldMaterials.size())
                {
                    OutMaterials[Slot] = OldMaterials[Slot];
                }
            }
        }
    }

    bool CMeshFactory::TryReimport(CObject* Asset, const FFixedString& SourceFile, const Import::FImportSettings* Settings)
    {
        using namespace Import::Mesh;

        CMesh* Mesh = Cast<CMesh>(Asset);
        if (Mesh == nullptr || Settings == nullptr)
        {
            return false;
        }

        FMeshImportData& ImportData = const_cast<FMeshImportData&>(Settings->As<FMeshImportData>());

        const FStringView SourceName = VFS::FileName(SourceFile, true);
        FFixedString SlowTaskTitle(FFixedString::CtorSprintf(), "Reimporting %.*s", (int)SourceName.length(), SourceName.data());
        FScopedSlowTask SlowTask(1.0f, SlowTaskTitle, "Processing geometry...");

        // Only the geometry matters here. Reimport replaces one asset's data; it does not mint the
        // skeletons, animations, materials and textures a fresh import of the same file would, because
        // those are separate assets with their own identities and references.
        FMeshImportOptions Options = ImportData.CommitOptions;
        Options.bImportMeshes     = true;
        Options.bImportSkeleton   = false;
        Options.bImportAnimations = false;
        Options.bImportMaterials  = false;
        Options.bImportTextures   = false;

        // Taken from the ASSET, not the dialog: a reimport refreshes geometry against settings the asset
        // already carries, and the dialog's globals are whatever the last unrelated import happened to
        // leave behind. Without this a mesh with a field silently loses it on every reimport.
        Options.DistanceField = Mesh->DistanceFieldSettings;

        FinalizeMeshImportData(ImportData, Options, &SlowTask, 0.9f);

        const int32 ResourceIndex = FindResourceForAsset(ImportData, Mesh->GetName(), Mesh->IsSkinned());
        if (ResourceIndex == INDEX_NONE)
        {
            LOG_ERROR("Reimport: '{0}' contains no {1} mesh to replace '{2}' with.",
                      SourceFile.c_str(), Mesh->IsSkinned() ? "skinned" : "static", Mesh->GetName().c_str());
            return false;
        }

        TUniquePtr<FMeshResource>& NewResource = const_cast<TUniquePtr<FMeshResource>&>(ImportData.Resources[ResourceIndex]);
        if (!NewResource || NewResource->GeometrySurfaces.empty())
        {
            LOG_ERROR("Reimport: the mesh selected from '{0}' has no surfaces; leaving '{1}' untouched.",
                      SourceFile.c_str(), Mesh->GetName().c_str());
            return false;
        }

        SlowTask.EnterProgressFrame(0.1f, "Replacing mesh data...");

        // Name follows the ASSET, not the source: the object keeps its identity through a reimport, and a
        // resource labelled with the source's internal name would show up in every debug view as a rename.
        NewResource->Name = Mesh->GetName();

        TVector<TObjectPtr<CMaterialInterface>> RemappedMaterials;
        RemapMaterialSlots(Mesh->GetMeshResource(), Mesh->Materials, *NewResource, RemappedMaterials);
        Mesh->Materials = Move(RemappedMaterials);

        // The swap itself. Same CObject, same GUID, same package: SetMeshResource rebuilds the bounds and
        // GPU buffers and invalidates the mesh-resolve cache entries that point at this asset, so live
        // components pick the new geometry up without anything re-resolving the reference.
        Mesh->SetMeshResource(Move(NewResource));
        Mesh->SourcePath = FString(SourceFile.c_str());

        if (CPackage* Package = Mesh->GetPackage())
        {
            Package->MarkDirty();
        }

        // Deliberately NOT touched: a skeletal mesh's Skeleton reference. Rebinding it is a separate
        // decision from replacing geometry, and the existing RequiredBoneCount check already reports a rig
        // whose bone count outruns the skeleton it is still pointing at.
        return true;
    }

    void CMeshFactory::TryImport(const FFixedString& RawPath, const FFixedString& DestinationPath, const Import::FImportSettings* Settings)
    {
        using namespace Import::Mesh;

        // Finalize the preview parse in place using the user's CommitOptions.
        FMeshImportData& ImportData = const_cast<FMeshImportData&>(Settings->As<FMeshImportData>());
        const FMeshImportOptions& Options = ImportData.CommitOptions;

        // Progress budget (sums to 1.0): the geometry finalize dominates wall time, so it
        // owns most of the bar; asset creation / texture import / package save get the rest.
        constexpr float kFinalizeBudget = 0.75f;
        constexpr float kCreateBudget   = 0.05f;
        constexpr float kTextureBudget  = 0.12f;
        constexpr float kSaveBudget     = 0.08f;

        const FStringView SourceName = VFS::FileName(RawPath, true);
        FFixedString SlowTaskTitle(FFixedString::CtorSprintf(), "Importing %.*s", (int)SourceName.length(), SourceName.data());
        FScopedSlowTask SlowTask(1.0f, SlowTaskTitle, "Processing geometry...");

        FinalizeMeshImportData(ImportData, Options, &SlowTask, kFinalizeBudget);

        FFixedString DestinationDir;
        FFixedString BaseName;
        const size_t LastSlashPos = DestinationPath.find_last_of('/');
        if (LastSlashPos == FFixedString::npos)
        {
            BaseName = DestinationPath;
        }
        else
        {
            DestinationDir = DestinationPath.substr(0, LastSlashPos + 1);
            BaseName       = DestinationPath.substr(LastSlashPos + 1, FFixedString::npos);
        }
        // Strip source-file extension so asset paths don't carry it.
        const size_t DotPos = BaseName.find_last_of('.');
        if (DotPos != FFixedString::npos)
        {
            BaseName = BaseName.substr(0, DotPos);
        }
        
        FFixedString TexturesDir = DestinationDir;
        TexturesDir.append("Textures/");
        FFixedString MaterialsDir = DestinationDir;
        MaterialsDir.append("Materials/");

        auto BuildPath = [&](FStringView Suffix) -> FFixedString
        {
            FFixedString Path = DestinationDir;
            Path.append(BaseName);
            if (!Suffix.empty())
            {
                Path.append("_");
                Path.append_convert(Suffix.data(), Suffix.length());
            }
            return Path;
        };

        // Avoid clobbering existing assets on reimport or sub-asset name collisions.
        auto EnsureUniquePath = [](FFixedString Path) -> FFixedString
        {
            if (FindObject<CPackage>(Path) == nullptr)
            {
                return Path;
            }
            for (uint32 N = 1; N < 10000; ++N)
            {
                FFixedString Candidate = Path;
                Candidate.append("_");
                Candidate.append_convert(eastl::to_string(N));
                if (FindObject<CPackage>(Candidate) == nullptr)
                {
                    return Candidate;
                }
            }
            return Path;
        };
        
        SlowTask.EnterProgressFrame(kCreateBudget, "Creating assets...");

        TVector<CObject*> CreatedObjects;
        CreatedObjects.reserve(ImportData.Skeletons.size() + ImportData.Resources.size() + ImportData.Animations.size());

        // Imported meshes whose material slots get the generated material instances assigned below.
        TVector<CMesh*> CreatedMeshes;

        TObjectPtr<CSkeleton> PrimarySkeleton;
        const bool bMultipleSkeletons = ImportData.Skeletons.size() > 1;

        for (size_t i = 0; Options.bImportSkeleton && i < ImportData.Skeletons.size(); ++i)
        {
            TUniquePtr<FSkeletonResource>& SkeletonRes = const_cast<TUniquePtr<FSkeletonResource>&>(ImportData.Skeletons[i]);
            if (!SkeletonRes || !SkeletonRes->bShouldImport)
            {
                continue;
            }

            // Disambiguate multi-skeleton sources by internal name.
            FFixedString SkeletonPath = bMultipleSkeletons
                ? BuildPath(SkeletonRes->Name.ToString())
                : BuildPath("Skeleton");
            SkeletonPath = EnsureUniquePath(SkeletonPath);

            CSkeleton* NewSkeleton = CreateNewOf<CSkeleton>(SkeletonPath);
            NewSkeleton->SetFlag(OF_NeedsPostLoad);
            NewSkeleton->SkeletonResource = Move(SkeletonRes);

            if (!PrimarySkeleton)
            {
                PrimarySkeleton = NewSkeleton;
            }
            CreatedObjects.push_back(NewSkeleton);
        }

        const bool bMultipleMeshes = ImportData.Resources.size() > 1;
        for (size_t i = 0; Options.bImportMeshes && i < ImportData.Resources.size(); ++i)
        {
            TUniquePtr<FMeshResource>& MeshResource = const_cast<TUniquePtr<FMeshResource>&>(ImportData.Resources[i]);
            if (!MeshResource)
            {
                continue;
            }


            FFixedString MeshPath = bMultipleMeshes
                ? BuildPath(MeshResource->Name.ToString())
                : BuildPath({});
            MeshPath = EnsureUniquePath(MeshPath);

            CMesh* NewMesh = nullptr;
            if (!MeshResource->bSkinnedMesh)
            {
                NewMesh = CreateNewOf<CStaticMesh>(MeshPath);
            }
            else
            {
                CSkeletalMesh* NewSkeletalMesh = CreateNewOf<CSkeletalMesh>(MeshPath);
                if (PrimarySkeleton)
                {
                    NewSkeletalMesh->Skeleton = PrimarySkeleton;
                    if (!PrimarySkeleton->PreviewMesh)
                    {
                        PrimarySkeleton->PreviewMesh = NewSkeletalMesh;
                    }
                }
                NewMesh = NewSkeletalMesh;
            }

            NewMesh->SetFlag(OF_NeedsPostLoad);


            // Size from highest referenced material index; merge-mode dedups onto shared slots.
            size_t MaterialSlotCount = 0;
            bool   bAnyExplicitMaterial = false;
            for (const FGeometrySurface& Surface : MeshResource->GeometrySurfaces)
            {
                if (Surface.MaterialIndex >= 0)
                {
                    bAnyExplicitMaterial = true;
                    MaterialSlotCount = eastl::max(MaterialSlotCount, (size_t)Surface.MaterialIndex + 1);
                }
            }
            if (!bAnyExplicitMaterial)
            {
                MaterialSlotCount = MeshResource->GeometrySurfaces.size();
            }
            NewMesh->Materials.clear();
            NewMesh->Materials.resize(MaterialSlotCount);

            // Recorded so "Reimport From File..." opens on the file this came from.
            NewMesh->SourcePath = FString(RawPath.c_str());

            // Seeded from the import dialog, so the asset remembers what its field was built with and a
            // later rebuild from the mesh editor reproduces it rather than falling back to the defaults.
            NewMesh->DistanceFieldSettings = Options.DistanceField;

            NewMesh->MeshResources = Move(MeshResource);
            CreatedObjects.push_back(NewMesh);
            CreatedMeshes.push_back(NewMesh);
        }

        const bool bMultipleAnims = ImportData.Animations.size() > 1;
        for (size_t i = 0; Options.bImportAnimations && i < ImportData.Animations.size(); ++i)
        {
            TUniquePtr<FAnimationResource>& Clip = const_cast<TUniquePtr<FAnimationResource>&>(ImportData.Animations[i]);
            if (!Clip)
            {
                continue;
            }

            FFixedString AnimPath = bMultipleAnims
                ? BuildPath(Clip->Name.ToString())
                : BuildPath("Animation");
            AnimPath = EnsureUniquePath(AnimPath);

            CAnimation* NewAnimation = CreateNewOf<CAnimation>(AnimPath);
            NewAnimation->SetFlag(OF_NeedsPostLoad);
            NewAnimation->AnimationResource = Move(Clip);
            NewAnimation->Skeleton = PrimarySkeleton;

            CreatedObjects.push_back(NewAnimation);
        }
        
        FScopedAssetRegistryBatch RegistryBatch;

        SlowTask.UpdateMessage("Importing textures...");

        THashMap<FFixedString, CTexture*> TextureMap;
        
        const bool bWantTextures = Options.bImportTextures || Options.bImportMaterials;

        if (bWantTextures && !ImportData.Textures.empty())
        {
            TVector<FMeshImportImage> Images(ImportData.Textures.begin(), ImportData.Textures.end());
            CTextureFactory* TextureFactory = CTextureFactory::StaticClass()->GetDefaultObject<CTextureFactory>();
            
            for (FMeshImportImage& Img : Images)
            {
                Img.EncodeThreadBudget = 1;
            }
            
            struct FTextureWork
            {
                FFixedString SourcePath;     // empty for mesh-embedded bytes
                FFixedString QualifiedPath;  // destination package path (with extension)
                size_t       ImageIndex;
                bool         bNeedsImport;
            };

            TVector<FTextureWork> Work;
            Work.reserve(Images.size());
            THashSet<FFixedString> SeenPaths;

            // Destination asset name: T_<clean stem> (directory + extension stripped, sanitized). The TextureMap
            // below is still keyed by the source RelativePath, so renaming the asset doesn't break material binding.
            auto TextureAssetName = [](FStringView Raw) -> FFixedString
            {
                const size_t Slash = Raw.find_last_of("/\\");
                FStringView Stem = (Slash == FStringView::npos) ? Raw : Raw.substr(Slash + 1);
                const size_t Dot = Stem.find_last_of('.');
                if (Dot != FStringView::npos && Dot > 0)
                {
                    Stem = Stem.substr(0, Dot);
                }
                return FFixedString(Import::MakeAssetName("T_", Stem).c_str());
            };

            for (size_t i = 0; i < Images.size(); ++i)
            {
                const FMeshImportImage& Texture = Images[i];

                FFixedString SourcePath;
                FFixedString QualifiedPath;
                if (Texture.IsBytes())
                {
                    QualifiedPath = Paths::Combine(TexturesDir, TextureAssetName(Texture.RelativePath).c_str());
                }
                else
                {
                    FStringView ParentPath = VFS::Parent(RawPath, true);
                    SourcePath.append_convert(ParentPath.data(), ParentPath.length()).append("/").append_convert(Texture.RelativePath);
                    FStringView TextureFileName = VFS::FileName(SourcePath, true);

                    QualifiedPath = Paths::Combine(TexturesDir, TextureAssetName(TextureFileName).c_str());
                }

                // Existence check (no extension), then add the extension for import/load.
                const bool bAlreadyExists = (FindObject<CPackage>(QualifiedPath) != nullptr);
                CPackage::AddPackageExt(QualifiedPath);

                // Import a given destination exactly once: skip if it already exists, or if an earlier item this
                // batch already claimed the same path (duplicate source filenames). Both still load + map below.
                const bool bDuplicate = !SeenPaths.insert(QualifiedPath).second;

                FTextureWork W;
                W.SourcePath    = Move(SourcePath);
                W.QualifiedPath = Move(QualifiedPath);
                W.ImageIndex    = i;
                W.bNeedsImport  = !bAlreadyExists && !bDuplicate;
                Work.push_back(Move(W));
            }
            
            Task::ParallelFor((uint32)Work.size(), [&](uint32 i)
            {
                const FTextureWork& W = Work[i];
                if (W.bNeedsImport)
                {
                    // Settings carry IntendedColorSpace + the single-thread encode budget set above.
                    TextureFactory->Import(W.SourcePath, W.QualifiedPath, &Images[W.ImageIndex]);
                }
            }, 1);
            
            if (Options.bImportMaterials)
            {
                for (const FTextureWork& W : Work)
                {
                    if (CTexture* Loaded = LoadObject<CTexture>(W.QualifiedPath))
                    {
                        TextureMap.emplace(Images[W.ImageIndex].RelativePath, Loaded);
                    }
                }
            }

            SlowTask.EnterProgressFrame(kTextureBudget);
        }
        else
        {
            // No textures to import; still advance this phase's slice of the bar.
            SlowTask.EnterProgressFrame(kTextureBudget);
        }

        // Generate the PBR master material(s) + per-source-material instances and assign them to mesh slots so
        // imported meshes render with their authored materials out of the box.
        if (Options.bImportMaterials && !ImportData.Materials.empty())
        {
            SlowTask.UpdateMessage("Generating materials...");

            auto GenerateAndAssign = [&]()
            {
                const TVector<CMaterialInstance*> Instances =
                    Import::Materials::GenerateMaterials(ImportData, MaterialsDir, BaseName, TextureMap, CreatedObjects);

                const TVector<int16>& SlotToSource = ImportData.MergedMaterialSlotToSource;
                for (CMesh* Mesh : CreatedMeshes)
                {
                    Mesh->ForEachSurface([&](const FGeometrySurface& Surface, uint32)
                    {
                        const int32 Slot = Surface.MaterialIndex;
                        if (Slot < 0 || (size_t)Slot >= Mesh->GetNumMaterials())
                        {
                            return;
                        }

                        // Identity unless merge mode remapped source indices into dense slots.
                        int32 SourceIndex = Slot;
                        if (!SlotToSource.empty())
                        {
                            SourceIndex = (Slot < (int32)SlotToSource.size()) ? SlotToSource[Slot] : -1;
                        }

                        if (SourceIndex >= 0 && (size_t)SourceIndex < Instances.size() && Instances[SourceIndex] != nullptr)
                        {
                            Mesh->SetMaterialAtSlot((size_t)Slot, Instances[SourceIndex]);
                        }
                    });
                }
            };

            // Material generation compiles shaders via GShaderCompiler->Flush(), whose hard atomic_wait would
            // stall a worker fiber.
            if (Threading::IsMainThread())
            {
                GenerateAndAssign();
            }
            else
            {
                TPromise<void> Promise;
                TFuture<void> Future = Promise.GetFuture();
                MainThread::Enqueue([&GenerateAndAssign, Promise = Move(Promise)]() mutable
                {
                    GenerateAndAssign();
                    Promise.SetValue();
                });
                Future.Wait();
            }
        }

        SlowTask.UpdateMessage("Saving packages...");

        const float SaveStep = kSaveBudget / (float)eastl::max<size_t>((size_t)1, CreatedObjects.size());
        for (CObject* Obj : CreatedObjects)
        {
            CPackage* Package = Obj->GetPackage();
            if (CPackage::SavePackage(Package, Package->GetPackagePath()))
            {
                // The generated material node graph rides along in its master's package (saved with it) but is
                // not itself a browsable asset; only register real assets in the registry.
                if (Obj->IsAsset())
                {
                    FAssetRegistry::Get().AssetCreated(Obj);
                }
            }
            else
            {
                LOG_ERROR("MeshFactory: failed to save {}; asset will not be registered", Package->GetPackagePath());
            }

            SlowTask.EnterProgressFrame(SaveStep);
        }
        if (CreatedObjects.empty())
        {
            SlowTask.EnterProgressFrame(kSaveBudget);
        }
        
        if (PrimarySkeleton)
        {
            PrimarySkeleton->PreviewMesh = nullptr;
        }
        PrimarySkeleton = nullptr;

        for (auto It = CreatedObjects.rbegin(); It != CreatedObjects.rend(); ++It)
        {
            (*It)->ConditionalBeginDestroy();
        }
    }
}
