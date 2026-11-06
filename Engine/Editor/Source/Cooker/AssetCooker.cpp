#include "AssetCooker.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Paths/Paths.h"
#include "Assets/AssetManager/AssetManager.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/CookRoot.h"
#include "Assets/AssetRegistry/TextAssetSidecar.h"
#include "Config/Config.h"
#include "Core/Engine/Engine.h"
#include "Core/Json/Json.h"
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
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Particles/ParticleOps.h"
#include "Core/Math/Hash/Hash.h"
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
            LOG_INFO("[Cooker] {}", *FString(Msg.GetData(), Msg.Len()));
        }

        FStringView LeafName(FStringView Path)
        {
            const int32 Slash = Path.FindLastOf("/\\");
            return Slash == Constants::kIndexNone ? Path : Path.Mid(Slash + 1);
        }

        // IDE state, build output, project files and source-control markers, none of which a game reads.
        bool IsDevelopmentOnlyFile(FStringView VirtualPath)
        {
            constexpr FStringView Folders[] = { "/obj/", "/bin/", "/.idea/", "/.vs/", "/.git/" };
            for (const FStringView Folder : Folders)
            {
                if (VirtualPath.Find(Folder) != Constants::kIndexNone)
                {
                    return true;
                }
            }

            // C# sources too, since the game loads the script assemblies the packager prebuilt.
            constexpr FStringView Suffixes[] = { ".cs", ".csproj", ".sln", ".user", ".DotSettings", "/.gitkeep", "/.gitignore", "/.hidden",
                                                 "/Config/EditorSession.json", "/Config/EditorProjectSettings.json" };
            for (const FStringView Suffix : Suffixes)
            {
                if (VirtualPath.EndsWith(Suffix))
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
                LogCooker(LogFunc, *Format("  [skip] missing: {}", VirtualPath));
                return false;
            }

            const TSpan<const uint8> Span(Bytes.GetData(), Bytes.Num());
            Writer.AddEntry(VirtualPath, Span);

            LogCooker(LogFunc, *Format("  + {} ({} bytes)",
                VirtualPath,
                Bytes.Num()));
            return true;
        }

        // A graph's nodes and pins are exports of their own, each an editor class, so they go by the same test.
        bool IsEditorOnlyObject(const CObject* Object)
        {
            const CClass* Class = Object->GetClass();
            return Class != nullptr && Class->GetPackage() != nullptr && IsEditorTypePackage(Class->GetPackage()->GetName());
        }

        // Every instance under a master, through instances of instances, since each may select its own permutation.
        void CollectMaterialInstances(const FGuid& Parent, TVector<FAssetData*>& OutInstances, THashSet<FGuid>& Visited)
        {
            for (FAssetData* Referencer : FAssetRegistry::Get().GetReferencersOf(Parent))
            {
                if (Referencer == nullptr || Referencer->AssetClass != CMaterialInstance::StaticClass()->GetName()
                    || !Visited.TryAdd(Referencer->AssetGUID))
                {
                    continue;
                }
                OutInstances.Add(Referencer);
                CollectMaterialInstances(Referencer->AssetGUID, OutInstances, Visited);
            }
        }

        // One bad asset falls back to a verbatim copy with a WARN rather than killing the whole cook.
        bool BundleAssetCooked(FPakWriter& Writer, FStringView VirtualPath, const TFunction<void(FStringView)>& LogFunc)
        {
            FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(VirtualPath);
            const bool bMaterial = Data != nullptr && Data->AssetClass == CMaterial::StaticClass()->GetName();
            // An emitter asset carries its own simulation shader, which systems sharing it reference rather than embed.
            const bool bParticleSystem = Data != nullptr && (Data->AssetClass == CParticleSystem::StaticClass()->GetName()
                                                          || Data->AssetClass == CParticleEmitter::StaticClass()->GetName());

            // Permutations are baked into the master at cook time, so its instances are inputs to its cooked bytes.
            TVector<FAssetData*> Instances;
            if (bMaterial)
            {
                THashSet<FGuid> Visited;
                CollectMaterialInstances(Data->AssetGUID, Instances, Visited);
            }

            // A material's cooked bytes carry shaders built from the templates, so a template edit must miss the cache too.
            uint64 SourceHash = Data ? Data->ContentHash ^ (bMaterial ? CMaterial::GetShaderTemplateHash() : 0) : 0;
            if (bParticleSystem)
            {
                SourceHash ^= CParticleEmitter::GetSimulationTemplateHash();
            }
            for (const FAssetData* Instance : Instances)
            {
                SourceHash = Hash::GetHash64(&Instance->ContentHash, sizeof(Instance->ContentHash), SourceHash);
            }
            const FCookInputHash Key = FCookDDC::ComputeKey(SourceHash);

            TVector<uint8> CookedBytes;
            if (FCookDDC::TryGet(Key, CookedBytes))
            {
                LogCooker(LogFunc, *Format("  + {} (ddc, {} bytes)",
                    VirtualPath,
                    CookedBytes.Num()));
                Writer.AddEntry(VirtualPath, Move(CookedBytes), true);
                return true;
            }

            CPackage* Package = CPackage::LoadPackage(VirtualPath);
            if (Package == nullptr)
            {
                LogCooker(LogFunc, *Format("  [warn] failed to load for cook, falling back to verbatim: {}",
                    VirtualPath));
                return BundleVfsFile(Writer, VirtualPath, LogFunc);
            }

            if (bMaterial)
            {
                CMaterial* Material = Cast<CMaterial>(Package->LoadObjectByName(Data->AssetName));
                const bool bStale = Material != nullptr && Material->CompiledTemplateHash != CMaterial::GetShaderTemplateHash();
                FString Error;
                if (!RecompileMaterialIfStale(Material, Error))
                {
                    LogCooker(LogFunc, *Format("  [warn] {} predates the current shader templates and could not be recompiled, since {}",
                        VirtualPath, Error));
                }
                else if (bStale)
                {
                    LogCooker(LogFunc, *Format("  recompiled {} against the current shader templates", VirtualPath));
                }

                // Editor assets carry no permutations, so each one an instance selects is pulled from the cache or compiled now.
                if (Material != nullptr && !Material->StaticSwitches.IsEmpty())
                {
                    TVector<uint64> Keys;
                    for (const FAssetData* InstanceData : Instances)
                    {
                        CPackage* InstancePackage = CPackage::LoadPackage(FStringView(*InstanceData->Path, InstanceData->Path.Len()));
                        const CMaterialInstance* Instance = InstancePackage != nullptr
                            ? Cast<CMaterialInstance>(InstancePackage->LoadObjectByName(InstanceData->AssetName)) : nullptr;
                        if (Instance != nullptr && Instance->GetMaterial() == Material && !Algo::Contains(Keys, Instance->GetStaticSwitchKey()))
                        {
                            Keys.Add(Instance->GetStaticSwitchKey());
                        }
                    }

                    if (const uint32 Failed = EnsureMaterialPermutations(Material, Keys); Failed != 0)
                    {
                        LogCooker(LogFunc, *Format("  [warn] {} of {}'s static switch permutations could not be built; those instances draw with default switches",
                            Failed, VirtualPath));
                    }
                }
            }

            if (bParticleSystem)
            {
                CObject* Loaded = Package->LoadObjectByName(Data->AssetName);
                CParticleEmitter* Standalone = Cast<CParticleEmitter>(Loaded);
                const TStrongObjectPtr<CParticleSystem> System = Standalone != nullptr ? ParticleOps::WrapEmitter(Standalone) : TStrongObjectPtr<CParticleSystem>(Cast<CParticleSystem>(Loaded));
                const int32 NumEmitters = System ? System->Emitters.Num() : 0;
                for (int32 Index = 0; Index < NumEmitters; ++Index)
                {
                    const CParticleEmitter* Emitter = System->Emitters[Index].Get();
                    if (Emitter == nullptr || Emitter->GetPackage() != Package || !Emitter->NeedsShaderCompile())
                    {
                        continue;
                    }

                    // The cache had nothing for the current templates, and a game cannot compile, so the cook builds it now.
                    FParticleEmitterCompileOutput Output;
                    if (ParticleOps::FindStack(System.Get(), Index) == nullptr || !ParticleOps::CompileEmitter(System.Get(), Index, Output))
                    {
                        LogCooker(LogFunc, *Format("  [warn] {} emitter {} has no simulation shader and could not be recompiled, so it will not simulate",
                            VirtualPath, Index));
                    }
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
                LogCooker(LogFunc, *Format("  [warn] cook-save failed, falling back to verbatim: {}",
                    VirtualPath));
                return BundleVfsFile(Writer, VirtualPath, LogFunc);
            }

            // Silent failure is fine, since the freshly-cooked bytes still ship and only the cache misses.
            FCookDDC::Put(Key, CookedBytes);

            LogCooker(LogFunc, *Format("  + {} (cooked, {} bytes, {} editor object(s) left out)",
                VirtualPath,
                CookedBytes.Num(),
                EditorObjects));
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
            if (auto It = Memo.Find(Struct); It != nullptr)
            {
                return (*It);
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
            if (auto It = ClassMemo.Find(Data->AssetClass); It != nullptr)
            {
                return (*It);
            }

            const CClass* Class = FindObject<CClass>(Data->AssetClass);

            // Materials rebuild stale shaders and strip SPIR-V, and textures drop their source, both only in a real save.
            const bool bEligible = Class != nullptr
                && !Class->IsChildOf(CMaterialInterface::StaticClass())
                && !Class->IsChildOf(CTexture::StaticClass())
                && !(Class->GetPackage() != nullptr && IsEditorTypePackage(Class->GetPackage()->GetName()))
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
            LogCooker(LogFunc, *Format("  [verify] {} {} ({} bytes from the file, {} from a load and save)",
                bMatch ? "matches" : "differs", Path, Stripped.Num(), Saved.Num()));

            // Both versions are kept so a difference can be decoded and compared outside the editor.
            if (!bMatch)
            {
                const FString Dir = FString(*Paths::GetEngineInstallDirectory()) + "/Intermediates/CookVerify/";
                Filesystem::MakeDirectoryTree(Dir);
                const FString Stem = Dir + FString(LeafName(Path).GetData(), LeafName(Path).Len());
                Filesystem::AtomicWriteFile(Stem + ".file", TSpan<const uint8>(Stripped.GetData(), Stripped.Num()));
                Filesystem::AtomicWriteFile(Stem + ".saved", TSpan<const uint8>(Saved.GetData(), Saved.Num()));
            }
        }

        // Most packages only lose their thumbnail, which needs no load, so they cook in parallel and only the rest are loaded and saved.
        void BundleChunkAssets(FPakWriter& Writer, const TVector<const FCookNode*>& Nodes, FCookResult& Result,
                               const TFunction<void(FStringView)>& LogFunc)
        {
            const double Start = PlatformTime::Seconds();

            THashMap<FName, bool> ClassMemo;
            THashMap<const CStruct*, bool> StructMemo;
            TVector<uint8> bTryStrip(Nodes.Num(), 0);
            for (int32 i = 0; i < Nodes.Num(); ++i)
            {
                const FStringView Path(*Nodes[i]->Path, Nodes[i]->Path.Len());
                bTryStrip[i] = CooksWithoutLoading(FAssetRegistry::Get().GetAssetByPath(Path), ClassMemo, StructMemo) ? 1 : 0;
            }

            TVector<TVector<uint8>> Stripped(Nodes.Num());
            TVector<uint8> bStripped(Nodes.Num(), 0);
            Jobs::ParallelFor((uint32)Nodes.Num(), [&](uint32 i)
            {
                if (bTryStrip[i] != 0)
                {
                    const FStringView Path(*Nodes[i]->Path, Nodes[i]->Path.Len());
                    bStripped[i] = CPackage::StripPackageForCook(Path, Stripped[i]) ? 1 : 0;
                }
            }, 16);

            // Added in graph order whichever path cooked them, so the pak stays deterministic.
            int32 NumStripped = 0;
            int32 NumLoaded = 0;
            THashMap<FName, int32> Verified;
            for (int32 i = 0; i < Nodes.Num(); ++i)
            {
                const FStringView Path(*Nodes[i]->Path, Nodes[i]->Path.Len());
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

            LogCooker(LogFunc, *Format("  {} asset(s) cooked from their files, {} loaded and saved, in {:.1f} s",
                NumStripped, NumLoaded, PlatformTime::Seconds() - Start));
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
                Text.Append(*Path, Path.Len());
                Text.AppendChar('\n');
            }

            // Kept with the project rather than in the package, so the shipped folder holds only what the game reads.
            const FString ContentsDir = FString(GEngine->GetProjectPath().GetData(), GEngine->GetProjectPath().Len()) + "/Saved/Cook";
            Filesystem::MakeDirectoryTree(FStringView(*ContentsDir, ContentsDir.Len()));
            const FString ContentsPath = ContentsDir + "/" + FString(LeafName(FStringView(*PakPath, PakPath.Len())).GetData(),
                                                                       LeafName(FStringView(*PakPath, PakPath.Len())).Len()) + ".contents.txt";
            if (Filesystem::AtomicWriteFile(ContentsPath, TSpan<const uint8>(reinterpret_cast<const uint8*>(Text.GetData()), Text.Len())))
            {
                LogCooker(LogFunc, *Format("  listed {} entries in {}", Paths.Num(), *ContentsPath));
            }
        }

        // Picks up loose files loaded by name at runtime, which carry no asset-reflected references.
        int32 BundleLooseContent(FPakWriter& Writer, const TFunction<void(FStringView)>& LogFunc)
        {
            int32 Count = 0;
            auto Walk = [&](FStringView Root)
            {
                VFS::RecursiveDirectoryIterator(Root, [&](const VFS::FFileInfo& Info)
                {
                    if (Info.IsDirectory()) return;
                    FStringView Vp(*Info.VirtualPath, Info.VirtualPath.Len());
                    if (EndsWithIgnoreCase(Vp, ".lasset")) return;
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
                LogCooker(LogFunc, *Format("  [skip] unreadable extra: {}",
                    DiskPath));
                return false;
            }
            Writer.AddEntry(VirtualPath, TSpan<const uint8>(Bytes.GetData(), Bytes.Num()));
            LogCooker(LogFunc, *Format("  + {} ({} bytes, extra)",
                VirtualPath, Bytes.Num()));
            return true;
        }

        int32 BundleExtras(FPakWriter& Writer, const FCookOptions& Options, const TFunction<void(FStringView)>& LogFunc)
        {
            int32 Count = 0;

            for (const FString& File : Options.ExtraFiles)
            {
                if (!Filesystem::IsFile(File))
                {
                    LogCooker(LogFunc, *Format("  [skip] extra file not found: {}", *File));
                    continue;
                }

                const FStringView Leaf = LeafName(File);

                FString Vp = "/Extras/";
                Vp.Append(Leaf.GetData(), Leaf.Len());

                if (BundleDiskFile(Writer, File, FStringView(*Vp, Vp.Len()), LogFunc))
                {
                    ++Count;
                }
            }

            for (const FString& Dir : Options.ExtraDirectories)
            {
                if (!Filesystem::IsDirectory(Dir))
                {
                    LogCooker(LogFunc, *Format("  [skip] extra dir not found: {}", *Dir));
                    continue;
                }

                const FStringView RootName = LeafName(Dir);
                const int32 RootLength = Dir.Len();

                Filesystem::IterateDirectoryRecursive(Dir, [&](const Filesystem::FDirectoryEntry& Entry)
                {
                    if (Entry.IsDirectory())
                    {
                        return;
                    }

                    const FStringView Relative = Entry.FullPath.Mid(RootLength);

                    FString Vp = "/Extras/";
                    Vp.Append(RootName.GetData(), RootName.Len());
                    Vp.Append(Relative.GetData(), Relative.Len());

                    if (BundleDiskFile(Writer, Entry.FullPath, FStringView(*Vp, Vp.Len()), LogFunc))
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

            FJsonValue Doc;
            if (!FJsonValue::Parse(JsonText.View(), Doc) || !Doc.IsObject())
            {
                Doc = FJsonValue::MakeObject();
            }

            if (!Doc["Project"].IsObject())
            {
                Doc.Set("Project", FJsonValue::MakeObject());
            }
            Doc.FindOrAdd("Project").Set("Name", FStringView(GEngine->GetProjectName().GetData(), GEngine->GetProjectName().Len()));

            const FString Out = Doc.ToString(4);
            Writer.AddEntry("/Config/GameSettings.json", Out.View());
            LogCooker(LogFunc, *Format("  + /Config/GameSettings.json (cooked, {} bytes)", Out.Len()));
            return true;
        }

        // Bundles the engine content a runtime needs, under /Engine/Resources.
        int32 BundleEngineResources(FPakWriter& Writer, const TFunction<void(FStringView)>& LogFunc)
        {
            int32 Count = 0;
            VFS::RecursiveDirectoryIterator("/Engine/Resources", [&](const VFS::FFileInfo& Info)
            {
                if (Info.IsDirectory())
                {
                    return;
                }
                
                FStringView Vp(*Info.VirtualPath, Info.VirtualPath.Len());

                if (BundleVfsFile(Writer, Vp, LogFunc))
                {
                    ++Count;
                }
            });
            return Count;
        }

        // Bundle every cached SPIR-V (.lsc) under /Intermediates/ShaderCache.
        int32 BundleShaderCache(FPakWriter& Writer, const TFunction<void(FStringView)>& LogFunc)
        {
            int32 Count = 0;
            VFS::RecursiveDirectoryIterator("/Intermediates/ShaderCache", [&](const VFS::FFileInfo& Info)
            {
                if (Info.IsDirectory() || Info.GetExt() != ".lsc")
                {
                    return;
                }

                const FStringView VirtualPath(*Info.VirtualPath, Info.VirtualPath.Len());
                TVector<uint8> Bytes;
                if (!VFS::ReadFile(Bytes, VirtualPath))
                {
                    return;
                }

                // The editor compiles with full debug info for Nsight, which nothing in a shipped game reads.
                TVector<uint8> Stripped;
                const bool bStripped = FShaderCache::StripCacheFileForCook(Bytes, Stripped);
                const TVector<uint8>& Shipped = bStripped ? Stripped : Bytes;

                Writer.AddEntry(VirtualPath, TSpan<const uint8>(Shipped.GetData(), Shipped.Num()));
                LogCooker(LogFunc, *Format("  + {} ({} bytes{})", VirtualPath, Shipped.Num(),
                    bStripped ? Format(", {} before stripping", Bytes.Num()) : FString()));
                ++Count;
            });
            return Count;
        }

        // Bundle other /Config/*.json files; GameSettings.json is handled by BundleConfigWithProjectName.
        int32 BundleAuxConfigFiles(FPakWriter& Writer, const TFunction<void(FStringView)>& LogFunc)
        {
            int32 Count = 0;
            VFS::DirectoryIterator("/Config", [&](const VFS::FFileInfo& Info)
            {
                if (Info.IsDirectory() || Info.GetExt() != ".json")
                {
                    return;
                }
                FStringView Vp(*Info.VirtualPath, Info.VirtualPath.Len());
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

        if (GEngine == nullptr || GEngine->GetProjectName().IsEmpty())
        {
            Result.ErrorMessage = "No project loaded.";
            return Result;
        }

        const TVector<FCookRoot> Roots = GEngine->GetCookRoots();
        if (Roots.IsEmpty())
        {
            Result.ErrorMessage =
                "No cook roots defined.\n"
                "  Open Project Settings -> Maps -> Cook Roots and add at least one asset path,\n"
                "  declare CookRoots in a plugin's .lplugin, or put assets in a REFLECT(Group = ...) class.";
            return Result;
        }

        LogCooker(LogFunc, *Format("Building cook graph from {} root(s), shader templates {:016x}...", Roots.Num(),
            CMaterial::GetShaderTemplateHash()));

        FCookGraph Graph(FAssetRegistry::Get());
        Graph.AddRoots(Roots);

        // FindByPredicate returns hash order, so sort by GUID before adding to keep the graph deterministic.
        {
            TVector<FAssetData*> Grouped = FAssetRegistry::Get().FindByPredicate(
                [](const FAssetData& D) { return !FAssetManager::Get().GetGroupOfClass(D.AssetClass).IsNone(); });
            Algo::Sort(Grouped,
                [](const FAssetData* A, const FAssetData* B) { return A->AssetGUID < B->AssetGUID; });
            if (!Grouped.IsEmpty())
            {
                LogCooker(LogFunc, *Format("  Grouped assets: {} -> implicit cook roots", Grouped.Num()));
            }
            for (const FAssetData* Data : Grouped)
            {
                FCookRoot Root;
                Root.Asset = FString(*Data->Path, Data->Path.Len());
                Root.Chunk = kMainChunkName;
                Graph.AddRoot(Root);
            }
        }

        // The project root subdirs plus every enabled plugin's mount.
        TVector<FString> ContentRoots;
        // All of /Game, since scripts keep the JSON they read beside them or in a data folder outside Content.
        ContentRoots.Emplace("/Game");
        for (const FPlugin* Plugin : FPluginManager::Get().GetAllPlugins())
        {
            if (!Plugin->IsEnabled())        continue;
            if (!Plugin->IsContentMounted()) continue;
            ContentRoots.Emplace(Plugin->GetMountAlias());
        }

        // VFS walk order is OS-dependent, so sort resolved paths before adding to keep the cook reproducible.
        {
            FRmlUiAssetScan::FResult UiScan = FRmlUiAssetScan::ScanRoots(
                ContentRoots, FAssetRegistry::Get(), LogFunc);
            Algo::Sort(UiScan.AssetPaths);
            if (UiScan.FilesScanned > 0)
            {
                LogCooker(LogFunc, *Format("  UI scan: {} file(s), {} candidate ref(s), {} resolved -> implicit cook roots",
                    UiScan.FilesScanned, UiScan.RawCandidates, UiScan.ResolvedRefs));
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
                LogCooker(LogFunc, *Format("  Script scan: {} file(s), {} asset ref(s), {} folder ref(s) -> implicit cook roots",
                    ScriptScan.FilesScanned, ScriptScan.AssetPaths.Num(), ScriptScan.FolderPaths.Num()));
            }
            for (const FString& Path : ScriptScan.AssetPaths)
            {
                Graph.AddRoot(FCookRoot{ Path, kMainChunkName });
            }
            for (const FString& Folder : ScriptScan.FolderPaths)
            {
                Graph.AddFolderRoot(FStringView(*Folder, Folder.Len()), kMainChunkName);
            }
        }

        Graph.Traverse();

        for (const FCookGraphIssue& Issue : Graph.GetIssues())
        {
            LogCooker(LogFunc, *Format("  [warn] {}: {}",
                *Issue.Source, *Issue.Detail));
        }

        const auto Reachable = Graph.GetReachableNodesSorted();
        LogCooker(LogFunc, *Format("Reachable assets: {}", Reachable.Num()));

        // Sorted-by-GUID input order is preserved per chunk so PAK entry order stays deterministic.
        THashMap<FName, TVector<const FCookNode*>> ByChunk;
        for (const FCookNode* Node : Reachable)
        {
            const FName Chunk = Node->Chunk.IsNone() ? kMainChunkName : Node->Chunk;
            ByChunk[Chunk].Add(Node);
        }

        // Alphabetical with Main forced first, since Main carries shared content and the verbatim path.
        TVector<FName> ChunkOrder;
        ByChunk.GenerateKeyArray(ChunkOrder);
        if (ByChunk.TryAdd(kMainChunkName))      // ensure a Main PAK exists for shared content
        {
            ChunkOrder.Add(kMainChunkName);
        }
        Algo::Sort(ChunkOrder,
            [&](const FName& A, const FName& B)
            {
                if (A == kMainChunkName) return true;
                if (B == kMainChunkName) return false;
                return A.ToString() < B.ToString();
            });

        // Main keeps the caller's name verbatim, while others become stem, chunk and extension.
        auto ChunkPakPath = [&](FName Chunk) -> FString
        {
            const FString FullPath(OutputPakPath.GetData(), OutputPakPath.Len());
            if (Chunk == kMainChunkName) return FullPath;

            const int32 DotPos = FullPath.FindLastOf('.');
            const int32 SlashPos = FullPath.FindLastOf("/\\");
            const bool bExtAfterSlash = DotPos != Constants::kIndexNone
                && (SlashPos == Constants::kIndexNone || DotPos > SlashPos);

            const FString Stem = bExtAfterSlash ? FullPath.Mid(0, DotPos) : FullPath;
            const FString Ext  = bExtAfterSlash ? FullPath.Mid(DotPos)    : FString(".pak");
            FString Out = Stem;
            Out += "-";
            Out += *Chunk.ToString();
            Out += Ext;
            return Out;
        };

        for (const FName& Chunk : ChunkOrder)
        {
            FPakWriter Writer;
            const bool bIsMain = (Chunk == kMainChunkName);

            BundleChunkAssets(Writer, ByChunk[Chunk], Result, LogFunc);

            int32 ChunkExtras = 0;
            if (bIsMain)
            {
                if (BundleConfigWithProjectName(Writer, LogFunc)) { ++ChunkExtras; }
                ChunkExtras += BundleAuxConfigFiles(Writer, LogFunc);

                LogCooker(LogFunc, "Bundling engine resources...");
                const int32 NumEngine = BundleEngineResources(Writer, LogFunc);
                ChunkExtras += NumEngine;
                LogCooker(LogFunc, *Format("  bundled {} engine files", NumEngine));

                LogCooker(LogFunc, "Bundling shader cache...");
                const int32 NumShaders = BundleShaderCache(Writer, LogFunc);
                ChunkExtras += NumShaders;
                LogCooker(LogFunc, *Format("  bundled {} cached shaders", NumShaders));

                if (!Options.bExtractScriptsAsLooseFiles)
                {
                    ChunkExtras += BundleLooseContent(Writer, LogFunc);
                }
                else
                {
                    LogCooker(LogFunc, "Skipping loose content in PAK (loose mode).");
                }

                if (!Options.ExtraFiles.IsEmpty() || !Options.ExtraDirectories.IsEmpty())
                {
                    LogCooker(LogFunc, "Bundling extras...");
                    ChunkExtras += BundleExtras(Writer, Options, LogFunc);
                }

                {
                    const FString Stamp = Format("{:016x}", CMaterial::GetShaderTemplateHash());
                    if (Writer.AddEntry(CMaterial::CookedShaderTemplateHashPath,
                        TSpan<const uint8>(reinterpret_cast<const uint8*>(Stamp.GetData()), Stamp.Len())))
                    {
                        ++ChunkExtras;
                        LogCooker(LogFunc, *Format("  + {} ({})", CMaterial::CookedShaderTemplateHashPath, Stamp));
                    }
                }

                // A near-empty registry usually means stale editor discovery, so warn loudly in the cook log.
                {
                    const int32 LiveCount = FAssetRegistry::Get().GetAssets().Num();
                    TVector<uint8> Bytes;
                    FMemoryWriter Ar(Bytes);
                    FAssetRegistry::Get().WriteToArchive(Ar);
                    if (Writer.AddEntry("/Engine/AssetRegistry.bin",
                        TSpan<const uint8>(Bytes.GetData(), Bytes.Num())))
                    {
                        ++ChunkExtras;
                        LogCooker(LogFunc, *Format("  + /Engine/AssetRegistry.bin (cooked, {} bytes, {} live entries)",
                            Bytes.Num(), LiveCount));
                    }
                    // Only warn at zero (fresh projects have few assets); zero means discovery never ran or wiped the registry, fix by deleting the .json cache + restart.
                    if (LiveCount == 0)
                    {
                        LogCooker(LogFunc, *Format("  [warn] live registry has 0 entries, Shipping runtime will not find anything. "
                            "Delete <EngineInstall>/Intermediates/AssetRegistry.json and restart the editor "
                            "to force a fresh discovery, then re-cook."));
                    }
                }
            }

            const FString OutPath = ChunkPakPath(Chunk);
            const int64 ChunkBytes = Writer.TotalEntryBytes();
            Filesystem::MakeParentDirectoryTree(FStringView(*OutPath, OutPath.Len()));
            if (!Writer.Finalize(OutPath))
            {
                Result.ErrorMessage = FString("Failed to write PAK at ") + OutPath;
                return Result;
            }
            WritePakContents(Writer, OutPath, LogFunc);

            FCookChunkResult ChunkResult;
            ChunkResult.Chunk     = Chunk;
            ChunkResult.PakPath   = OutPath;
            ChunkResult.NumAssets = ByChunk[Chunk].Num();
            ChunkResult.NumExtras = ChunkExtras;
            ChunkResult.Bytes     = ChunkBytes;
            Result.Chunks.Add(Move(ChunkResult));

            Result.NumExtraFiles += ChunkExtras;
            Result.TotalBytes    += ChunkBytes;

            LogCooker(LogFunc, *Format("Wrote chunk '{}' -> {} ({} entries, {} bytes)",
                *Chunk.ToString(), *OutPath,
                Writer.NumEntries(), ChunkBytes));
        }

        LogCooker(LogFunc, *Format("DDC: {} hits, {} misses ({} bytes written this cook)",
            FCookDDC::Hits(), FCookDDC::Misses(), FCookDDC::WrittenBytes()));

        Result.bSuccess = true;
        return Result;
    }
}
