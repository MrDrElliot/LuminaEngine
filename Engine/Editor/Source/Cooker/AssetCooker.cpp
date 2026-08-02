#include "AssetCooker.h"
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
#include "Cooker/Analyzers/RmlUiAssetScan.h"
#include "Cooker/CookDDC.h"
#include "Cooker/Graph/CookGraph.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"
#include "Pak/PakWriter.h"
#include "World/World.h"

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
            // Hidden text-asset identity sidecars are editor-only; the cooked AssetRegistry.bin carries the
            // GUID<->path table, so never ship the .lmeta tree.
            if (TextAssetSidecar::IsSidecarPath(VirtualPath))
            {
                return false;
            }

            TVector<uint8> Bytes;
            if (!VFS::ReadFile(Bytes, VirtualPath))
            {
                LogCooker(LogFunc, FString().sprintf("  [skip] missing: %.*s", (int)VirtualPath.size(), VirtualPath.data()).c_str());
                return false;
            }

            const TSpan<const uint8> Span(Bytes.data(), Bytes.size());
            Writer.AddEntry(VirtualPath, Span);

            LogCooker(LogFunc, FString().sprintf("  + %.*s (%zu bytes)",
                (int)VirtualPath.size(), VirtualPath.data(),
                Bytes.size()).c_str());
            return true;
        }

        // Load + cook-mode-resave the .lasset (strips EditorOnly props + thumbnails) and bundle; DDC reuses cached bytes when the source hash is unchanged.
        // On load/resave failure, falls back to a verbatim copy with a WARN so one bad asset doesn't kill the cook (bundled but unstripped).
        bool BundleAssetCooked(FPakWriter& Writer, FStringView VirtualPath, const TFunction<void(FStringView)>& LogFunc)
        {
            FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(VirtualPath);
            const uint64 SourceHash = Data ? Data->ContentHash : 0;
            const FCookInputHash Key = FCookDDC::ComputeKey(SourceHash);

            TVector<uint8> CookedBytes;
            if (FCookDDC::TryGet(Key, CookedBytes))
            {
                Writer.AddEntry(VirtualPath, TSpan<const uint8>(CookedBytes.data(), CookedBytes.size()));
                LogCooker(LogFunc, FString().sprintf("  + %.*s (ddc, %zu bytes)",
                    (int)VirtualPath.size(), VirtualPath.data(),
                    CookedBytes.size()).c_str());
                return true;
            }

            CPackage* Package = CPackage::LoadPackage(VirtualPath);
            if (Package == nullptr)
            {
                LogCooker(LogFunc, FString().sprintf("  [warn] failed to load for cook, falling back to verbatim: %.*s",
                    (int)VirtualPath.size(), VirtualPath.data()).c_str());
                return BundleVfsFile(Writer, VirtualPath, LogFunc);
            }

            if (!CPackage::SavePackageForCook(Package, CookedBytes))
            {
                LogCooker(LogFunc, FString().sprintf("  [warn] cook-save failed, falling back to verbatim: %.*s",
                    (int)VirtualPath.size(), VirtualPath.data()).c_str());
                return BundleVfsFile(Writer, VirtualPath, LogFunc);
            }

            // Stash into DDC for next cook. Silent failure is fine, we still
            // emit the freshly-cooked bytes; the cache just won't help next time.
            FCookDDC::Put(Key, CookedBytes);

            Writer.AddEntry(VirtualPath, TSpan<const uint8>(CookedBytes.data(), CookedBytes.size()));
            LogCooker(LogFunc, FString().sprintf("  + %.*s (cooked, %zu bytes)",
                (int)VirtualPath.size(), VirtualPath.data(),
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

        // Bundle every non-.lasset file under /Game + plugin content mounts (.lasset arrives via dep-graph traversal).
        // Picks up loose files (.rml, .rcss, JSON, fonts) loaded by name at runtime with no asset-reflected refs.
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

        bool BundleDiskFile(FPakWriter& Writer, const std::filesystem::path& DiskPath, FStringView VirtualPath, const TFunction<void(FStringView)>& LogFunc)
        {
            std::ifstream In(DiskPath, std::ios::binary | std::ios::ate);
            if (!In)
            {
                LogCooker(LogFunc, FString().sprintf("  [skip] missing extra: %s", DiskPath.string().c_str()).c_str());
                return false;
            }
            const std::streamsize Size = In.tellg();
            In.seekg(0, std::ios::beg);
            TVector<uint8> Bytes((size_t)Size);
            if (Size > 0 && !In.read(reinterpret_cast<char*>(Bytes.data()), Size))
            {
                LogCooker(LogFunc, FString().sprintf("  [skip] read failed: %s", DiskPath.string().c_str()).c_str());
                return false;
            }
            Writer.AddEntry(VirtualPath, TSpan<const uint8>(Bytes.data(), Bytes.size()));
            LogCooker(LogFunc, FString().sprintf("  + %.*s (%zu bytes, extra)",
                (int)VirtualPath.size(), VirtualPath.data(), Bytes.size()).c_str());
            return true;
        }

        size_t BundleExtras(FPakWriter& Writer, const FCookOptions& Options, const TFunction<void(FStringView)>& LogFunc)
        {
            size_t Count = 0;
            std::error_code Ec;

            for (const FString& File : Options.ExtraFiles)
            {
                std::filesystem::path P(File.c_str());
                if (!std::filesystem::exists(P, Ec) || !std::filesystem::is_regular_file(P, Ec))
                {
                    LogCooker(LogFunc, FString().sprintf("  [skip] extra file not found: %s", File.c_str()).c_str());
                    continue;
                }
                FString Vp = FString("/Extras/") + P.filename().string().c_str();
                if (BundleDiskFile(Writer, P, FStringView(Vp.c_str(), Vp.size()), LogFunc))
                {
                    ++Count;
                }
            }

            for (const FString& Dir : Options.ExtraDirectories)
            {
                std::filesystem::path Root(Dir.c_str());
                if (!std::filesystem::exists(Root, Ec) || !std::filesystem::is_directory(Root, Ec))
                {
                    LogCooker(LogFunc, FString().sprintf("  [skip] extra dir not found: %s", Dir.c_str()).c_str());
                    continue;
                }
                const std::string RootName = Root.filename().string();
                for (const auto& Entry : std::filesystem::recursive_directory_iterator(Root, Ec))
                {
                    if (Ec || !Entry.is_regular_file())
                    {
                        continue;
                    }
                    std::filesystem::path Rel = std::filesystem::relative(Entry.path(), Root, Ec);
                    if (Ec)
                    {
                        continue;
                    }
                    std::string RelStr = Rel.generic_string();
                    FString Vp = FString("/Extras/") + RootName.c_str() + "/" + RelStr.c_str();
                    if (BundleDiskFile(Writer, Entry.path(), FStringView(Vp.c_str(), Vp.size()), LogFunc))
                    {
                        ++Count;
                    }
                }
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
            LogCooker(LogFunc, FString().sprintf("  + /Config/GameSettings.json (cooked, %zu bytes)", Out.size()).c_str());
            return true;
        }

        // Bundle runtime-needed engine content: /Engine/Resources/{Content, Fonts, Textures, UI,...}.
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

        LogCooker(LogFunc, FString().sprintf("Building cook graph from %zu root(s)...", Roots.size()).c_str());

        FCookGraph Graph(FAssetRegistry::Get());
        Graph.AddRoots(Roots);

        // Auto-root Primary-flagged assets (FAssetManager entry points via FPrimaryAssetId) so game code needn't list them in .lproject CookRoots[].
        // FindByPredicate's hash_set order is undefined; sort by GUID before adding so the graph builds deterministically.
        {
            TVector<FAssetData*> Primaries = FAssetRegistry::Get().FindByPredicate(
                [](const FAssetData& D) { return HasFlag(D.Flags, EAssetFlags::Primary); });
            eastl::sort(Primaries.begin(), Primaries.end(),
                [](const FAssetData* A, const FAssetData* B) { return A->AssetGUID < B->AssetGUID; });
            if (!Primaries.empty())
            {
                LogCooker(LogFunc, FString().sprintf("  Primary assets: %zu -> implicit cook roots", Primaries.size()).c_str());
            }
            for (const FAssetData* Data : Primaries)
            {
                FCookRoot Root;
                Root.Asset = FString(Data->Path.c_str(), Data->Path.size());
                Root.Chunk = FName("Primary");
                Graph.AddRoot(Root);
            }
        }

        // Content roots scanned by the UI analyzer below:
        // /Game/Content + /Game/Scripts (project-root subdirs) + every enabled plugin's mount.
        TVector<FString> ContentRoots;
        ContentRoots.emplace_back("/Game/Content");
        ContentRoots.emplace_back("/Game/Scripts");
        for (const FPlugin* Plugin : FPluginManager::Get().GetAllPlugins())
        {
            if (!Plugin->IsEnabled())        continue;
            if (!Plugin->IsContentMounted()) continue;
            ContentRoots.emplace_back(Plugin->GetMountAlias());
        }

        // Scan .rml/.rcss and fold references in as implicit Soft cook roots, else UI-only assets (e.g. a material in an .rcss decorator) get dropped by reachability.
        // VFS walk order is OS-dependent; sort resolved paths before adding so the cook is reproducible.
        {
            FRmlUiAssetScan::FResult UiScan = FRmlUiAssetScan::ScanRoots(
                ContentRoots, FAssetRegistry::Get(), LogFunc);
            eastl::sort(UiScan.AssetPaths.begin(), UiScan.AssetPaths.end());
            if (UiScan.FilesScanned > 0)
            {
                LogCooker(LogFunc, FString().sprintf(
                    "  UI scan: %zu file(s), %zu candidate ref(s), %zu resolved -> implicit cook roots",
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
            LogCooker(LogFunc, FString().sprintf("  [warn] %s: %s",
                Issue.Source.c_str(), Issue.Detail.c_str()).c_str());
        }

        const auto Reachable = Graph.GetReachableNodesSorted();
        LogCooker(LogFunc, FString().sprintf("Reachable assets: %zu", Reachable.size()).c_str());

        // Bucket reachable assets by chunk. Sorted-by-GUID input order is
        // preserved per chunk so PAK entry order is deterministic.
        const FName kMainChunk("Main");
        THashMap<FName, TVector<const FCookNode*>> ByChunk;
        for (const FCookNode* Node : Reachable)
        {
            const FName Chunk = Node->Chunk.IsNone() ? kMainChunk : Node->Chunk;
            ByChunk[Chunk].push_back(Node);
        }

        // Stable chunk order: alphabetical, with "Main" forced first (it carries shared content + uses the caller's verbatim OutputPakPath).
        TVector<FName> ChunkOrder;
        ChunkOrder.reserve(ByChunk.size());
        for (const auto& Pair : ByChunk) ChunkOrder.push_back(Pair.first);
        if (ByChunk.find(kMainChunk) == ByChunk.end())
        {
            ByChunk[kMainChunk] = {};      // ensure a Main PAK exists for shared content
            ChunkOrder.push_back(kMainChunk);
        }
        eastl::sort(ChunkOrder.begin(), ChunkOrder.end(),
            [&](const FName& A, const FName& B)
            {
                if (A == kMainChunk) return true;
                if (B == kMainChunk) return false;
                return A.ToString() < B.ToString();
            });

        // Derive per-chunk output paths from OutputPakPath. Main keeps the
        // caller's name verbatim; others become "<stem>-<chunk><ext>".
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
                LogCooker(LogFunc, FString().sprintf("  bundled %zu engine files", NumEngine).c_str());

                LogCooker(LogFunc, "Bundling shader cache...");
                const size_t NumShaders = BundleShaderCache(Writer, LogFunc);
                ChunkExtras += NumShaders;
                LogCooker(LogFunc, FString().sprintf("  bundled %zu cached shaders", NumShaders).c_str());

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

                // Pre-baked asset registry: cooked-mode runtime loads this from the VFS at boot instead of walking the filesystem. In Main so it's always present.
                // A near-empty registry usually means stale/wiped editor discovery; warn loudly so the cook log explains a future black-screen Shipping run.
                {
                    const size_t LiveCount = FAssetRegistry::Get().GetAssets().size();
                    TVector<uint8> Bytes;
                    FMemoryWriter Ar(Bytes);
                    FAssetRegistry::Get().WriteToArchive(Ar);
                    if (Writer.AddEntry("/Engine/AssetRegistry.bin",
                        TSpan<const uint8>(Bytes.data(), Bytes.size())))
                    {
                        ++ChunkExtras;
                        LogCooker(LogFunc, FString().sprintf(
                            "  + /Engine/AssetRegistry.bin (cooked, %zu bytes, %zu live entries)",
                            Bytes.size(), LiveCount).c_str());
                    }
                    // Only warn at zero (fresh projects have few assets); zero means discovery never ran or wiped the registry, fix by deleting the .json cache + restart.
                    if (LiveCount == 0)
                    {
                        LogCooker(LogFunc, FString().sprintf(
                            "  [warn] live registry has 0 entries, Shipping runtime will not find anything. "
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

            LogCooker(LogFunc, FString().sprintf("Wrote chunk '%s' -> %s (%zu entries, %zu bytes)",
                Chunk.ToString().c_str(), OutPath.c_str(),
                Writer.NumEntries(), ChunkBytes).c_str());
        }

        LogCooker(LogFunc, FString().sprintf("DDC: %zu hits, %zu misses (%zu bytes written this cook)",
            FCookDDC::Hits(), FCookDDC::Misses(), FCookDDC::WrittenBytes()).c_str());

        Result.bSuccess = true;
        return Result;
    }
}
