#include "AssetCooker.h"
#include <string>
#include "Platform/Filesystem/PlatformFilesystem.h"
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
#include "Cooker/Analyzers/RmlUiAssetScan.h"
#include "Cooker/CookDDC.h"
#include "Cooker/Graph/CookGraph.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"
#include "Pak/PakWriter.h"
#include "World/World.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
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

        bool BundleVfsFile(FPakWriter& Writer, FStringView VirtualPath, const TFunction<void(FStringView)>& LogFunc)
        {
            // Sidecars are editor-only, since the cooked AssetRegistry.bin already carries the GUID table.
            if (TextAssetSidecar::IsSidecarPath(VirtualPath))
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
        bool BundleAssetCooked(FPakWriter& Writer, FStringView VirtualPath, const TFunction<void(FStringView)>& LogFunc)
        {
            FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(VirtualPath);
            const uint64 SourceHash = Data ? Data->ContentHash : 0;
            const FCookInputHash Key = FCookDDC::ComputeKey(SourceHash);

            TVector<uint8> CookedBytes;
            if (FCookDDC::TryGet(Key, CookedBytes))
            {
                Writer.AddEntry(VirtualPath, TSpan<const uint8>(CookedBytes.data(), CookedBytes.size()));
                LogCooker(LogFunc, Format("  + {} (ddc, {} bytes)",
                    VirtualPath,
                    CookedBytes.size()).c_str());
                return true;
            }

            CPackage* Package = CPackage::LoadPackage(VirtualPath);
            if (Package == nullptr)
            {
                LogCooker(LogFunc, Format("  [warn] failed to load for cook, falling back to verbatim: {}",
                    VirtualPath).c_str());
                return BundleVfsFile(Writer, VirtualPath, LogFunc);
            }

            if (!CPackage::SavePackageForCook(Package, CookedBytes))
            {
                LogCooker(LogFunc, Format("  [warn] cook-save failed, falling back to verbatim: {}",
                    VirtualPath).c_str());
                return BundleVfsFile(Writer, VirtualPath, LogFunc);
            }

            // Silent failure is fine, since the freshly-cooked bytes still ship and only the cache misses.
            FCookDDC::Put(Key, CookedBytes);

            Writer.AddEntry(VirtualPath, TSpan<const uint8>(CookedBytes.data(), CookedBytes.size()));
            LogCooker(LogFunc, Format("  + {} (cooked, {} bytes)",
                VirtualPath,
                CookedBytes.size()).c_str());
            return true;
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

        FStringView LeafName(FStringView Path)
        {
            const size_t Slash = Path.find_last_of("/\\");
            return Slash == FStringView::npos ? Path : Path.substr(Slash + 1);
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
                if (BundleVfsFile(Writer, Info.VirtualPath.c_str(), LogFunc))
                {
                    ++Count;
                }
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

        LogCooker(LogFunc, Format("Building cook graph from {} root(s)...", Roots.size()).c_str());

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
                Root.Chunk = FName("Primary");
                Graph.AddRoot(Root);
            }
        }

        // The project root subdirs plus every enabled plugin's mount.
        TVector<FString> ContentRoots;
        ContentRoots.emplace_back("/Game/Content");
        ContentRoots.emplace_back("/Game/Scripts");
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
                Root.Chunk = FName("UI");
                Graph.AddRoot(Root);
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

            for (const FCookNode* Node : ByChunk[Chunk])
            {
                FStringView Vp(Node->Path.c_str(), Node->Path.size());
                if (BundleAssetCooked(Writer, Vp, LogFunc))
                {
                    ++Result.NumAssetsCooked;
                }
            }

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
            if (!Writer.Finalize(OutPath))
            {
                Result.ErrorMessage = FString("Failed to write PAK at ") + OutPath;
                return Result;
            }

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
