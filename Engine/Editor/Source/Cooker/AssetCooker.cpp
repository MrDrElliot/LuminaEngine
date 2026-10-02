#include "AssetCooker.h"
#include <string>
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Paths/Paths.h"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/CookRoot.h"
#include "Assets/AssetRegistry/TextAssetSidecar.h"
#include "Config/Config.h"
#include "Core/Engine/Engine.h"
#include "Core/Object/Object.h"
#include "Core/Object/Package/Package.h"
#include "Core/Plugin/Plugin.h"
#include "Core/Plugin/PluginManager.h"
#include "Core/Serialization/Package/PackageSaver.h"
#include "Cooker/Analyzers/CSharpAssetScan.h"
#include "Cooker/Analyzers/RmlUiAssetScan.h"
#include "Cooker/CookDDC.h"
#include "Cooker/Graph/CookGraph.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Core/Object/Class.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Platform/Time/PlatformTime.h"
#include "TaskSystem/TaskSystem.h"
#include "Core/Object/Cast.h"
#include "UI/Tools/NodeGraph/Material/MaterialGraphCompile.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"
#include "Pak/PakWriter.h"
#include "Renderer/ShaderCache.h"
#include "World/World.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    // Roots the cooker finds itself land in the one main pak, and only a plugin that names a chunk splits one off.
    static const FName kMainChunkName("Main");

    static TConsoleVar CVarCookVerifyStrip("Cook.VerifyStripPerClass", 0,
        "Cooks this many file-cooked packages of each class through a load and save too, and logs whether the bytes match");

    namespace
    {
        void LogCooker(const TFunction<void(FStringView)>& LogFunc, FStringView Msg)
        {
            if (LogFunc)
            {
                LogFunc(Msg);
            }
            LOG_INFO("[Cooker] {}", FString(Msg.data(), Msg.size()).c_str());
        }

        FStringView LeafName(FStringView Path)
        {
            const size_t Slash = Path.find_last_of("/\\");
            return Slash == FStringView::npos ? Path : Path.substr(Slash + 1);
        }

        // IDE state, build output, project files and source-control markers, none of which a game reads.
        bool IsDevelopmentOnlyFile(FStringView VirtualPath)
        {
            constexpr FStringView Folders[] = { "/obj/", "/bin/", "/.idea/", "/.vs/", "/.git/" };
            for (const FStringView Folder : Folders)
            {
                if (VirtualPath.find(Folder) != FStringView::npos)
                {
                    return true;
                }
            }

            // C# sources too, since the game loads the script assemblies the packager prebuilt.
            constexpr FStringView Suffixes[] = { ".cs", ".csproj", ".sln", ".user", ".DotSettings", "/.gitkeep", "/.gitignore", "/.hidden",
                                                 "/Config/EditorSession.json" };
            for (const FStringView Suffix : Suffixes)
            {
                if (VirtualPath.ends_with(Suffix))
                {
                    return true;
                }
            }
            return false;
        }

        bool BundleVfsFile(FPakWriter& Writer, FStringView VirtualPath, const TFunction<void(FStringView)>& LogFunc)
        {
            // Sidecars are editor-only, since the cooked AssetRegistry.bin already carries the GUID table.
            if (TextAssetSidecar::IsSidecarPath(VirtualPath) || IsDevelopmentOnlyFile(VirtualPath))
            {
                return false;
            }

            TVector<uint8> Bytes;
            if (!VFS::ReadFile(Bytes, VirtualPath))
            {
                LogCooker(LogFunc, Format("  [skip] missing: {}", VirtualPath).c_str());
                return false;
            }

            const TSpan<const uint8> Span(Bytes.data(), Bytes.size());
            Writer.AddEntry(VirtualPath, Span);

            LogCooker(LogFunc, Format("  + {} ({} bytes)",
                VirtualPath,
                Bytes.size()).c_str());
            return true;
        }

        // One bad asset falls back to a verbatim copy with a WARN rather than killing the whole cook.
        // Script packages of modules the shipped game never loads, whose objects in an asset are editor state such as a node graph.
        const THashSet<FName>& EditorScriptPackages()
        {
            static const THashSet<FName> Packages = []
            {
                THashSet<FName> Out;
                Out.insert(FName("/Script/Editor"));
                for (const FPlugin* Plugin : FPluginManager::Get().GetAllPlugins())
                {
                    for (const FPluginModuleDescriptor& Module : Plugin->GetDescriptor().Modules)
                    {
                        if (Module.Type != EPluginModuleType::Runtime)
                        {
                            Out.insert(FName((FString("/Script/") + Module.Name).c_str()));
                        }
                    }
                }
                return Out;
            }();
            return Packages;
        }

        // A graph's nodes and pins are exports of their own, each an editor class, so they go by the same test.
        bool IsEditorOnlyObject(const CObject* Object)
        {
            const CClass* Class = Object->GetClass();
            return Class != nullptr && Class->GetPackage() != nullptr && EditorScriptPackages().contains(Class->GetPackage()->GetName());
        }

        bool BundleAssetCooked(FPakWriter& Writer, FStringView VirtualPath, const TFunction<void(FStringView)>& LogFunc)
        {
            FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(VirtualPath);
            const bool bMaterial = Data != nullptr && Data->AssetClass == CMaterial::StaticClass()->GetName();

            // A material's cooked bytes carry shaders built from the templates, so a template edit must miss the cache too.
            const uint64 SourceHash = Data ? Data->ContentHash ^ (bMaterial ? CMaterial::GetShaderTemplateHash() : 0) : 0;
            const FCookInputHash Key = FCookDDC::ComputeKey(SourceHash);

            TVector<uint8> CookedBytes;
            if (FCookDDC::TryGet(Key, CookedBytes))
            {
                LogCooker(LogFunc, Format("  + {} (ddc, {} bytes)",
                    VirtualPath,
                    CookedBytes.size()).c_str());
                Writer.AddEntry(VirtualPath, Move(CookedBytes), true);
                return true;
            }

            CPackage* Package = CPackage::LoadPackage(VirtualPath);
            if (Package == nullptr)
            {
                LogCooker(LogFunc, Format("  [warn] failed to load for cook, falling back to verbatim: {}",
                    VirtualPath).c_str());
                return BundleVfsFile(Writer, VirtualPath, LogFunc);
            }

            if (bMaterial)
            {
                CMaterial* Material = Cast<CMaterial>(Package->LoadObjectByName(Data->AssetName));
                const bool bStale = Material != nullptr && Material->CompiledTemplateHash != CMaterial::GetShaderTemplateHash();
                FString Error;
                if (!RecompileMaterialIfStale(Material, Error))
                {
                    LogCooker(LogFunc, Format("  [warn] {} predates the current shader templates and could not be recompiled, since {}",
                        VirtualPath, Error).c_str());
                }
                else if (bStale)
                {
                    LogCooker(LogFunc, Format("  recompiled {} against the current shader templates", VirtualPath).c_str());
                }
            }

            uint32 EditorObjects = 0;
            auto ExcludeEditorObjects = [&EditorObjects](const CObject* Object)
            {
                const bool bEditorOnly = IsEditorOnlyObject(Object);
                EditorObjects += bEditorOnly ? 1u : 0u;
                return bEditorOnly;
            };

            if (!CPackage::SavePackageForCook(Package, CookedBytes, ExcludeEditorObjects))
            {
                LogCooker(LogFunc, Format("  [warn] cook-save failed, falling back to verbatim: {}",
                    VirtualPath).c_str());
                return BundleVfsFile(Writer, VirtualPath, LogFunc);
            }

            // Silent failure is fine, since the freshly-cooked bytes still ship and only the cache misses.
            FCookDDC::Put(Key, CookedBytes);

            LogCooker(LogFunc, Format("  + {} (cooked, {} bytes, {} editor object(s) left out)",
                VirtualPath,
                CookedBytes.size(),
                EditorObjects).c_str());
            Writer.AddEntry(VirtualPath, Move(CookedBytes), true);
            return true;
        }

        bool HasEditorOnlyData(const CStruct* Struct, THashMap<const CStruct*, bool>& Memo);

        bool PropertyHasEditorOnlyData(const FProperty* Property, THashMap<const CStruct*, bool>& Memo)
        {
            if (Property->IsEditorOnly())
            {
                return true;
            }
            // Dispatched on the type tag, since some property classes come from modules built without RTTI.
            if (Property->GetType() == EPropertyTypeFlags::Struct)
            {
                const CStruct* Struct = static_cast<const FStructProperty*>(Property)->GetStruct();
                return Struct != nullptr && HasEditorOnlyData(Struct, Memo);
            }
            if (Property->GetType() == EPropertyTypeFlags::Vector)
            {
                const FProperty* Inner = static_cast<const FArrayProperty*>(Property)->GetInternalProperty();
                return Inner != nullptr && PropertyHasEditorOnlyData(Inner, Memo);
            }
            return false;
        }

        bool HasEditorOnlyData(const CStruct* Struct, THashMap<const CStruct*, bool>& Memo)
        {
            if (auto It = Memo.find(Struct); It != Memo.end())
            {
                return It->second;
            }
            // Seeded false so a struct that contains itself through an array ends the walk.
            Memo[Struct] = false;
            bool bEditorOnly = false;
            for (const FProperty* Property : Struct->GetProperties())
            {
                if (PropertyHasEditorOnlyData(Property, Memo))
                {
                    bEditorOnly = true;
                    break;
                }
            }
            Memo[Struct] = bEditorOnly;
            return bEditorOnly;
        }

        // A class whose cooked bytes equal its saved bytes minus the thumbnail, so its packages cook without being loaded.
        bool CooksWithoutLoading(const FAssetData* Data, THashMap<FName, bool>& ClassMemo, THashMap<const CStruct*, bool>& StructMemo)
        {
            if (Data == nullptr)
            {
                return false;
            }
            if (auto It = ClassMemo.find(Data->AssetClass); It != ClassMemo.end())
            {
                return It->second;
            }

            const CClass* Class = FindObject<CClass>(Data->AssetClass);

            // Materials rebuild stale shaders and strip SPIR-V, and textures drop their source, both only in a real save.
            const bool bEligible = Class != nullptr
                && !Class->IsChildOf(CMaterialInterface::StaticClass())
                && !Class->IsChildOf(CTexture::StaticClass())
                && !(Class->GetPackage() != nullptr && EditorScriptPackages().contains(Class->GetPackage()->GetName()))
                && !HasEditorOnlyData(Class, StructMemo);

            ClassMemo[Data->AssetClass] = bEligible;
            return bEligible;
        }

        void VerifyStrippedPackage(FStringView Path, const TVector<uint8>& Stripped, const TFunction<void(FStringView)>& LogFunc)
        {
            TVector<uint8> Saved;
            CPackage* Package = CPackage::LoadPackage(Path);
            const bool bSaved = Package != nullptr && CPackage::SavePackageForCook(Package, Saved, [](const CObject* Object) { return IsEditorOnlyObject(Object); });
            const bool bMatch = bSaved && Saved == Stripped;
            LogCooker(LogFunc, Format("  [verify] {} {} ({} bytes from the file, {} from a load and save)",
                bMatch ? "matches" : "differs", Path, Stripped.size(), Saved.size()).c_str());

            // Both versions are kept so a difference can be decoded and compared outside the editor.
            if (!bMatch)
            {
                const FString Dir = FString(Paths::GetEngineInstallDirectory().c_str()) + "/Intermediates/CookVerify/";
                std::filesystem::create_directories(Dir.c_str());
                const FString Stem = Dir + FString(LeafName(Path).data(), LeafName(Path).size());
                Filesystem::AtomicWriteFile(Stem + ".file", TSpan<const uint8>(Stripped.data(), Stripped.size()));
                Filesystem::AtomicWriteFile(Stem + ".saved", TSpan<const uint8>(Saved.data(), Saved.size()));
            }
        }

        // Most packages only lose their thumbnail, which needs no load, so they cook in parallel and only the rest are loaded and saved.
        void BundleChunkAssets(FPakWriter& Writer, const TVector<const FCookNode*>& Nodes, FCookResult& Result,
                               const TFunction<void(FStringView)>& LogFunc)
        {
            const double Start = PlatformTime::Seconds();

            THashMap<FName, bool> ClassMemo;
            THashMap<const CStruct*, bool> StructMemo;
            TVector<uint8> bTryStrip(Nodes.size(), 0);
            for (size_t i = 0; i < Nodes.size(); ++i)
            {
                const FStringView Path(Nodes[i]->Path.c_str(), Nodes[i]->Path.size());
                bTryStrip[i] = CooksWithoutLoading(FAssetRegistry::Get().GetAssetByPath(Path), ClassMemo, StructMemo) ? 1 : 0;
            }

            TVector<TVector<uint8>> Stripped(Nodes.size());
            TVector<uint8> bStripped(Nodes.size(), 0);
            Task::ParallelFor((uint32)Nodes.size(), [&](uint32 i)
            {
                if (bTryStrip[i] != 0)
                {
                    const FStringView Path(Nodes[i]->Path.c_str(), Nodes[i]->Path.size());
                    bStripped[i] = CPackage::StripPackageForCook(Path, Stripped[i]) ? 1 : 0;
                }
            }, 16);

            // Added in graph order whichever path cooked them, so the pak stays deterministic.
            size_t NumStripped = 0;
            size_t NumLoaded = 0;
            THashMap<FName, int32> Verified;
            for (size_t i = 0; i < Nodes.size(); ++i)
            {
                const FStringView Path(Nodes[i]->Path.c_str(), Nodes[i]->Path.size());
                if (bStripped[i] != 0 && Verified[Nodes[i]->AssetClass]++ < CVarCookVerifyStrip.GetValue())
                {
                    VerifyStrippedPackage(Path, Stripped[i], LogFunc);
                }
                if (bStripped[i] != 0)
                {
                    Writer.AddEntry(Path, Move(Stripped[i]), true);
                    ++Result.NumAssetsCooked;
                    ++NumStripped;
                }
                else if (BundleAssetCooked(Writer, Path, LogFunc))
                {
                    ++Result.NumAssetsCooked;
                    ++NumLoaded;
                }
            }

            LogCooker(LogFunc, Format("  {} asset(s) cooked from their files, {} loaded and saved, in {:.1f} s",
                NumStripped, NumLoaded, PlatformTime::Seconds() - Start).c_str());
        }

        // Every entry of a pak, one per line, so what shipped can be checked without opening the pak.
        void WritePakContents(const FPakWriter& Writer, const FString& PakPath, const TFunction<void(FStringView)>& LogFunc)
        {
            TVector<FFixedString> Paths;
            Writer.GetEntryPaths(Paths);
            Algo::Sort(Paths);

            FString Text;
            for (const FFixedString& Path : Paths)
            {
                Text.append(Path.c_str(), Path.size());
                Text.push_back('\n');
            }

            // Kept with the project rather than in the package, so the shipped folder holds only what the game reads.
            const FString ContentsDir = FString(GEngine->GetProjectPath().data(), GEngine->GetProjectPath().size()) + "/Saved/Cook";
            Filesystem::MakeDirectoryTree(FStringView(ContentsDir.c_str(), ContentsDir.size()));
            const FString ContentsPath = ContentsDir + "/" + FString(LeafName(FStringView(PakPath.c_str(), PakPath.size())).data(),
                                                                       LeafName(FStringView(PakPath.c_str(), PakPath.size())).size()) + ".contents.txt";
            std::ofstream File(ContentsPath.c_str(), std::ios::binary | std::ios::trunc);
            File.write(Text.data(), (std::streamsize)Text.size());
            if (File.good())
            {
                LogCooker(LogFunc, Format("  listed {} entries in {}", Paths.size(), ContentsPath.c_str()).c_str());
            }
        }

        // VirtualPath ends in ".lasset" (case-insensitive).
        bool IsLAssetPath(FStringView VirtualPath)
        {
            static constexpr FStringView Ext = ".lasset";
            if (VirtualPath.size() < Ext.size())
            {
                return false;
            }
            FStringView Tail = VirtualPath.substr(VirtualPath.size() - Ext.size());
            for (size_t i = 0; i < Ext.size(); ++i)
            {
                char a = Tail[i]; char b = Ext[i];
                if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
                if (a != b) return false;
            }
            return true;
        }

        // Picks up loose files loaded by name at runtime, which carry no asset-reflected references.
        size_t BundleLooseContent(FPakWriter& Writer, const TFunction<void(FStringView)>& LogFunc)
        {
            size_t Count = 0;
            auto Walk = [&](FStringView Root)
            {
                VFS::RecursiveDirectoryIterator(Root, [&](const VFS::FFileInfo& Info)
                {
                    if (Info.IsDirectory()) return;
                    FStringView Vp(Info.VirtualPath.c_str(), Info.VirtualPath.size());
                    if (IsLAssetPath(Vp)) return;
                    if (BundleVfsFile(Writer, Vp, LogFunc))
                    {
                        ++Count;
                    }
                });
            };

            Walk("/Game"); // /Game is the project root; walking it covers loose files under both Content/ and Scripts/.
            for (const FPlugin* Plugin : FPluginManager::Get().GetAllPlugins())
            {
                if (!Plugin->IsEnabled())          continue;
                if (!Plugin->IsContentMounted())   continue;
                Walk(Plugin->GetMountAlias());
            }
            return Count;
        }

        bool BundleDiskFile(FPakWriter& Writer, FStringView DiskPath, FStringView VirtualPath, const TFunction<void(FStringView)>& LogFunc)
        {
            TVector<uint8> Bytes;
            if (!Filesystem::ReadFile(Bytes, DiskPath))
            {
                LogCooker(LogFunc, Format("  [skip] unreadable extra: {}",
                    DiskPath).c_str());
                return false;
            }
            Writer.AddEntry(VirtualPath, TSpan<const uint8>(Bytes.data(), Bytes.size()));
            LogCooker(LogFunc, Format("  + {} ({} bytes, extra)",
                VirtualPath, Bytes.size()).c_str());
            return true;
        }

        size_t BundleExtras(FPakWriter& Writer, const FCookOptions& Options, const TFunction<void(FStringView)>& LogFunc)
        {
            size_t Count = 0;

            for (const FString& File : Options.ExtraFiles)
            {
                if (!Filesystem::IsFile(File))
                {
                    LogCooker(LogFunc, Format("  [skip] extra file not found: {}", File.c_str()).c_str());
                    continue;
                }

                const FStringView Leaf = LeafName(File);

                FString Vp = "/Extras/";
                Vp.append(Leaf.data(), Leaf.size());

                if (BundleDiskFile(Writer, File, FStringView(Vp.c_str(), Vp.size()), LogFunc))
                {
                    ++Count;
                }
            }

            for (const FString& Dir : Options.ExtraDirectories)
            {
                if (!Filesystem::IsDirectory(Dir))
                {
                    LogCooker(LogFunc, Format("  [skip] extra dir not found: {}", Dir.c_str()).c_str());
                    continue;
                }

                const FStringView RootName = LeafName(Dir);
                const size_t RootLength = Dir.size();

                Filesystem::IterateDirectoryRecursive(Dir, [&](const Filesystem::FDirectoryEntry& Entry)
                {
                    if (Entry.IsDirectory())
                    {
                        return;
                    }

                    const FStringView Relative = Entry.FullPath.substr(RootLength);

                    FString Vp = "/Extras/";
                    Vp.append(RootName.data(), RootName.size());
                    Vp.append(Relative.data(), Relative.size());

                    if (BundleDiskFile(Writer, Entry.FullPath, FStringView(Vp.c_str(), Vp.size()), LogFunc))
                    {
                        ++Count;
                    }
                });
            }
            return Count;
        }

        // Inject Project.Name into /Config/GameSettings.json so the cooked runtime can resolve the project DLL.
        bool BundleConfigWithProjectName(FPakWriter& Writer, const TFunction<void(FStringView)>& LogFunc)
        {
            FString JsonText;
            if (!VFS::ReadFile(JsonText, "/Config/GameSettings.json"))
            {
                JsonText = "{}";
            }

            nlohmann::json Doc;
            try
            {
                Doc = nlohmann::json::parse(JsonText.c_str());
            }
            catch (...)
            {
                Doc = nlohmann::json::object();
            }

            if (!Doc.contains("Project") || !Doc["Project"].is_object())
            {
                Doc["Project"] = nlohmann::json::object();
            }
            Doc["Project"]["Name"] = std::string(GEngine->GetProjectName().data(), GEngine->GetProjectName().size());

            const std::string Out = Doc.dump(4);
            Writer.AddEntry("/Config/GameSettings.json", FStringView(Out.c_str(), Out.size()));
            LogCooker(LogFunc, Format("  + /Config/GameSettings.json (cooked, {} bytes)", Out.size()).c_str());
            return true;
        }

        // Bundles the engine content a runtime needs, under /Engine/Resources.
        size_t BundleEngineResources(FPakWriter& Writer, const TFunction<void(FStringView)>& LogFunc)
        {
            size_t Count = 0;
            VFS::RecursiveDirectoryIterator("/Engine/Resources", [&](const VFS::FFileInfo& Info)
            {
                if (Info.IsDirectory())
                {
                    return;
                }
                
                FStringView Vp(Info.VirtualPath.c_str(), Info.VirtualPath.size());

                if (BundleVfsFile(Writer, Vp, LogFunc))
                {
                    ++Count;
                }
            });
            return Count;
        }

        // Bundle every cached SPIR-V (.lsc) under /Intermediates/ShaderCache.
        size_t BundleShaderCache(FPakWriter& Writer, const TFunction<void(FStringView)>& LogFunc)
        {
            size_t Count = 0;
            VFS::RecursiveDirectoryIterator("/Intermediates/ShaderCache", [&](const VFS::FFileInfo& Info)
            {
                if (Info.IsDirectory() || Info.GetExt() != ".lsc")
                {
                    return;
                }

                const FStringView VirtualPath(Info.VirtualPath.c_str(), Info.VirtualPath.size());
                TVector<uint8> Bytes;
                if (!VFS::ReadFile(Bytes, VirtualPath))
                {
                    return;
                }

                // The editor compiles with full debug info for Nsight, which nothing in a shipped game reads.
                TVector<uint8> Stripped;
                const bool bStripped = FShaderCache::StripCacheFileForCook(Bytes, Stripped);
                const TVector<uint8>& Shipped = bStripped ? Stripped : Bytes;

                Writer.AddEntry(VirtualPath, TSpan<const uint8>(Shipped.data(), Shipped.size()));
                LogCooker(LogFunc, Format("  + {} ({} bytes{})", VirtualPath, Shipped.size(),
                    bStripped ? Format(", {} before stripping", Bytes.size()) : FString()).c_str());
                ++Count;
            });
            return Count;
        }

        // Bundle other /Config/*.json files; GameSettings.json is handled by BundleConfigWithProjectName.
        size_t BundleAuxConfigFiles(FPakWriter& Writer, const TFunction<void(FStringView)>& LogFunc)
        {
            size_t Count = 0;
            VFS::DirectoryIterator("/Config", [&](const VFS::FFileInfo& Info)
            {
                if (Info.IsDirectory() || Info.GetExt() != ".json")
                {
                    return;
                }
                FStringView Vp(Info.VirtualPath.c_str(), Info.VirtualPath.size());
                if (Vp == FStringView("/Config/GameSettings.json"))
                {
                    return;
                }
                if (BundleVfsFile(Writer, Vp, LogFunc))
                {
                    ++Count;
                }
            });
            return Count;
        }
    }

    FCookResult FAssetCooker::Cook(FStringView OutputPakPath, const FCookOptions& Options, const TFunction<void(FStringView)>& LogFunc)
    {
        FCookResult Result;
        FCookDDC::Reset();

        if (GEngine == nullptr || GEngine->GetProjectName().empty())
        {
            Result.ErrorMessage = "No project loaded.";
            return Result;
        }

        const TVector<FCookRoot> Roots = GEngine->GetCookRoots();
        if (Roots.empty())
        {
            Result.ErrorMessage =
                "No cook roots defined.\n"
                "  Open Project Settings -> Maps -> Cook Roots and add at least one asset path,\n"
                "  declare CookRoots in a plugin's .lplugin, or flag an asset EAssetFlags::Primary.";
            return Result;
        }

        LogCooker(LogFunc, Format("Building cook graph from {} root(s), shader templates {:016x}...", Roots.size(),
            CMaterial::GetShaderTemplateHash()).c_str());

        FCookGraph Graph(FAssetRegistry::Get());
        Graph.AddRoots(Roots);

        // FindByPredicate returns hash order, so sort by GUID before adding to keep the graph deterministic.
        {
            TVector<FAssetData*> Primaries = FAssetRegistry::Get().FindByPredicate(
                [](const FAssetData& D) { return HasFlag(D.Flags, EAssetFlags::Primary); });
            Algo::Sort(Primaries,
                [](const FAssetData* A, const FAssetData* B) { return A->AssetGUID < B->AssetGUID; });
            if (!Primaries.empty())
            {
                LogCooker(LogFunc, Format("  Primary assets: {} -> implicit cook roots", Primaries.size()).c_str());
            }
            for (const FAssetData* Data : Primaries)
            {
                FCookRoot Root;
                Root.Asset = FString(Data->Path.c_str(), Data->Path.size());
                Root.Chunk = kMainChunkName;
                Graph.AddRoot(Root);
            }
        }

        // The project root subdirs plus every enabled plugin's mount.
        TVector<FString> ContentRoots;
        // All of /Game, since scripts keep the JSON they read beside them or in a data folder outside Content.
        ContentRoots.emplace_back("/Game");
        for (const FPlugin* Plugin : FPluginManager::Get().GetAllPlugins())
        {
            if (!Plugin->IsEnabled())        continue;
            if (!Plugin->IsContentMounted()) continue;
            ContentRoots.emplace_back(Plugin->GetMountAlias());
        }

        // VFS walk order is OS-dependent, so sort resolved paths before adding to keep the cook reproducible.
        {
            FRmlUiAssetScan::FResult UiScan = FRmlUiAssetScan::ScanRoots(
                ContentRoots, FAssetRegistry::Get(), LogFunc);
            Algo::Sort(UiScan.AssetPaths);
            if (UiScan.FilesScanned > 0)
            {
                LogCooker(LogFunc, Format("  UI scan: {} file(s), {} candidate ref(s), {} resolved -> implicit cook roots",
                    UiScan.FilesScanned, UiScan.RawCandidates, UiScan.ResolvedRefs).c_str());
            }
            for (const FString& Path : UiScan.AssetPaths)
            {
                FCookRoot Root;
                Root.Asset = Path;
                Root.Chunk = kMainChunkName;
                Graph.AddRoot(Root);
            }
        }

        {
            FCSharpAssetScan::FResult ScriptScan = FCSharpAssetScan::ScanRoots(ContentRoots, FAssetRegistry::Get(), LogFunc);
            Algo::Sort(ScriptScan.AssetPaths);
            Algo::Sort(ScriptScan.FolderPaths);
            if (ScriptScan.FilesScanned > 0)
            {
                LogCooker(LogFunc, Format("  Script scan: {} file(s), {} asset ref(s), {} folder ref(s) -> implicit cook roots",
                    ScriptScan.FilesScanned, ScriptScan.AssetPaths.size(), ScriptScan.FolderPaths.size()).c_str());
            }
            for (const FString& Path : ScriptScan.AssetPaths)
            {
                Graph.AddRoot(FCookRoot{ Path, kMainChunkName });
            }
            for (const FString& Folder : ScriptScan.FolderPaths)
            {
                Graph.AddFolderRoot(FStringView(Folder.c_str(), Folder.size()), kMainChunkName);
            }
        }

        Graph.Traverse();

        for (const FCookGraphIssue& Issue : Graph.GetIssues())
        {
            LogCooker(LogFunc, Format("  [warn] {}: {}",
                Issue.Source.c_str(), Issue.Detail.c_str()).c_str());
        }

        const auto Reachable = Graph.GetReachableNodesSorted();
        LogCooker(LogFunc, Format("Reachable assets: {}", Reachable.size()).c_str());

        // Sorted-by-GUID input order is preserved per chunk so PAK entry order stays deterministic.
        const FName kMainChunk("Main");
        THashMap<FName, TVector<const FCookNode*>> ByChunk;
        for (const FCookNode* Node : Reachable)
        {
            const FName Chunk = Node->Chunk.IsNone() ? kMainChunk : Node->Chunk;
            ByChunk[Chunk].push_back(Node);
        }

        // Alphabetical with Main forced first, since Main carries shared content and the verbatim path.
        TVector<FName> ChunkOrder;
        ChunkOrder.reserve(ByChunk.size());
        for (const auto& Pair : ByChunk) ChunkOrder.push_back(Pair.first);
        if (ByChunk.find(kMainChunk) == ByChunk.end())
        {
            ByChunk[kMainChunk] = {};      // ensure a Main PAK exists for shared content
            ChunkOrder.push_back(kMainChunk);
        }
        Algo::Sort(ChunkOrder,
            [&](const FName& A, const FName& B)
            {
                if (A == kMainChunk) return true;
                if (B == kMainChunk) return false;
                return A.ToString() < B.ToString();
            });

        // Main keeps the caller's name verbatim, while others become stem, chunk and extension.
        auto ChunkPakPath = [&](FName Chunk) -> FString
        {
            const FString FullPath(OutputPakPath.data(), OutputPakPath.size());
            if (Chunk == kMainChunk) return FullPath;

            const size_t DotPos = FullPath.find_last_of('.');
            const size_t SlashPos = FullPath.find_last_of("/\\");
            const bool bExtAfterSlash = DotPos != FString::npos
                && (SlashPos == FString::npos || DotPos > SlashPos);

            const FString Stem = bExtAfterSlash ? FullPath.substr(0, DotPos) : FullPath;
            const FString Ext  = bExtAfterSlash ? FullPath.substr(DotPos)    : FString(".pak");
            FString Out = Stem;
            Out += "-";
            Out += Chunk.ToString().c_str();
            Out += Ext;
            return Out;
        };

        for (const FName& Chunk : ChunkOrder)
        {
            FPakWriter Writer;
            const bool bIsMain = (Chunk == kMainChunk);

            BundleChunkAssets(Writer, ByChunk[Chunk], Result, LogFunc);

            size_t ChunkExtras = 0;
            if (bIsMain)
            {
                if (BundleConfigWithProjectName(Writer, LogFunc)) { ++ChunkExtras; }
                ChunkExtras += BundleAuxConfigFiles(Writer, LogFunc);

                LogCooker(LogFunc, "Bundling engine resources...");
                const size_t NumEngine = BundleEngineResources(Writer, LogFunc);
                ChunkExtras += NumEngine;
                LogCooker(LogFunc, Format("  bundled {} engine files", NumEngine).c_str());

                LogCooker(LogFunc, "Bundling shader cache...");
                const size_t NumShaders = BundleShaderCache(Writer, LogFunc);
                ChunkExtras += NumShaders;
                LogCooker(LogFunc, Format("  bundled {} cached shaders", NumShaders).c_str());

                if (!Options.bExtractScriptsAsLooseFiles)
                {
                    ChunkExtras += BundleLooseContent(Writer, LogFunc);
                }
                else
                {
                    LogCooker(LogFunc, "Skipping loose content in PAK (loose mode).");
                }

                if (!Options.ExtraFiles.empty() || !Options.ExtraDirectories.empty())
                {
                    LogCooker(LogFunc, "Bundling extras...");
                    ChunkExtras += BundleExtras(Writer, Options, LogFunc);
                }

                {
                    const FString Stamp = Format("{:016x}", CMaterial::GetShaderTemplateHash());
                    if (Writer.AddEntry(CMaterial::CookedShaderTemplateHashPath,
                        TSpan<const uint8>(reinterpret_cast<const uint8*>(Stamp.data()), Stamp.size())))
                    {
                        ++ChunkExtras;
                        LogCooker(LogFunc, Format("  + {} ({})", CMaterial::CookedShaderTemplateHashPath, Stamp).c_str());
                    }
                }

                // A near-empty registry usually means stale editor discovery, so warn loudly in the cook log.
                {
                    const size_t LiveCount = FAssetRegistry::Get().GetAssets().size();
                    TVector<uint8> Bytes;
                    FMemoryWriter Ar(Bytes);
                    FAssetRegistry::Get().WriteToArchive(Ar);
                    if (Writer.AddEntry("/Engine/AssetRegistry.bin",
                        TSpan<const uint8>(Bytes.data(), Bytes.size())))
                    {
                        ++ChunkExtras;
                        LogCooker(LogFunc, Format("  + /Engine/AssetRegistry.bin (cooked, {} bytes, {} live entries)",
                            Bytes.size(), LiveCount).c_str());
                    }
                    // Only warn at zero (fresh projects have few assets); zero means discovery never ran or wiped the registry, fix by deleting the .json cache + restart.
                    if (LiveCount == 0)
                    {
                        LogCooker(LogFunc, Format("  [warn] live registry has 0 entries, Shipping runtime will not find anything. "
                            "Delete <EngineInstall>/Intermediates/AssetRegistry.json and restart the editor "
                            "to force a fresh discovery, then re-cook.").c_str());
                    }
                }
            }

            const FString OutPath = ChunkPakPath(Chunk);
            const size_t ChunkBytes = Writer.TotalEntryBytes();
            Filesystem::MakeParentDirectoryTree(FStringView(OutPath.c_str(), OutPath.size()));
            if (!Writer.Finalize(OutPath))
            {
                Result.ErrorMessage = FString("Failed to write PAK at ") + OutPath;
                return Result;
            }
            WritePakContents(Writer, OutPath, LogFunc);

            FCookChunkResult ChunkResult;
            ChunkResult.Chunk     = Chunk;
            ChunkResult.PakPath   = OutPath;
            ChunkResult.NumAssets = ByChunk[Chunk].size();
            ChunkResult.NumExtras = ChunkExtras;
            ChunkResult.Bytes     = ChunkBytes;
            Result.Chunks.push_back(Move(ChunkResult));

            Result.NumExtraFiles += ChunkExtras;
            Result.TotalBytes    += ChunkBytes;

            LogCooker(LogFunc, Format("Wrote chunk '{}' -> {} ({} entries, {} bytes)",
                Chunk.ToString().c_str(), OutPath.c_str(),
                Writer.NumEntries(), ChunkBytes).c_str());
        }

        LogCooker(LogFunc, Format("DDC: {} hits, {} misses ({} bytes written this cook)",
            FCookDDC::Hits(), FCookDDC::Misses(), FCookDDC::WrittenBytes()).c_str());

        Result.bSuccess = true;
        return Result;
    }
}
