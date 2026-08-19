#include "LayoutRegistry.h"
#include "DotNetHost.h"
#include "ManagedRenderScene.h"

#include "Platform/Filesystem/PlatformFilesystem.h"
#include <fstream>
#include <iterator>
#include <string>
#include <cstring>
#include <cstdio>

#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Delegates/ScriptDelegate.h"
#include "Core/Engine/Engine.h"
#include "Core/Object/ManagedInstance.h"
#include "Scripting/ScriptStruct.h"
#include "Scripting/ScriptableObject.h"
#include "Scripting/ScriptDataStruct.h"
#include "Core/Plugin/Plugin.h"
#include "Core/Plugin/PluginManager.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"
#include "Paths/Paths.h"
#include "Platform/Process/PlatformProcess.h"
#include "World/World.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/Component.h"
#include "Scripting/DotNet/DotNetExport.h"
#include "Scripting/ScriptExports.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/SoftObjectPtr.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "TaskSystem/ThreadedCallback.h"
#include "Tools/UI/ImGui/ImGuiX.h"   // editor toast notifications for script-compile feedback
#include "nlohmann/json.hpp"          // cooked script manifest (prebuilt-DLL unit graph)

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <tlhelp32.h>
#else
    #include <dlfcn.h>
    #include <link.h>
    #include <cstring>
#endif

#include "coreclr_delegates.h"
#include "hostfxr.h"

namespace Lumina
{
    // Defined in CSharpLayoutChecks.cpp: validates the FString/TVector byte layout that
    // LuminaSharp.NativeMarshal reads in place (Dev/Debug self-test; a no-op in Shipping).
    void VerifyContainerInteropLayout();
}

namespace Lumina::DotNet
{
    namespace
    {
    #if defined(_WIN32)
        #define LSTR(s) L##s
        constexpr const char* kSharedExt = ".dll";
        constexpr const char* kSharedPrefix = "";
    #elif defined(__APPLE__)
        #define LSTR(s) s
        constexpr const char* kSharedExt = ".dylib";
        constexpr const char* kSharedPrefix = "lib";
    #else
        #define LSTR(s) s
        constexpr const char* kSharedExt = ".so";
        constexpr const char* kSharedPrefix = "lib";
    #endif

        // Defined further down; the project-generation helpers above it normalize paths through it.
        FString NativePath(FStringView P);

        FString Join(FStringView Left, FStringView Right)
        {
            FString Result(Left.data(), Left.size());
            if (!Result.empty() && Result.back() != '/' && Result.back() != '\\')
            {
                Result.push_back('/');
            }
            Result.append(Right.data(), Right.size());
            return Result;
        }

        FString ParentOf(FStringView Path)
        {
            const size_t Slash = Path.find_last_of("/\\");
            return Slash == FStringView::npos ? FString() : FString(Path.data(), Slash);
        }

        // hostfxr takes wchar_t paths on Windows and char elsewhere, so they cross the boundary here.
        class FHostString
        {
        public:

            explicit FHostString(FStringView Utf8)
            {
            #if defined(_WIN32)
                Storage = StringUtils::ToWideString(Utf8);
            #else
                Storage.assign(Utf8.data(), Utf8.size());
            #endif
            }

            const char_t* Get() const { return Storage.c_str(); }

        private:

            #if defined(_WIN32)
                FWString Storage;
            #else
                FString Storage;
            #endif
        };

        // Native<->managed boundary. The managed Host mirrors this layout exactly;
        // any change here bumps GAbiVersion and the matching managed constant.
        struct FExporterTable
        {
            void (CORECLR_DELEGATE_CALLTYPE* Log)(int32 Level, const char* Utf8, int32 Len);
        };

        struct FBootstrapArgs
        {
            int32                   AbiVersion;
            const FExporterTable*   Exports;
            void*                   NativeModule;   // Runtime.dll handle; managed resolves "LuminaNative" to it
        };

        // One script source handed to managed. Mirrors LuminaSharp.SourceFile (natural x64 layout).
        struct FSourceFile
        {
            const char* Path;
            int32       PathLen;
            const char* Text;
            int32       TextLen;
        };

        // One compilation unit handed to managed (a plugin, the game, or the engine library). Mirrors
        // LuminaSharp.FSourceAssembly. Sources points at SourceCount FSourceFile; Deps is a ';'-joined list
        // of sibling unit names this one references; DllPath (optional) is an absolute prebuilt managed
        // assembly used when SourceCount == 0. All pointers must outlive the LoadScripts call.
        struct FSourceAssembly
        {
            const char*        Name;
            int32              NameLen;
            const char*        Deps;
            int32              DepsLen;
            const FSourceFile* Sources;
            int32              SourceCount;
            const char*        DllPath;
            int32              DllPathLen;
        };

        typedef int32 (CORECLR_DELEGATE_CALLTYPE* BootstrapFn)(const FBootstrapArgs*);
        // Resolves a native->managed export by name to its function pointer (engine or script/plugin), or null.
        // Native resolves THIS entry by name via hostfxr, then uses it to look up every other managed entry.
        typedef void* (CORECLR_DELEGATE_CALLTYPE* ResolveManagedExportFn)(const char*, int32);
        typedef int32 (CORECLR_DELEGATE_CALLTYPE* LoadScriptsFn)(const FSourceAssembly*, int32);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* TickFn)();
        typedef void  (CORECLR_DELEGATE_CALLTYPE* ShutdownFn)();
        typedef int32 (CORECLR_DELEGATE_CALLTYPE* GetGenerationFn)();
        typedef int32 (CORECLR_DELEGATE_CALLTYPE* GetRuntimeDiagnosticsFn)(void*, int32);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* EnumerateEntityScriptsFn)(void*, void*);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* EnumerateEntitySystemsFn)(void*, void*);
        typedef void* (CORECLR_DELEGATE_CALLTYPE* CreateScriptableFn)(const char*, int32, uint64);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* EnumerateScriptablesFn)(void*, void*);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* EnumerateScriptableAliasesFn)(void*, void*);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* ApplyScriptableDefaultsFn)(const char*, int32, uint64);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* EnumerateScriptStructsFn)(void*, void*);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* GetScriptStructSchemaFn)(const char*, int32, void*, void*);
        typedef void* (CORECLR_DELEGATE_CALLTYPE* CreateEntitySystemFn)(const char*, int32, uint64);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* TickEntitySystemFn)(void*, void*);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* DestroyEntitySystemFn)(void*);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* EnumerateRenderScenesFn)(void*, void*);
        typedef void* (CORECLR_DELEGATE_CALLTYPE* CreateRenderSceneFn)(const char*, int32, uint64);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* DestroyRenderSceneFn)(void*);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* RenderSceneExtractFn)(void*, const void*);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* RenderSceneRenderFn)(void*, int32);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* RenderSceneResizeFn)(void*, uint32, uint32);
        typedef uint64 (CORECLR_DELEGATE_CALLTYPE* RenderSceneGetDisplayTextureFn)(void*);
        typedef uint32 (CORECLR_DELEGATE_CALLTYPE* RenderSceneGetDisplayResourceIDFn)(void*);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* RenderSceneGetExtentFn)(void*, uint32*, uint32*);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* OnNativeDelegateDestroyedFn)(void*);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* GetScriptSchemaFn)(const char*, int32, void*, void*);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* GetScriptButtonsFn)(const char*, int32, void*, void*);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* ResolveEntityScriptNameFn)(const char*, int32, void*, void*);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* InvokeAssetCallbackFn)(void*, void*);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* ManagedFreeHandleFn)(void*);
        typedef int32 (CORECLR_DELEGATE_CALLTYPE* InvokeScriptButtonFn)(void*, const char*, int32);

        // A LOCAL typed cache of the engine's managed entries (call sites stay typed). NOT an ABI mirror: native
        // fills each field at bootstrap by resolving it by name via ResolveManagedExport, so field set/order is
        // a local concern only, no C# struct to match, no drift hash. A missing entry fails loudly per-name.
        struct FManagedExports
        {
            CreateEntitySystemFn        CreateEntitySystem;
            DestroyEntitySystemFn       DestroyEntitySystem;
            OnNativeDelegateDestroyedFn OnNativeDelegateDestroyed;
            EnumerateEntityScriptsFn    EnumerateEntityScripts;
            EnumerateEntitySystemsFn    EnumerateEntitySystems;
            CreateScriptableFn          CreateScriptable;
            EnumerateScriptablesFn      EnumerateScriptables;
            EnumerateScriptableAliasesFn EnumerateScriptableAliases;
            ApplyScriptableDefaultsFn   ApplyScriptableDefaults;
            EnumerateScriptStructsFn    EnumerateScriptStructs;
            GetScriptStructSchemaFn     GetScriptStructSchema;
            ManagedFreeHandleFn         FreeHandle;
            InvokeScriptButtonFn        InvokeScriptButton;
            GetGenerationFn             GetGeneration;
            GetRuntimeDiagnosticsFn     GetRuntimeDiagnostics;
            GetScriptSchemaFn           GetScriptSchema;
            GetScriptButtonsFn          GetScriptButtons;
            ResolveEntityScriptNameFn   ResolveEntityScriptName;
            InvokeAssetCallbackFn       InvokeAssetCallback;
            LoadScriptsFn               LoadScripts;
            ShutdownFn                  Shutdown;
            TickFn                      Tick;
            TickEntitySystemFn          TickEntitySystem;

            EnumerateRenderScenesFn             EnumerateRenderScenes;
            CreateRenderSceneFn                 CreateRenderScene;
            DestroyRenderSceneFn                DestroyRenderScene;
            RenderSceneExtractFn                RenderSceneExtract;
            RenderSceneRenderFn                 RenderSceneRender;
            RenderSceneResizeFn                 RenderSceneResize;
            RenderSceneGetDisplayTextureFn      RenderSceneGetDisplayTexture;
            RenderSceneGetDisplayResourceIDFn   RenderSceneGetDisplayResourceID;
            RenderSceneGetExtentFn              RenderSceneGetExtent;
        };

        bool                                        bInitialized = false;
        hostfxr_get_runtime_delegate_fn             GGetDelegate = nullptr;
        FExporterTable                              GExports{};
        int32                                       GCachedGeneration = 0;
        ResolveManagedExportFn                      GResolveManagedExport = nullptr;
        FManagedExports                             GManaged{};

        // Reflection layouts for each C# script type, cleared on reload/shutdown.
        Scripting::FScriptStructRegistry            GScriptStructs;

        // On native delegate destruction, ask the managed registry to free the matching GCHandles.
        void NotifyManagedDelegateDestroyed(void* DelegateAddress)
        {
            if (bInitialized && GManaged.OnNativeDelegateDestroyed != nullptr)
            {
                GManaged.OnNativeDelegateDestroyed(DelegateAddress);
            }
        }

        // Sink the managed EnumerateEntityScripts calls once per script type; Ctx is the out vector.
        void LmScriptNameSink(void* Ctx, const char* Name, int Len)
        {
            auto* Out = static_cast<TVector<FString>*>(Ctx);
            if (Out != nullptr && Name != nullptr && Len > 0)
            {
                Out->emplace_back(FString(Name, static_cast<size_t>(Len)));
            }
        }

        // Sink one (prior name, current name) pair; Ctx is the out pair vector.
        void LmScriptableAliasSink(void* Ctx, const char* Old, int OldLen, const char* New, int NewLen)
        {
            auto* Out = static_cast<TVector<FScriptableAlias>*>(Ctx);
            if (Out == nullptr || Old == nullptr || OldLen <= 0 || New == nullptr || NewLen <= 0)
            {
                return;
            }
            FScriptableAlias Alias;
            Alias.OldName = FString(Old, static_cast<size_t>(OldLen));
            Alias.NewName = FString(New, static_cast<size_t>(NewLen));
            Out->emplace_back(std::move(Alias));
        }

        // Sink the managed EnumerateScriptables calls once per Scriptable C# type; Ctx is the out desc vector.
        void LmScriptableSink(void* Ctx, const char* Name, int NameLen, const char* Base, int BaseLen, uint64 OverrideFlags)
        {
            auto* Out = static_cast<TVector<FScriptableTypeDesc>*>(Ctx);
            if (Out == nullptr || Name == nullptr || NameLen <= 0)
            {
                return;
            }
            FScriptableTypeDesc Desc;
            Desc.TypeName = FString(Name, static_cast<size_t>(NameLen));
            if (Base != nullptr && BaseLen > 0)
            {
                Desc.NativeBaseName = FString(Base, static_cast<size_t>(BaseLen));
            }
            Desc.OverrideFlags = OverrideFlags;
            Out->emplace_back(std::move(Desc));
        }

        // Sink the managed EnumerateScriptStructs calls once per marked C# data type; Ctx is the out desc vector.
        void LmScriptStructSink(void* Ctx, const char* Name, int NameLen, const char* Base, int BaseLen)
        {
            auto* Out = static_cast<TVector<FScriptStructTypeDesc>*>(Ctx);
            if (Out == nullptr || Name == nullptr || NameLen <= 0)
            {
                return;
            }
            FScriptStructTypeDesc Desc;
            Desc.ScriptTypeName = FString(Name, static_cast<size_t>(NameLen));
            if (Base != nullptr && BaseLen > 0)
            {
                Desc.NativeBaseName = FString(Base, static_cast<size_t>(BaseLen));
            }
            Out->emplace_back(std::move(Desc));
        }

        // Single-name sink for ResolveEntityScriptName; Ctx is the out FString.
        void LmSingleNameSink(void* Ctx, const char* Name, int Len)
        {
            auto* Out = static_cast<FString*>(Ctx);
            if (Out != nullptr && Name != nullptr && Len > 0)
            {
                Out->assign(Name, static_cast<size_t>(Len));
            }
        }

        // Reads the entt::type_hash ids off an array of FComponentOps* access tokens into Out.
        void LmCollectAccessIds(const void* const* Tokens, int Count, TVector<uint32>& Out)
        {
            if (Tokens == nullptr || Count <= 0)
            {
                return;
            }
            Out.reserve(static_cast<size_t>(Count));
            for (int i = 0; i < Count; ++i)
            {
                if (const FComponentOps* Ops = static_cast<const FComponentOps*>(Tokens[i]))
                {
                    Out.push_back(static_cast<uint32>(Ops->TypeId));
                }
            }
        }

        // Sink the managed EnumerateEntitySystems calls once per system type; Ctx is the out vector. The
        // write/read tokens are FComponentOps* the system declared via [Writes]/[Reads]; their TypeId is the
        // entt::type_hash used to build the FSystemAccess (no tokens => exclusive).
        void LmSystemDescSink(void* Ctx, const char* Name, int Len, int Stage, int Priority,
            const void* const* WriteTokens, int NWrite, const void* const* ReadTokens, int NRead)
        {
            auto* Out = static_cast<TVector<FManagedSystemDesc>*>(Ctx);
            if (Out != nullptr && Name != nullptr && Len > 0)
            {
                FManagedSystemDesc Desc;
                Desc.TypeName.assign(Name, static_cast<size_t>(Len));
                Desc.Stage = (Stage >= 0 && Stage < (int)EUpdateStage::Max) ? (EUpdateStage)Stage : EUpdateStage::PrePhysics;
                Desc.Priority = Priority;
                LmCollectAccessIds(WriteTokens, NWrite, Desc.Writes);
                LmCollectAccessIds(ReadTokens, NRead, Desc.Reads);
                Out->push_back(std::move(Desc));
            }
        }

        // Keeps each source file's text alive while it's marshaled to managed.
        struct FGatheredSource
        {
            FString Path;
            FString Text;
        };

        // Gathers every .cs under an absolute DISK directory (recursively), skipping obj/bin. Scripts are
        // source, not VFS content, so this is decoupled from plugin content-mounts: a code-only plugin
        // (bContainsContent=false) still gets its <PluginDir>/Scripts compiled. Paths are absolute (good for
        // compiler diagnostics + the debugger).
        void GatherSourcesUnder(const FString& DiskDir, TVector<FGatheredSource>& Out)
        {
            if (DiskDir.empty())
            {
                return;
            }

            if (!Filesystem::IsDirectory(DiskDir))
            {
                return;
            }

            Filesystem::IterateDirectoryRecursive(DiskDir, [&Out](const Filesystem::FDirectoryEntry& Entry)
            {
                if (Entry.IsDirectory() || Entry.GetExtension() != FStringView(".cs"))
                {
                    return;
                }

                const FStringView P = Entry.FullPath;
                if (P.find("/obj/") != FStringView::npos || P.find("/bin/") != FStringView::npos)
                {
                    return;
                }

                FGatheredSource Src;
                if (!Filesystem::ReadFile(Src.Text, P))
                {
                    return;
                }

                Src.Path.assign(P.data(), P.size());
                Out.push_back(std::move(Src));
            });
        }
        
        // One compilation unit of a script generation: the game, an enabled plugin, or the engine library.
        // The SINGLE source of truth for the dependency graph -- both the runtime compile (ReloadScripts) and
        // the IDE project generation (GenerateScriptProjects) consume the same units, so the IntelliSense view
        // can never disagree with what actually compiles.
        struct FScriptUnit
        {
            FString          Name;            // assembly label (becomes the managed assembly name)
            FString          DiskDir;         // absolute Scripts dir on disk (source root + .csproj location)
            FString          BinaryDir;       // <root>/Binaries/DotNet  (where this unit's compiled DLL is emitted)
            FString          IntermediateDir; // <root>/Intermediates/DotNet/<Name>  (IDE obj dir)
            FString          AssemblyPath;    // <BinaryDir>/<Name>.dll (emit target when sources exist; load source otherwise)
            TVector<FString> Deps;            // sibling unit names this one references
        };

        // Enumerate the script units: every enabled plugin (deps = its .lplugin dependencies), the game (an
        // implicit dependency on every enabled plugin), and the engine library (standalone base). Each unit
        // compiles into its OWN DLL under its OWN <root>/Binaries/DotNet, the game in the project, a plugin in
        // its plugin dir (alongside its C++ Binaries), the engine next to the engine binaries. Nothing is
        // bundled into LuminaSharp, and a disabled plugin is filtered out here so it produces no unit at all.
        TVector<FScriptUnit> BuildScriptUnits()
        {
            TVector<FScriptUnit> Units;
            TVector<FString>     PluginNames;

            for (const FPlugin* Plugin : FPluginManager::Get().GetAllPlugins())
            {
                if (Plugin == nullptr || !Plugin->IsEnabled())
                {
                    continue;
                }

                FScriptUnit Unit;
                Unit.Name.assign(Plugin->GetName().data(), Plugin->GetName().size());
                const FString PluginDir(Plugin->GetDirectory().data(), Plugin->GetDirectory().size());
                Unit.DiskDir         = PluginDir + "/Scripts";
                Unit.BinaryDir       = PluginDir + "/Binaries/DotNet";
                Unit.IntermediateDir = PluginDir + "/Intermediates/DotNet/" + Unit.Name;
                Unit.AssemblyPath    = Unit.BinaryDir + "/" + Unit.Name + ".dll";
                for (const FPluginDependency& Dep : Plugin->GetDescriptor().Dependencies)
                {
                    Unit.Deps.push_back(Dep.Name);
                }

                PluginNames.push_back(Unit.Name);
                Units.push_back(std::move(Unit));
            }

            {
                FScriptUnit Game;
                Game.Name = "Game";
                if (GEngine != nullptr)
                {
                    const FFixedString ScriptsDir = GEngine->GetProjectScriptsDirectory();
                    Game.DiskDir.assign(ScriptsDir.data(), ScriptsDir.size());

                    const FStringView ProjectPath = GEngine->GetProjectPath();
                    if (!ProjectPath.empty())
                    {
                        const FString Root(ProjectPath.data(), ProjectPath.size());
                        Game.BinaryDir       = Root + "/Binaries/DotNet";
                        Game.IntermediateDir = Root + "/Intermediates/DotNet/Game";
                        Game.AssemblyPath    = Game.BinaryDir + "/Game.dll";
                    }
                }
                Game.Deps = PluginNames;
                Units.push_back(std::move(Game));
            }

            {
                FScriptUnit Engine;
                Engine.Name = "Engine";
                Engine.DiskDir = Paths::GetEngineResourceDirectory() + "/Scripts";
                // Engine example scripts live next to the engine binaries (a sibling of DotNet/Managed, not in it).
                const FString ExeDir = ParentOf(Platform::GetCurrentProcessPath());
                Engine.BinaryDir       = NativePath(Join(ExeDir, "DotNet"));
                Engine.IntermediateDir = NativePath(Join(ExeDir, "DotNet/obj/Engine"));
                Engine.AssemblyPath    = Engine.BinaryDir + "/Engine.dll";
                Units.push_back(std::move(Engine));
            }

            return Units;
        }

        // Cooked-game unit graph. The packager stages each script unit's prebuilt DLL under
        // <exeDir>/DotNet/Scripts/ together with scripts.manifest.json carrying [{Name, Deps, Dll}], so the
        // managed load order (topo-sort over Deps) and cross-assembly references survive without re-running
        // plugin discovery. Units carry NO sources (DiskDir empty) -> the shared load core takes the
        // prebuilt-DLL branch. Returns empty when no manifest is present (dev runs, or a project with no C#).
        TVector<FScriptUnit> BuildCookedScriptUnits()
        {
            TVector<FScriptUnit> Units;

            const FString ExeDir       = ParentOf(Platform::GetCurrentProcessPath());
            const FString ScriptsDir   = Join(ExeDir, "DotNet/Scripts");
            const FString ManifestPath = Join(ScriptsDir, "scripts.manifest.json");

            FString Text;
            if (!Filesystem::ReadFile(Text, ManifestPath))
            {
                return Units;
            }

            nlohmann::json J;
            try
            {
                J = nlohmann::json::parse(Text.c_str());
            }
            catch (const std::exception& Err)
            {
                LOG_ERROR("C#: failed to parse cooked script manifest '{}': {}", ManifestPath, Err.what());
                return Units;
            }

            const nlohmann::json& Arr = J.contains("Units") ? J["Units"] : J;
            if (!Arr.is_array())
            {
                return Units;
            }

            for (const nlohmann::json& Entry : Arr)
            {
                if (!Entry.is_object())
                {
                    continue;
                }
                const std::string Name = Entry.value("Name", std::string());
                if (Name.empty())
                {
                    continue;
                }

                FScriptUnit Unit;
                Unit.Name = FString(Name.c_str());
                const std::string Dll = Entry.value("Dll", Name + ".dll");
                Unit.AssemblyPath = Join(ScriptsDir, FStringView(Dll.c_str(), Dll.size()));

                if (Entry.contains("Deps") && Entry["Deps"].is_array())
                {
                    for (const nlohmann::json& Dep : Entry["Deps"])
                    {
                        if (Dep.is_string())
                        {
                            Unit.Deps.push_back(FString(Dep.get<std::string>().c_str()));
                        }
                    }
                }
                Units.push_back(std::move(Unit));
            }
            return Units;
        }

        // Writes Content to Path only when it differs from what is on disk (so frequent reloads don't churn
        // the files and the IDE doesn't reload projects on every hot-reload). Creates parent dirs as needed.
        void WriteTextIfChanged(FStringView Path, const std::string& Content)
        {
            FString Existing;
            if (Filesystem::ReadFile(Existing, Path)
                && Existing.size() == Content.size()
                && ::memcmp(Existing.data(), Content.data(), Content.size()) == 0)
            {
                return;
            }

            const TSpan<const uint8> Bytes(reinterpret_cast<const uint8*>(Content.data()), Content.size());
            if (Filesystem::WriteFile(Path, Bytes))
            {
                LOG_DISPLAY("Generated C# script project: {}", Path);
            }
        }

        // Sidecar stamped next to a unit's DLL when WE compiled it from sources. It is what tells a stale
        // emitted artifact (sources since deleted -> delete the DLL, never load it) apart from an authored
        // prebuilt assembly at the same canonical path (a code-only plugin -> load it).
        FString CompiledMarkerPath(const FString& DllPath)
        {
            return DllPath + ".compiled";
        }

        // Deterministic GUID from a seed string (FNV-1a x2 -> 16 bytes), so a unit's .sln project GUID is
        // stable across regenerations and the IDE doesn't treat each regen as a brand-new project.
        std::string MakeStableGuid(const FString& Seed)
        {
            auto Fnv = [](const char* S, uint64 Basis) -> uint64
            {
                uint64 H = Basis;
                for (; *S != '\0'; ++S)
                {
                    H = (H ^ static_cast<uint64>(static_cast<uint8>(*S))) * 1099511628211ULL;
                }
                return H;
            };

            const uint64 A = Fnv(Seed.c_str(), 14695981039346656037ULL);
            const uint64 B = Fnv(Seed.c_str(), 14695981039346656037ULL ^ 0x9E3779B97F4A7C15ULL);
            uint8 Bytes[16];
            for (int Index = 0; Index < 8; ++Index)
            {
                Bytes[Index]     = static_cast<uint8>(A >> (Index * 8));
                Bytes[8 + Index] = static_cast<uint8>(B >> (Index * 8));
            }

            char Buf[40];
            std::snprintf(Buf, sizeof(Buf),
                "{%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                Bytes[0], Bytes[1], Bytes[2], Bytes[3], Bytes[4], Bytes[5], Bytes[6], Bytes[7],
                Bytes[8], Bytes[9], Bytes[10], Bytes[11], Bytes[12], Bytes[13], Bytes[14], Bytes[15]);
            return std::string(Buf);
        }

        // The absolute path of the generated IDE project for a unit ("<DiskDir>/<Name>.Scripts.csproj").
        FString UnitProjectPath(const FScriptUnit& Unit)
        {
            return NativePath(Join(Unit.DiskDir, Unit.Name + ".Scripts.csproj"));
        }

        // The SDK-style .csproj XML for a unit: references the engine LuminaSharp.dll and ProjectReferences
        // every dependency unit that has a generated project on disk -- so editing this unit's scripts gives
        // IntelliSense for the plugins it builds on, matching the runtime cross-assembly references exactly.
        std::string BuildCsprojXml(const FScriptUnit& Unit, const TVector<FScriptUnit>& AllUnits, const FString& LuminaSharpDll)
        {
            std::string Xml;
            Xml += "<Project Sdk=\"Microsoft.NET.Sdk\">\n";
            Xml += "  <!-- GENERATED for IDE IntelliSense only (run \"dotnet.genprojects\" to refresh).\n";
            Xml += "       Scripts are compiled at runtime by the engine; this project is never the runtime\n";
            Xml += "       path and is overwritten on reload. -->\n";
            Xml += "  <PropertyGroup>\n";
            Xml += "    <TargetFramework>net10.0</TargetFramework>\n";
            Xml += "    <Nullable>enable</Nullable>\n";
            Xml += "    <ImplicitUsings>disable</ImplicitUsings>\n";
            Xml += "    <EnableDefaultItems>true</EnableDefaultItems>\n";
            // Generated reflected-type bindings read properties through raw pointers.
            Xml += "    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>\n";
            Xml += "    <AssemblyName>" + std::string(Unit.Name.c_str()) + "</AssemblyName>\n";
            // Build output goes to this unit's OWN Binaries/Intermediates (matching the engine's runtime emit
            // location), so an IDE build produces the same artifact and nothing lands in LuminaSharp.
            if (!Unit.BinaryDir.empty())
            {
                Xml += "    <AppendTargetFrameworkToOutputPath>false</AppendTargetFrameworkToOutputPath>\n";
                Xml += "    <OutputPath>" + std::string(Unit.BinaryDir.c_str()) + "/</OutputPath>\n";
            }
            if (!Unit.IntermediateDir.empty())
            {
                Xml += "    <IntermediateOutputPath>" + std::string(Unit.IntermediateDir.c_str()) + "/</IntermediateOutputPath>\n";
            }
            Xml += "  </PropertyGroup>\n";
            Xml += "  <ItemGroup>\n";
            Xml += "    <Reference Include=\"LuminaSharp\">\n";
            Xml += "      <HintPath>" + std::string(LuminaSharpDll.c_str()) + "</HintPath>\n";
            Xml += "    </Reference>\n";
            // The generator the runtime compile loads; without it partial NativeCall bodies are missing.
            Xml += "    <Analyzer Include=\""
                + std::string(NativePath(Join(ParentOf(LuminaSharpDll), "LuminaSharp.Generators.dll")).c_str())
                + "\" />\n";
            Xml += "  </ItemGroup>\n";

            std::string Refs;
            for (const FString& DepName : Unit.Deps)
            {
                const FScriptUnit* Dep = nullptr;
                for (const FScriptUnit& Candidate : AllUnits)
                {
                    if (Candidate.Name == DepName)
                    {
                        Dep = &Candidate;
                        break;
                    }
                }
                if (Dep == nullptr || Dep->DiskDir.empty() || !Filesystem::Exists(Dep->DiskDir))
                {
                    continue; // the dependency ships no scripts on disk; nothing to reference
                }
                Refs += "    <ProjectReference Include=\"" + std::string(UnitProjectPath(*Dep).c_str()) + "\" />\n";
            }
            if (!Refs.empty())
            {
                Xml += "  <ItemGroup>\n" + Refs + "  </ItemGroup>\n";
            }

            Xml += "</Project>\n";
            return Xml;
        }

        // Writes a solution tying every generated script project together, so opening it in an IDE gives one
        // unified, cross-plugin IntelliSense view. Placed at the project root; skipped if no project is loaded.
        void WriteScriptSolution(const TVector<FScriptUnit>& Units)
        {
            if (GEngine == nullptr || GEngine->GetProjectPath().empty())
            {
                return;
            }

            struct FSlnEntry { std::string Name; std::string Path; std::string Guid; };
            TVector<FSlnEntry> Entries;
            for (const FScriptUnit& Unit : Units)
            {
                if (Unit.DiskDir.empty())
                {
                    continue;
                }
                if (!Filesystem::Exists(Unit.DiskDir))
                {
                    continue;
                }
                Entries.push_back({ std::string(Unit.Name.c_str()) + ".Scripts", std::string(UnitProjectPath(Unit).c_str()), MakeStableGuid(Unit.Name) });
            }
            if (Entries.empty())
            {
                return;
            }

            const char* CsProjTypeGuid = "{FAE04EC0-301F-11D3-BF4B-00C04F79EFBC}";
            std::string Sln;
            Sln += "\xEF\xBB\xBF";
            Sln += "Microsoft Visual Studio Solution File, Format Version 12.00\n";
            Sln += "# Visual Studio Version 17\n";
            for (const FSlnEntry& Entry : Entries)
            {
                Sln += "Project(\"" + std::string(CsProjTypeGuid) + "\") = \"" + Entry.Name + "\", \"" + Entry.Path + "\", \"" + Entry.Guid + "\"\n";
                Sln += "EndProject\n";
            }
            Sln += "Global\n";
            Sln += "\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\n";
            Sln += "\t\tDebug|Any CPU = Debug|Any CPU\n";
            Sln += "\t\tRelease|Any CPU = Release|Any CPU\n";
            Sln += "\tEndGlobalSection\n";
            Sln += "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\n";
            for (const FSlnEntry& Entry : Entries)
            {
                Sln += "\t\t" + Entry.Guid + ".Debug|Any CPU.ActiveCfg = Debug|Any CPU\n";
                Sln += "\t\t" + Entry.Guid + ".Release|Any CPU.ActiveCfg = Release|Any CPU\n";
            }
            Sln += "\tEndGlobalSection\n";
            Sln += "EndGlobal\n";

            FString ProjectName(GEngine->GetProjectName().data(), GEngine->GetProjectName().size());
            if (ProjectName.empty())
            {
                ProjectName = "Game";
            }
            const FString SlnPath = NativePath(Join(GEngine->GetProjectPath(), ProjectName + ".GameScripts.sln"));
            WriteTextIfChanged(SlnPath, Sln);
        }

        // Native function the managed side calls to write into the engine log.
        void CORECLR_DELEGATE_CALLTYPE Export_Log(int32 Level, const char* Utf8, int32 Len)
        {
            const FString Msg(Utf8, (size_t)Len);
            switch (Level)
            {
            case 0:  LOG_TRACE("[C#] {}", Msg.c_str()); break;
            case 2:  LOG_WARN ("[C#] {}", Msg.c_str()); break;
            case 3:  LOG_ERROR("[C#] {}", Msg.c_str()); break;
            default: LOG_INFO ("[C#] {}", Msg.c_str()); break;
            }
        }
        
        FString NativePath(FStringView P)
        {
            FString Result(P.data(), P.size());
        #if defined(_WIN32)
            std::replace(Result.begin(), Result.end(), '/', '\\');
        #endif
            return Result;
        }

        const char* RuntimeRid()
        {
        #if defined(_WIN32)
            return "win-x64";
        #elif defined(__APPLE__)
            return "osx-x64";
        #else
            return "linux-x64";
        #endif
        }

        void* LoadShared(FStringView Path)
        {
            const FHostString Native(Path);
        #if defined(_WIN32)
            return (void*)::LoadLibraryW(Native.Get());
        #else
            return ::dlopen(Native.Get(), RTLD_LAZY | RTLD_LOCAL);
        #endif
        }

        void* GetSym(void* Lib, const char* Name)
        {
        #if defined(_WIN32)
            return (void*)::GetProcAddress((HMODULE)Lib, Name);
        #else
            return ::dlsym(Lib, Name);
        #endif
        }

        // Handle of the module this code lives in (Runtime.dll), where the generated property thunks
        // are exported. Managed maps the "LuminaNative" P/Invoke library name to it.
        void* GetThunkModuleHandle()
        {
        #if defined(_WIN32)
            HMODULE Module = nullptr;
            ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&Export_Log), &Module);
            return (void*)Module;
        #else
            // RTLD_NOLOAD: this image is already mapped, so the handle is a reference to it rather
            // than a second load.
            Dl_info Info{};
            if (::dladdr(reinterpret_cast<const void*>(&Export_Log), &Info) != 0 && Info.dli_fname != nullptr)
            {
                return ::dlopen(Info.dli_fname, RTLD_LAZY | RTLD_NOLOAD);
            }
            return nullptr;
        #endif
        }

        // Best-effort recursive copy (overwriting). Returns false if any file failed to copy (e.g. a
        // destination locked by another process). Used to shadow the managed dir so the build output stays free.
        bool CopyTreeBestEffort(FStringView Src, FStringView Dst)
        {
            return Filesystem::CopyTree(Src, Dst);
        }

        // Frees one cached-wrapper GC handle on Core's behalf. Core owns the per-CObject managed-instance
        // cache but knows nothing about the managed runtime, so it calls back through here. Null-safe against
        // a teardown that has already cleared the managed export table.
        void FreeManagedInstanceHandle(void* Handle)
        {
            if (Handle != nullptr && GManaged.FreeHandle != nullptr)
            {
                GManaged.FreeHandle(Handle);
            }
        }
    }

    void Initialize()
    {
        if (bInitialized)
        {
            return;
        }

        // A packaged game ships the runtime next to the exe (<exeDir>/External/DotNet/runtime/<rid>) and has
        // no LUMINA_DIR / on-disk Engine/Resources, so GetEngineInstallDirectory() is empty there. Probe the
        // exe-relative layout first, then fall back to the engine install dir for the in-tree dev build.
        const FString ExeDir = ParentOf(Platform::GetCurrentProcessPath());

        FString RuntimeSubPath = "External/DotNet/runtime/";
        RuntimeSubPath.append(RuntimeRid());

        FString Bundled = Join(ExeDir, RuntimeSubPath);
        if (!Filesystem::Exists(Bundled))
        {
            Bundled = Join(Paths::GetEngineInstallDirectory(), RuntimeSubPath);
        }
        if (!Filesystem::Exists(Bundled))
        {
            LOG_ERROR("C# scripting disabled: bundled .NET runtime not found next to the exe or under the engine install ('{}'). Run Setup.bat to extract External.", Bundled);
            return;
        }
        Bundled = NativePath(Bundled);

        // Locate host/fxr/<version>/hostfxr.<ext> under the bundled root.
        const FString FxrRoot  = Join(Bundled, "host/fxr");
        const FString FxrName  = FString(kSharedPrefix) + "hostfxr" + kSharedExt;

        FString HostfxrPath;
        Filesystem::IterateDirectory(FxrRoot, [&HostfxrPath, &FxrName](const Filesystem::FDirectoryEntry& Entry) -> Filesystem::EVisit
        {
            if (!Entry.IsDirectory())
            {
                return Filesystem::EVisit::Continue;
            }

            FString Candidate = Join(Entry.FullPath, FxrName);
            if (!Filesystem::Exists(Candidate))
            {
                return Filesystem::EVisit::Continue;
            }

            HostfxrPath = NativePath(Candidate);
            return Filesystem::EVisit::Stop;
        });

        if (HostfxrPath.empty())
        {
            LOG_ERROR("C# scripting disabled: hostfxr not found under '{}'.", FxrRoot);
            return;
        }

        void* Lib = LoadShared(HostfxrPath);
        if (!Lib)
        {
            LOG_ERROR("C# scripting disabled: failed to load hostfxr at '{}'.", HostfxrPath);
            return;
        }

        auto Init  = (hostfxr_initialize_for_runtime_config_fn)GetSym(Lib, "hostfxr_initialize_for_runtime_config");
        GGetDelegate = (hostfxr_get_runtime_delegate_fn)       GetSym(Lib, "hostfxr_get_runtime_delegate");
        auto Close = (hostfxr_close_fn)                        GetSym(Lib, "hostfxr_close");
        if (!Init || !GGetDelegate || !Close)
        {
            LOG_ERROR("C# scripting disabled: hostfxr is missing expected exports.");
            return;
        }

        // Managed bootstrap, built next to the binaries by the LuminaSharp project.
        const FString ExePath    = NativePath(Platform::GetCurrentProcessPath());
        const FString ManagedDir = Join(ExeDir, "DotNet/Managed");

        // In the editor, CoreCLR keeps LuminaSharp.dll (and the Roslyn assemblies the script compiler pulls
        // from the same folder) open for the whole session. That locks the build output, so a packaging
        // MSBuild can't overwrite it ("LuminaSharp.dll is in use"). Load from a shadow copy instead, leaving
        // the canonical DotNet/Managed output free to rebuild. A cooked game has no build step (and may sit in
        // a read-only dir), so it loads in place. Multi-instance: if the shadow is locked by another editor,
        // the copy fails and we fall back to loading in place.
        FString LoadDir = ManagedDir;
    #if WITH_EDITOR
        {
            const FString ShadowDir = Join(ExeDir, "DotNet/ManagedShadow");
            if (CopyTreeBestEffort(ManagedDir, ShadowDir)
                && Filesystem::Exists(Join(ShadowDir, "LuminaSharp.dll"))
                && Filesystem::Exists(Join(ShadowDir, "LuminaSharp.runtimeconfig.json")))
            {
                LoadDir = ShadowDir;
            }
            else
            {
                LOG_WARN("C#: managed shadow copy incomplete; loading in place (packaging may report LuminaSharp.dll in use).");
            }
        }
    #endif

        const FString BootstrapDll = NativePath(Join(LoadDir, "LuminaSharp.dll"));
        const FString BootstrapCfg = NativePath(Join(LoadDir, "LuminaSharp.runtimeconfig.json"));
        if (!Filesystem::Exists(BootstrapDll) || !Filesystem::Exists(BootstrapCfg))
        {
            LOG_ERROR("C# scripting disabled: managed bootstrap missing under '{}'. Did LuminaSharp build?", LoadDir);
            return;
        }

        const FHostString HostExePath(ExePath);
        const FHostString HostDotNetRoot(Bundled);
        const FHostString HostBootstrapCfg(BootstrapCfg);

        hostfxr_initialize_parameters Params{};
        Params.size = sizeof(Params);
        Params.host_path = HostExePath.Get();
        Params.dotnet_root = HostDotNetRoot.Get();

        hostfxr_handle Ctx = nullptr;
        int rc = Init(HostBootstrapCfg.Get(), &Params, &Ctx);
        if (rc != 0 || Ctx == nullptr)
        {
            LOG_ERROR("C# scripting disabled: hostfxr_initialize_for_runtime_config failed (0x{:x}).", (uint32)rc);
            if (Ctx)
            {
                Close(Ctx);
            }
            return;
        }

        void* LoadAsmFn = nullptr;
        rc = GGetDelegate(Ctx, hdt_load_assembly_and_get_function_pointer, &LoadAsmFn);
        Close(Ctx);
        if (rc != 0 || LoadAsmFn == nullptr)
        {
            LOG_ERROR("C# scripting disabled: get_runtime_delegate failed (0x{:x}).", (uint32)rc);
            return;
        }

        auto LoadAssembly = (load_assembly_and_get_function_pointer_fn)LoadAsmFn;

        const auto* HostType = LSTR("LuminaSharp.Host, LuminaSharp");
        const FHostString HostBootstrapDll(BootstrapDll);

        BootstrapFn Bootstrap = nullptr;
        rc = LoadAssembly(HostBootstrapDll.Get(), HostType, LSTR("Bootstrap"), UNMANAGEDCALLERSONLY_METHOD, nullptr, (void**)&Bootstrap);
        if (rc != 0 || Bootstrap == nullptr)
        {
            LOG_ERROR("C# scripting disabled: failed to resolve managed Bootstrap entry (0x{:x}).", (uint32)rc);
            return;
        }

        // Bootstrap registers every engine [ManagedExport] into the managed ManagedExportRegistry (by name);
        // it no longer hands native a function-pointer table.
        GExports.Log = &Export_Log;

        FBootstrapArgs Args{};
        Args.AbiVersion = GAbiVersion;
        Args.Exports = &GExports;
        Args.NativeModule = GetThunkModuleHandle();

        const int32 Result = Bootstrap(&Args);
        if (Result != 0)
        {
            LOG_ERROR("C# scripting disabled: managed bootstrap handshake failed (returned {}). ABI mismatch?", Result);
            return;
        }

        // C# now reads reflected FString/TVector properties in place by hard-coding the container layout
        // (LuminaSharp.NativeMarshal). Validate that layout against the real accessors once (Dev/Debug).
        VerifyContainerInteropLayout();

        // Resolve the managed export resolver by name (like Bootstrap itself), then fill the engine entry cache
        // through it. There is no hand-mirrored table: each field is resolved by its own name, and a missing
        // entry simply leaves that field null (the mandatory ones are checked below).
        rc = LoadAssembly(HostBootstrapDll.Get(), HostType, LSTR("ResolveManagedExport"), UNMANAGEDCALLERSONLY_METHOD, nullptr, (void**)&GResolveManagedExport);
        if (rc != 0 || GResolveManagedExport == nullptr)
        {
            LOG_ERROR("C# scripting disabled: failed to resolve managed ResolveManagedExport entry (0x{:x}).", (uint32)rc);
            return;
        }

        #define LM_RESOLVE(Field, Type) GManaged.Field = (Type)GResolveManagedExport(#Field, (int32)std::strlen(#Field))
        LM_RESOLVE(CreateEntitySystem,     CreateEntitySystemFn);
        LM_RESOLVE(DestroyEntitySystem,    DestroyEntitySystemFn);
        LM_RESOLVE(EnumerateEntityScripts, EnumerateEntityScriptsFn);
        LM_RESOLVE(EnumerateEntitySystems, EnumerateEntitySystemsFn);
        LM_RESOLVE(CreateScriptable,       CreateScriptableFn);      // optional: only when scripts ship Scriptables
        LM_RESOLVE(EnumerateScriptables,   EnumerateScriptablesFn);
        LM_RESOLVE(EnumerateScriptableAliases, EnumerateScriptableAliasesFn);   // optional: only when a script class declares [Alias]
        LM_RESOLVE(ApplyScriptableDefaults, ApplyScriptableDefaultsFn);   // optional: only when a script declares a [Property] initializer
        LM_RESOLVE(EnumerateScriptStructs, EnumerateScriptStructsFn);   // optional: only when scripts ship data types
        LM_RESOLVE(GetScriptStructSchema,  GetScriptStructSchemaFn);
        LM_RESOLVE(FreeHandle,             ManagedFreeHandleFn);
        LM_RESOLVE(InvokeScriptButton,     InvokeScriptButtonFn);   // optional: editor [Button] support
        LM_RESOLVE(GetGeneration,          GetGenerationFn);
        LM_RESOLVE(GetRuntimeDiagnostics,  GetRuntimeDiagnosticsFn);
        LM_RESOLVE(GetScriptSchema,        GetScriptSchemaFn);
        LM_RESOLVE(GetScriptButtons,       GetScriptButtonsFn);
        LM_RESOLVE(ResolveEntityScriptName, ResolveEntityScriptNameFn);
        LM_RESOLVE(InvokeAssetCallback,    InvokeAssetCallbackFn);
        LM_RESOLVE(LoadScripts,            LoadScriptsFn);
        LM_RESOLVE(OnNativeDelegateDestroyed, OnNativeDelegateDestroyedFn);
        LM_RESOLVE(Shutdown,               ShutdownFn);
        LM_RESOLVE(Tick,                   TickFn);
        LM_RESOLVE(TickEntitySystem,       TickEntitySystemFn);
        LM_RESOLVE(EnumerateRenderScenes,  EnumerateRenderScenesFn);
        LM_RESOLVE(CreateRenderScene,      CreateRenderSceneFn);
        LM_RESOLVE(DestroyRenderScene,     DestroyRenderSceneFn);
        LM_RESOLVE(RenderSceneExtract,     RenderSceneExtractFn);
        LM_RESOLVE(RenderSceneRender,      RenderSceneRenderFn);
        LM_RESOLVE(RenderSceneResize,      RenderSceneResizeFn);
        LM_RESOLVE(RenderSceneGetDisplayTexture,    RenderSceneGetDisplayTextureFn);
        LM_RESOLVE(RenderSceneGetDisplayResourceID, RenderSceneGetDisplayResourceIDFn);
        LM_RESOLVE(RenderSceneGetExtent,   RenderSceneGetExtentFn);
        #undef LM_RESOLVE

        // The core script entries are mandatory. Entity-script creation and ticking are NOT among them any
        // more: scripts are CEntityScript CObjects driven by the native SEntityScriptSystem, so the managed
        // side no longer owns their lifecycle.
        if (GManaged.LoadScripts == nullptr || GManaged.Tick == nullptr || GManaged.Shutdown == nullptr ||
            GManaged.GetGeneration == nullptr)
        {
            LOG_ERROR("C# scripting disabled: managed export resolution missing core script entry points.");
            return;
        }

        bInitialized = true;
        GOnScriptDelegateDestroyed = &NotifyManagedDelegateDestroyed;

        // Core owns the per-CObject managed-instance cache but must not know about GC handles; hand it the
        // free function now that the managed side can service one.
        Lumina::ManagedInstances::SetFreeHandleFn(&FreeManagedInstanceHandle);

        LOG_DISPLAY(".NET host initialized (bundled runtime: {}).", Bundled);
    }

    void Shutdown()
    {
        if (!bInitialized)
        {
            return;
        }

        // Before the managed side goes away: drop every cached C# wrapper while the handles can still be
        // freed. After this the table is empty and SetFreeHandleFn(nullptr) makes any later Set a no-op free.
        Lumina::ManagedInstances::ReleaseAll();

        if (GManaged.Shutdown)
        {
            GManaged.Shutdown();
        }

        Lumina::ManagedInstances::SetFreeHandleFn(nullptr);

        bInitialized = false;
        GOnScriptDelegateDestroyed = nullptr;
        GExports = FExporterTable{};
        GCachedGeneration = 0;
        GScriptStructs.Clear();
        GManaged = FManagedExports{};   // clears the whole native->managed table in one go
        LOG_DISPLAY(".NET host shut down.");
    }

    void Tick()
    {
        if (!bInitialized || GManaged.Tick == nullptr)
        {
            return;
        }
        GManaged.Tick();
    }

    // Core load path shared by the editor (ReloadScripts, compile-from-source) and the cooked game
    // (LoadCookedScripts, prebuilt DLLs). Turns a prepared unit list into managed assemblies and refreshes
    // the native mirrors. bEditorFollowups drives the editor-only steps (toast notifications + .csproj regen)
    // that a cooked game has no use for. Returns the managed load result (0 == ok, <0 = could not run).
    int32 LoadScriptUnitsCore(const TVector<FScriptUnit>& UnitList, bool bEditorFollowups)
    {
        if (!bInitialized || GManaged.LoadScripts == nullptr)
        {
            return -1;
        }

        // Turn the shared unit graph into compilation buckets: gather each unit's .cs, and skip units with
        // neither sources nor a prebuilt DLL. Each surviving bucket becomes its own assembly in the managed
        // collectible ALC, emitted to its own <root>/Binaries/DotNet/<Name>.dll. DllPath is that canonical
        // location: the emit target when the unit has sources, or the load source when it ships a prebuilt DLL.
        struct FSourceBucket
        {
            FString                  Name;
            FString                  Deps;     // ';'-joined sibling unit names
            TVector<FGatheredSource> Sources;  // owns the path/text strings the marshaled views point at
            FString                  DllPath;  // canonical on-disk DLL: emit target (sources) or load source (prebuilt)
        };

        TVector<FSourceBucket> Buckets;
        for (const FScriptUnit& Unit : UnitList)
        {
            FSourceBucket Bucket;
            Bucket.Name    = Unit.Name;
            Bucket.DllPath = Unit.AssemblyPath;

            GatherSourcesUnder(Unit.DiskDir, Bucket.Sources);

            // A source-rooted unit (editor units carry a DiskDir; cooked ones don't) whose sources are all
            // gone must NOT fall back to the DLL we emitted from them earlier: loading it would resurrect
            // every deleted class. The compile marker identifies that DLL as ours; delete both and move on.
            if (Bucket.Sources.empty() && !Unit.DiskDir.empty() && !Unit.AssemblyPath.empty())
            {
                std::error_code Ec;
                const FString Marker = CompiledMarkerPath(Unit.AssemblyPath);
                if (Filesystem::Exists(Marker))
                {
                    Filesystem::RemoveFile(Unit.AssemblyPath);
                    Filesystem::RemoveFile(Marker);
                    LOG_DISPLAY("C#: unit '{}' no longer has sources; deleted its stale compiled assembly.", Unit.Name.c_str());
                    continue;
                }
            }

            // With no .cs, the unit can still load a prebuilt managed DLL sitting at its canonical path (a
            // code-only plugin that ships a compiled assembly). With neither, there is nothing to do.
            const bool bHasPrebuilt = Bucket.Sources.empty() && !Unit.AssemblyPath.empty()
                && Filesystem::Exists(Unit.AssemblyPath);
            if (Bucket.Sources.empty() && !bHasPrebuilt)
            {
                continue;
            }

            for (size_t Index = 0; Index < Unit.Deps.size(); ++Index)
            {
                if (Index != 0)
                {
                    Bucket.Deps += ";";
                }
                Bucket.Deps += Unit.Deps[Index];
            }

            Buckets.push_back(std::move(Bucket));
        }

        // Marshal. Each bucket's FSourceFile array must outlive the call, so keep one per bucket alive here.
        TVector<TVector<FSourceFile>> PerBucketFiles;
        PerBucketFiles.resize(Buckets.size());
        TVector<FSourceAssembly>      Units;
        Units.reserve(Buckets.size());
        size_t TotalFiles = 0;

        for (size_t Index = 0; Index < Buckets.size(); ++Index)
        {
            FSourceBucket&         Bucket = Buckets[Index];
            TVector<FSourceFile>&  Files  = PerBucketFiles[Index];
            Files.reserve(Bucket.Sources.size());
            for (const FGatheredSource& S : Bucket.Sources)
            {
                FSourceFile File;
                File.Path    = S.Path.c_str();
                File.PathLen = (int32)S.Path.size();
                File.Text    = S.Text.c_str();
                File.TextLen = (int32)S.Text.size();
                Files.push_back(File);
            }

            FSourceAssembly Unit;
            Unit.Name        = Bucket.Name.c_str();
            Unit.NameLen     = (int32)Bucket.Name.size();
            Unit.Deps        = Bucket.Deps.c_str();
            Unit.DepsLen     = (int32)Bucket.Deps.size();
            Unit.Sources     = Files.empty() ? nullptr : Files.data();
            Unit.SourceCount = (int32)Files.size();
            Unit.DllPath     = Bucket.DllPath.c_str();
            Unit.DllPathLen  = (int32)Bucket.DllPath.size();
            Units.push_back(Unit);
            TotalFiles += Files.size();
        }

        LOG_DISPLAY("C#: {} {} script unit(s), {} file(s)...",
            bEditorFollowups ? "compiling" : "loading", Units.size(), TotalFiles);

        // Managed render scenes hold GCHandles into the generation about to unload; tear their worlds'
        // renderers down first (waits for the GPU) so nothing dispatches into a dead ALC.
        ManagedRenderScenes::PreScriptUnload();

        const int32 Result = GManaged.LoadScripts(Units.empty() ? nullptr : Units.data(), (int32)Units.size());
        // The compile runs synchronously on this (main) thread, so a live progress modal can't animate during
        // it; instead report the outcome as a toast (covers every reload trigger: hot-key, content-browser
        // save-watch, console command, and the initial project-load compile). A cooked game has no editor UI.
        if (Result != 0)
        {
            LOG_ERROR("C# script load/reload returned error {}.", Result);
            if (bEditorFollowups)
            {
                ImGuiX::Notifications::NotifyError("Script compile failed ({} file(s)) -- see the Output Log.", TotalFiles);
            }
        }
        else if (bEditorFollowups)
        {
            ImGuiX::Notifications::NotifySuccess("Recompiled {} C# script file(s).", TotalFiles);
        }

        if (Result == 0)
        {
            // Stamp each source-compiled unit's emitted DLL so a later reload can tell it apart from an
            // authored prebuilt assembly (and delete it once the unit's sources are gone, see above).
            for (const FSourceBucket& Bucket : Buckets)
            {
                if (!Bucket.Sources.empty() && !Bucket.DllPath.empty())
                {
                    const FString Marker = CompiledMarkerPath(Bucket.DllPath);
                    if (!Filesystem::Exists(Marker))
                    {
                        static constexpr const char Text[] =
                            "Compiled from this unit's C# sources by the engine. Both this marker and the DLL are deleted when the sources are removed.\n";
                        Filesystem::WriteFile(Marker, TSpan<const uint8>(reinterpret_cast<const uint8*>(Text), sizeof(Text) - 1));
                    }
                }
            }
        }

        // Refresh the native generation mirror once here (the only place it can change), so the
        // per-frame tick never crosses the boundary to read it.
        GCachedGeneration = GManaged.GetGeneration ? GManaged.GetGeneration() : GCachedGeneration;

        // Drop every cached C# wrapper for this generation. The handles are WEAK, so they never pinned the
        // ALC that just unloaded and this is not what makes hot reload work -- it exists so the table does not
        // carry handles whose target died with the old ALC, and so its slots are recycled instead of growing
        // once per reload. Objects are untouched; the next access re-creates the wrapper against the new
        // generation's types.
        Lumina::ManagedInstances::ReleaseAll();

        GScriptStructs.Clear();

        // Mint a real CClass for every C# subclass of a REFLECT(Scriptable) native class, so FindObject<CClass>
        // / NewObject / editor pickers see them like any native class (minted classes are reused by name across
        // reloads; the managed instance behind each rebinds via the per-instance FScriptableBridge generation gate).
        FScriptableRegistry::RefreshMintedClasses();

        // Mint a CScriptStruct for every C# type carrying a data marker, deriving from the native base the
        // marker names, so blackboards and data tables see script-declared shapes exactly as native ones.
        // Ordered after the CClass minting for the same reason: both read the generation that just loaded.
        FScriptDataStructRegistry::Get().Refresh();

        // Re-sync the renderer override against the new generation's RenderScene types and give the worlds
        // PreScriptUnload tore down a renderer again. Runs even on a failed load so those worlds fall back
        // to the engine renderer instead of staying black.
        ManagedRenderScenes::PostScriptLoad();

        // Keep the IDE projects in lockstep with the scripts that just (re)loaded so an absent or deleted
        // .csproj self-heals on ANY reload, not only on a full project load (idempotent; no-op if unchanged).
        // Editor-only: a cooked game ships no sources and no IDE.
        if (bEditorFollowups)
        {
            GenerateScriptProjects();
        }
        return Result;
    }

    void ReloadScripts()
    {
        LoadScriptUnitsCore(BuildScriptUnits(), /*bEditorFollowups*/true);
    }

    void LoadCookedScripts()
    {
        if (!bInitialized || GManaged.LoadScripts == nullptr)
        {
            return;
        }

        const TVector<FScriptUnit> Units = BuildCookedScriptUnits();
        if (Units.empty())
        {
            LOG_DISPLAY("C#: no cooked script units to load (no DotNet/Scripts/scripts.manifest.json or it was empty).");
            return;
        }
        LoadScriptUnitsCore(Units, /*bEditorFollowups*/false);
    }

    void GatherScriptUnitsForPackaging(TVector<FPackagedScriptUnit>& Out)
    {
        Out.clear();
        if (!bInitialized)
        {
            return;
        }

        // Recompile first so each unit's <root>/Binaries/DotNet/<Name>.dll is emitted fresh (captures the
        // latest .cs edits); the cooked game then loads these prebuilt rather than running Roslyn at boot.
        ReloadScripts();

        for (const FScriptUnit& Unit : BuildScriptUnits())
        {
            if (Unit.AssemblyPath.empty())
            {
                continue; // no canonical DLL location (e.g. a unit with no project path) -- nothing to ship
            }
            FPackagedScriptUnit Packaged;
            Packaged.Name          = Unit.Name;
            Packaged.DllSourcePath = Unit.AssemblyPath;
            Packaged.Deps          = Unit.Deps;
            Out.push_back(std::move(Packaged));
        }
    }

    void GenerateScriptProjects()
    {
        if (!bInitialized)
        {
            return;
        }

        const FString ExePath = NativePath(Platform::GetCurrentProcessPath());
        const FString Dll = NativePath(Join(ParentOf(ExePath), "DotNet/Managed/LuminaSharp.dll"));

        // One SDK-style project per unit (game, each enabled plugin, engine library), each ProjectReferencing
        // its dependencies, plus a solution tying them together. Same unit graph the runtime compiles, so the
        // IDE view matches what actually builds. Only emit for roots that exist on disk.
        const TVector<FScriptUnit> Units = BuildScriptUnits();
        for (const FScriptUnit& Unit : Units)
        {
            if (Unit.DiskDir.empty())
            {
                continue;
            }
            std::error_code Ec;
            if (!Filesystem::Exists(Unit.DiskDir))
            {
                continue;
            }
            WriteTextIfChanged(UnitProjectPath(Unit), BuildCsprojXml(Unit, Units, Dll));
        }

        WriteScriptSolution(Units);
    }

    int32 GetScriptGeneration()
    {
        return GCachedGeneration; // native mirror; refreshed on (re)load, see ReloadScripts
    }

    void PollScriptInput(CObject* Script, const FInputActionState* States, int32 Count, uint32 Serial,
        float DeltaTime)
    {
        if (!bInitialized || Script == nullptr || States == nullptr)
        {
            return;
        }

        // Find, never create: a C++ script has no managed instance and minting one per frame for every
        // script on an input entity is exactly the cost this lookup exists to avoid.
        void* Handle = ManagedInstances::Find(Script);
        if (Handle == nullptr)
        {
            return;
        }

        // An engine export lives in the non-collectible LuminaSharp.dll, so its pointer outlives hot
        // reloads and is safe to cache, matching what the generated ScriptEvent shims do.
        using FThunk = void (CORECLR_DELEGATE_CALLTYPE*)(void*, const FInputActionState*, int32, uint32, float);
        static FThunk Thunk = (FThunk)ResolveManagedExport("PollScriptInputBindings");
        if (Thunk == nullptr)
        {
            return;
        }

        Thunk(Handle, States, Count, Serial, DeltaTime);
    }

    void* ResolveManagedExport(FStringView Name)
    {
        if (!bInitialized || GResolveManagedExport == nullptr)
        {
            return nullptr;
        }
        return GResolveManagedExport(Name.data(), (int32)Name.size());
    }

    bool GetRuntimeDiagnostics(FScriptDiagnostics& OutDiagnostics, bool bForceCollect)
    {
        OutDiagnostics = FScriptDiagnostics{};
        if (!bInitialized || GManaged.GetRuntimeDiagnostics == nullptr)
        {
            return false;
        }
        return GManaged.GetRuntimeDiagnostics(&OutDiagnostics, bForceCollect ? 1 : 0) != 0;
    }

    void GatherEntityScriptTypes(TVector<FString>& OutTypeNames)
    {
        OutTypeNames.clear();
        if (bInitialized && GManaged.EnumerateEntityScripts)
        {
            GManaged.EnumerateEntityScripts(reinterpret_cast<void*>(&LmScriptNameSink), &OutTypeNames);
        }
    }

    void GatherScriptableTypes(TVector<FScriptableTypeDesc>& Out)
    {
        Out.clear();
        if (bInitialized && GManaged.EnumerateScriptables)
        {
            GManaged.EnumerateScriptables(reinterpret_cast<void*>(&LmScriptableSink), &Out);
        }
    }

    void GatherScriptableAliases(TVector<FScriptableAlias>& Out)
    {
        Out.clear();
        if (bInitialized && GManaged.EnumerateScriptableAliases)
        {
            GManaged.EnumerateScriptableAliases(reinterpret_cast<void*>(&LmScriptableAliasSink), &Out);
        }
    }

    void* CreateScriptable(FStringView TypeName, uint64 NativePtr)
    {
        if (!bInitialized || GManaged.CreateScriptable == nullptr)
        {
            return nullptr;
        }
        return GManaged.CreateScriptable(TypeName.data(), (int32)TypeName.size(), NativePtr);
    }

    void ApplyScriptableDefaults(FStringView TypeName, void* DefaultObject)
    {
        if (!bInitialized || GManaged.ApplyScriptableDefaults == nullptr || DefaultObject == nullptr)
        {
            return;
        }
        GManaged.ApplyScriptableDefaults(TypeName.data(), (int32)TypeName.size(), (uint64)(uintptr_t)DefaultObject);
    }

    void GatherManagedSystemDescs(TVector<FManagedSystemDesc>& Out)
    {
        Out.clear();
        if (bInitialized && GManaged.EnumerateEntitySystems)
        {
            GManaged.EnumerateEntitySystems(reinterpret_cast<void*>(&LmSystemDescSink), &Out);
        }
    }

    void* CreateManagedSystem(FStringView TypeName, uint64 World)
    {
        if (!bInitialized || GManaged.CreateEntitySystem == nullptr)
        {
            return nullptr;
        }
        return GManaged.CreateEntitySystem(TypeName.data(), (int32)TypeName.size(), World);
    }

    void DestroyManagedSystem(void* Handle)
    {
        if (bInitialized && GManaged.DestroyEntitySystem && Handle)
        {
            GManaged.DestroyEntitySystem(Handle);
        }
    }

    void TickManagedSystem(void* Handle, const FSystemContext* Context)
    {
        if (bInitialized && GManaged.TickEntitySystem && Handle)
        {
            GManaged.TickEntitySystem(Handle, const_cast<void*>(reinterpret_cast<const void*>(Context)));
        }
    }

    void GatherManagedRenderSceneTypes(TVector<FString>& Out)
    {
        Out.clear();
        if (bInitialized && GManaged.EnumerateRenderScenes)
        {
            GManaged.EnumerateRenderScenes(reinterpret_cast<void*>(&LmScriptNameSink), &Out);
        }
    }

    void* CreateManagedRenderScene(FStringView TypeName, uint64 World)
    {
        if (!bInitialized || GManaged.CreateRenderScene == nullptr)
        {
            return nullptr;
        }
        return GManaged.CreateRenderScene(TypeName.data(), (int32)TypeName.size(), World);
    }

    void DestroyManagedRenderScene(void* Handle)
    {
        if (bInitialized && GManaged.DestroyRenderScene && Handle)
        {
            GManaged.DestroyRenderScene(Handle);
        }
    }

    void ManagedRenderSceneExtract(void* Handle, const void* View)
    {
        if (bInitialized && GManaged.RenderSceneExtract && Handle)
        {
            GManaged.RenderSceneExtract(Handle, View);
        }
    }

    void ManagedRenderSceneRender(void* Handle, int32 FrameIndex)
    {
        if (bInitialized && GManaged.RenderSceneRender && Handle)
        {
            GManaged.RenderSceneRender(Handle, FrameIndex);
        }
    }

    void ManagedRenderSceneResize(void* Handle, uint32 Width, uint32 Height)
    {
        if (bInitialized && GManaged.RenderSceneResize && Handle)
        {
            GManaged.RenderSceneResize(Handle, Width, Height);
        }
    }

    uint64 ManagedRenderSceneGetDisplayTexture(void* Handle)
    {
        if (!bInitialized || GManaged.RenderSceneGetDisplayTexture == nullptr || Handle == nullptr)
        {
            return 0;
        }
        return GManaged.RenderSceneGetDisplayTexture(Handle);
    }

    uint32 ManagedRenderSceneGetDisplayResourceID(void* Handle)
    {
        if (!bInitialized || GManaged.RenderSceneGetDisplayResourceID == nullptr || Handle == nullptr)
        {
            return ~0u;
        }
        return GManaged.RenderSceneGetDisplayResourceID(Handle);
    }

    void ManagedRenderSceneGetExtent(void* Handle, uint32* OutWidth, uint32* OutHeight)
    {
        if (bInitialized && GManaged.RenderSceneGetExtent && Handle)
        {
            GManaged.RenderSceneGetExtent(Handle, OutWidth, OutHeight);
        }
    }

    namespace
    {
        // Captures the schema blob managed writes (Ctx is a TVector<uint8>).
        void LmSchemaBlobSink(void* Ctx, const char* Data, int Len)
        {
            auto* Out = static_cast<TVector<uint8>*>(Ctx);
            if (Out != nullptr && Data != nullptr && Len > 0)
            {
                Out->assign(reinterpret_cast<const uint8*>(Data), reinterpret_cast<const uint8*>(Data) + Len);
            }
        }

        // Little-endian cursor over the managed-written schema blob (see ScriptProperties.BuildSchemaBlob).
        struct FBlobReader
        {
            const uint8* P;
            const uint8* End;

            bool Take(void* Dst, size_t N) { if (P + N > End) { return false; } memcpy(Dst, P, N); P += N; return true; }
            int32   I32() { int32 V = 0; Take(&V, 4); return V; }
            int64   I64() { int64 V = 0; Take(&V, 8); return V; }
            double  F64() { double V = 0; Take(&V, 8); return V; }
            uint8   U8()  { uint8 V = 0; Take(&V, 1); return V; }

            // On a truncated/corrupt length, consume the rest of the buffer so every subsequent read fails
            // cleanly: returning without advancing P would desync the cursor and silently misread the tail.
            FString Str()
            {
                int32 N = I32();
                if (N == 0) { return FString(); }
                if (N < 0 || P + N > End) { P = End; return FString(); }
                FString S(reinterpret_cast<const char*>(P), static_cast<size_t>(N));
                P += N;
                return S;
            }
        };

        FString NumberToString(double V) { char Buf[32]; snprintf(Buf, sizeof(Buf), "%g", V); return FString(Buf); }

        // Append-cursor over a byte vector; mirror of the managed FBlobReader (little-endian).
        struct FBlobWriter
        {
            TVector<uint8>& B;
            void Raw(const void* P, size_t N) { B.insert(B.end(), (const uint8*)P, (const uint8*)P + N); }
            void U8(uint8 V) { B.push_back(V); }
            void I32(int32 V) { Raw(&V, 4); }
            void I64(int64 V) { Raw(&V, 8); }
            void F64(double V) { Raw(&V, 8); }
            void Str(FStringView S) { I32((int32)S.size()); Raw(S.data(), S.size()); }
        };

        // Folds a field's alias list into the field metadata as a ';'-joined "Aliases" value.
        void ReadAliasesInto(FBlobReader& R, Scripting::FScriptExportMeta& Meta)
        {
            const int32 N = R.I32();
            FString Joined;
            for (int32 i = 0; i < N; ++i)
            {
                const FString Alias = R.Str();
                if (Alias.empty())
                {
                    continue;
                }
                if (!Joined.empty())
                {
                    Joined += ";";
                }
                Joined += Alias;
            }
            if (!Joined.empty())
            {
                Meta.Set("Aliases", Joined);
            }
        }

        // The editor display block a [Property] carries: category, tooltip, units, clamps, color flag.
        // Shared by the top-level schema and by every nested / instanced-candidate field so the two can
        // never drift -- they read the identical bytes WriteMeta wrote.
        void ReadMetaInto(FBlobReader& R, Scripting::FScriptExportMeta& Meta)
        {
            const FString Category = R.Str();
            const FString Tooltip  = R.Str();
            const FString Units    = R.Str();
            if (!Category.empty()) { Meta.Set("Category", Category); }
            if (!Tooltip.empty())  { Meta.Set("ToolTip", Tooltip); }
            if (!Units.empty())    { Meta.Set("Units", Units); }
            if (R.U8()) { Meta.Set("ClampMin", NumberToString(R.F64())); }
            if (R.U8()) { Meta.Set("ClampMax", NumberToString(R.F64())); }
            if (R.U8()) { Meta.Set("Color", FString()); }
        }

        void ReadValue(FBlobReader& R, Scripting::FScriptPropertyValue& Out);

        TSharedPtr<Scripting::FScriptExportType> ReadType(FBlobReader& R)
        {
            auto Type = MakeShared<Scripting::FScriptExportType>();
            Type->Kind = static_cast<EPropertyTypeFlags>(R.U8());
            Type->bEntity = R.U8() != 0;
            Type->bInputAction = R.U8() != 0;
            switch (Type->Kind)
            {
                case EPropertyTypeFlags::Enum:
                {
                    Type->EnumName = FName(R.Str().c_str());
                    Type->EnumUnderlying = static_cast<EPropertyTypeFlags>(R.U8());
                    const int32 N = R.I32();
                    for (int32 i = 0; i < N; ++i)
                    {
                        Scripting::FScriptEnumEntry E;
                        E.Name = FName(R.Str().c_str());
                        E.Value = R.I64();
                        Type->EnumEntries.push_back(E);
                    }
                    break;
                }
                case EPropertyTypeFlags::Struct:
                {
                    // A native-struct mirror (NativeName set) and a C#-defined script struct (NativeName empty)
                    // share one wire shape; the reader tells them apart by whether NativeName is present.
                    const FString NativeName = R.Str();
                    if (!NativeName.empty())
                    {
                        Type->NativeName = FName(NativeName.c_str());
                    }
                    const int32 N = R.I32();
                    for (int32 i = 0; i < N; ++i)
                    {
                        Scripting::FScriptExportField F;
                        F.Name = FName(R.Str().c_str());
                        ReadAliasesInto(R, F.Meta);
                        ReadMetaInto(R, F.Meta);
                        F.Type = ReadType(R);
                        ReadValue(R, F.Default);
                        Type->Fields.push_back(F);
                    }
                    break;
                }
                case EPropertyTypeFlags::SoftObject:
                {
                    Type->TargetClass = FName(R.Str().c_str());
                    break;
                }
                case EPropertyTypeFlags::Vector:
                {
                    Type->ElementType = ReadType(R);
                    break;
                }
                case EPropertyTypeFlags::Map:
                {
                    Type->KeyType   = ReadType(R);
                    Type->ValueType = ReadType(R);
                    break;
                }
                case EPropertyTypeFlags::InstancedStruct:
                {
                    Type->BaseName = FName(R.Str().c_str());
                    const int32 NumCandidates = R.I32();
                    for (int32 c = 0; c < NumCandidates; ++c)
                    {
                        Scripting::FScriptExportInstanceCandidate Candidate;
                        Candidate.TypeName = FName(R.Str().c_str());
                        const int32 NumFields = R.I32();
                        for (int32 i = 0; i < NumFields; ++i)
                        {
                            Scripting::FScriptExportField F;
                            F.Name = FName(R.Str().c_str());
                            ReadAliasesInto(R, F.Meta);
                            ReadMetaInto(R, F.Meta);
                            F.Type = ReadType(R);
                            ReadValue(R, F.Default);
                            Candidate.Fields.push_back(F);
                        }
                        Type->Candidates.push_back(std::move(Candidate));
                    }
                    break;
                }
                default: break;
            }
            return Type;
        }

        // Recursive self-describing value reader; each value leads with its kind byte.
        void ReadValue(FBlobReader& R, Scripting::FScriptPropertyValue& Out)
        {
            // (forward-declared above ReadType, which reads a default for every nested field)
            Out = Scripting::FScriptPropertyValue{};
            Out.Kind = static_cast<Scripting::EScriptValueKind>(R.U8());
            switch (Out.Kind)
            {
                case Scripting::EScriptValueKind::Bool:   Out.AsBool = R.U8() != 0; break;
                case Scripting::EScriptValueKind::Int:    Out.AsInt = R.I64(); break;
                case Scripting::EScriptValueKind::Double: Out.AsDouble = R.F64(); break;
                case Scripting::EScriptValueKind::String: Out.AsString = R.Str(); break;
                case Scripting::EScriptValueKind::Array:
                {
                    const int32 N = R.I32();
                    Out.Items.reserve(N > 0 ? N : 0);
                    for (int32 i = 0; i < N; ++i)
                    {
                        Scripting::FScriptPropertyValue E;
                        ReadValue(R, E);
                        Out.Items.push_back(E);
                    }
                    break;
                }
                case Scripting::EScriptValueKind::Map:
                {
                    // count pairs, each [key, value]; stored interleaved in Items (Items[2i]=key, [2i+1]=value).
                    const int32 N = R.I32();
                    Out.Items.reserve(N > 0 ? N * 2 : 0);
                    for (int32 i = 0; i < N; ++i)
                    {
                        Scripting::FScriptPropertyValue K;
                        ReadValue(R, K);
                        Out.Items.push_back(std::move(K));
                        Scripting::FScriptPropertyValue Val;
                        ReadValue(R, Val);
                        Out.Items.push_back(std::move(Val));
                    }
                    break;
                }
                case Scripting::EScriptValueKind::Nested:
                {
                    const int32 N = R.I32();
                    Out.StructFields.reserve(N > 0 ? N : 0);
                    for (int32 i = 0; i < N; ++i)
                    {
                        Scripting::FScriptPropertyEntry E;
                        E.Name = FName(R.Str().c_str());
                        ReadValue(R, E.Value);
                        Out.StructFields.push_back(E);
                    }
                    break;
                }
                case Scripting::EScriptValueKind::Instance:
                {
                    // Chosen type name, then (when non-empty) a field count and each field's name and value.
                    Out.AsString = R.Str();
                    if (!Out.AsString.empty())
                    {
                        const int32 N = R.I32();
                        Out.StructFields.reserve(N > 0 ? N : 0);
                        for (int32 i = 0; i < N; ++i)
                        {
                            Scripting::FScriptPropertyEntry E;
                            E.Name = FName(R.Str().c_str());
                            ReadValue(R, E.Value);
                            Out.StructFields.push_back(E);
                        }
                    }
                    break;
                }
                default: break;
            }
        }

        // Mirror of ReadValue.
        void WriteValue(FBlobWriter& W, const Scripting::FScriptPropertyValue& V)
        {
            W.U8((uint8)V.Kind);
            switch (V.Kind)
            {
                case Scripting::EScriptValueKind::Bool:   W.U8(V.AsBool ? 1 : 0); break;
                case Scripting::EScriptValueKind::Int:    W.I64(V.AsInt); break;
                case Scripting::EScriptValueKind::Double: W.F64(V.AsDouble); break;
                case Scripting::EScriptValueKind::String: W.Str(FStringView(V.AsString.c_str(), V.AsString.size())); break;
                case Scripting::EScriptValueKind::Array:
                {
                    W.I32((int32)V.Items.size());
                    for (const Scripting::FScriptPropertyValue& E : V.Items)
                    {
                        WriteValue(W, E);
                    }
                    break;
                }
                case Scripting::EScriptValueKind::Map:
                {
                    // Items are the interleaved [key, value] pairs; the wire leads with the PAIR count.
                    W.I32((int32)(V.Items.size() / 2));
                    for (const Scripting::FScriptPropertyValue& E : V.Items)
                    {
                        WriteValue(W, E);
                    }
                    break;
                }
                case Scripting::EScriptValueKind::Nested:
                {
                    W.I32((int32)V.StructFields.size());
                    for (const Scripting::FScriptPropertyEntry& E : V.StructFields)
                    {
                        W.Str(FStringView(E.Name.c_str()));
                        WriteValue(W, E.Value);
                    }
                    break;
                }
                case Scripting::EScriptValueKind::Instance:
                {
                    W.Str(FStringView(V.AsString.c_str(), V.AsString.size()));
                    if (!V.AsString.empty())
                    {
                        W.I32((int32)V.StructFields.size());
                        for (const Scripting::FScriptPropertyEntry& E : V.StructFields)
                        {
                            W.Str(FStringView(E.Name.c_str()));
                            WriteValue(W, E.Value);
                        }
                    }
                    break;
                }
                default: break;
            }
        }
    }

    namespace
    {
        // Decodes one managed schema blob. Shared by every caller that asks for a member layout, so the
        // wire format is read in exactly one place regardless of which managed export produced it.
        bool ParseSchemaBlob(const TVector<uint8>& Blob, Scripting::FScriptExportSchema& OutSchema,
            TVector<Scripting::FScriptPropertyEntry>& OutDefaults)
        {
            if (Blob.empty())
            {
                return false;
            }

            FBlobReader R{ Blob.data(), Blob.data() + Blob.size() };
            const int32 Count = R.I32();
            for (int32 i = 0; i < Count; ++i)
            {
                Scripting::FScriptExportField Field;
                Field.Name = FName(R.Str().c_str());
                ReadAliasesInto(R, Field.Meta);
                ReadMetaInto(R, Field.Meta);

                // Top level only: nested fields have no hot-reload identity of their own, so WriteFields
                // does not emit this byte and nothing here may consume one.
                if (R.U8())
                {
                    Field.Meta.Set("SkipHotReload", FString());
                }

                Field.Type = ReadType(R);
                Scripting::FScriptPropertyValue Val;
                ReadValue(R, Val);

                OutSchema.Fields.push_back(Field);
                Scripting::FScriptPropertyEntry Entry;
                Entry.Name = Field.Name;
                Entry.Value = Val;
                OutDefaults.push_back(Entry);
            }
            return true;
        }
    }

    void GatherScriptStructTypes(TVector<FScriptStructTypeDesc>& Out)
    {
        Out.clear();
        if (bInitialized && GManaged.EnumerateScriptStructs)
        {
            GManaged.EnumerateScriptStructs(reinterpret_cast<void*>(&LmScriptStructSink), &Out);
        }
    }

    bool GatherScriptStructSchema(FStringView ScriptTypeName, Scripting::FScriptExportSchema& OutSchema,
        TVector<Scripting::FScriptPropertyEntry>& OutDefaults)
    {
        OutSchema.Fields.clear();
        OutDefaults.clear();
        if (!bInitialized || GManaged.GetScriptStructSchema == nullptr || ScriptTypeName.empty())
        {
            return false;
        }

        const FString Name(ScriptTypeName.data(), ScriptTypeName.size());
        TVector<uint8> Blob;
        GManaged.GetScriptStructSchema(Name.c_str(), (int32)Name.size(), reinterpret_cast<void*>(&LmSchemaBlobSink), &Blob);

        if (!ParseSchemaBlob(Blob, OutSchema, OutDefaults))
        {
            return false;
        }

        OutSchema.ScriptTypeName = FName(Name.c_str());
        return true;
    }

    bool GatherScriptSchema(FStringView ScriptClass, Scripting::FScriptExportSchema& OutSchema, TVector<Scripting::FScriptPropertyEntry>& OutDefaults)
    {
        OutSchema.Fields.clear();
        OutDefaults.clear();
        if (!bInitialized || GManaged.GetScriptSchema == nullptr || ScriptClass.empty())
        {
            return false;
        }

        const FString Name(ScriptClass.data(), ScriptClass.size());
        TVector<uint8> Blob;
        GManaged.GetScriptSchema(Name.c_str(), (int32)Name.size(), reinterpret_cast<void*>(&LmSchemaBlobSink), &Blob);

        return ParseSchemaBlob(Blob, OutSchema, OutDefaults);
    }

    const CScriptStruct* GetScriptStruct(FStringView ScriptClass)
    {
        return GScriptStructs.GetOrBuild(ScriptClass);
    }

    FString ResolveScriptClassName(FStringView ScriptClass)
    {
        FString Result;
        if (bInitialized && GManaged.ResolveEntityScriptName != nullptr && !ScriptClass.empty())
        {
            GManaged.ResolveEntityScriptName(ScriptClass.data(), (int32)ScriptClass.size(),
                reinterpret_cast<void*>(&LmSingleNameSink), &Result);
        }
        return Result;
    }


    void GatherScriptButtons(FStringView ScriptClass, TVector<Scripting::FScriptButton>& OutButtons)
    {
        OutButtons.clear();
        if (!bInitialized || GManaged.GetScriptButtons == nullptr || ScriptClass.empty())
        {
            return;
        }

        const FString Name(ScriptClass.data(), ScriptClass.size());
        TVector<uint8> Blob;
        GManaged.GetScriptButtons(Name.c_str(), (int32)Name.size(), reinterpret_cast<void*>(&LmSchemaBlobSink), &Blob);
        if (Blob.empty())
        {
            return;
        }

        FBlobReader R{ Blob.data(), Blob.data() + Blob.size() };
        const int32 Count = R.I32();
        OutButtons.reserve(Count > 0 ? Count : 0);
        for (int32 i = 0; i < Count; ++i)
        {
            Scripting::FScriptButton Button;
            Button.Method = R.Str();
            Button.Label = R.Str();
            Button.Tooltip = R.Str();
            OutButtons.push_back(std::move(Button));
        }
    }

    bool InvokeScriptButton(void* Instance, FStringView Method)
    {
        if (!bInitialized || Instance == nullptr || Method.empty() || GManaged.InvokeScriptButton == nullptr)
        {
            return false;
        }
        return GManaged.InvokeScriptButton(Instance, Method.data(), (int32)Method.size()) == 0;
    }

    bool IsInitialized()
    {
        return bInitialized;
    }

    // Resumes a managed Asset.LoadAsync continuation on the game thread. Reachable from the global
    // extern "C" load export (which can't see the anonymous-namespace delegate directly).
    void DispatchAssetCallback(void* Callback, void* Object)
    {
        if (bInitialized && GManaged.InvokeAssetCallback != nullptr)
        {
            GManaged.InvokeAssetCallback(Callback, Object);
        }
    }

    namespace
    {
        // Manual reload trigger (the editor's "Reload Scripts" button calls DotNet::ReloadScripts too).
        FAutoConsoleCommand GReloadScriptsCommand(
            "dotnet.reload",
            "Recompile and hot-reload all C# scripts across every mounted Scripts/ folder.",
            []() { Lumina::DotNet::ReloadScripts(); });

        FAutoConsoleCommand GGenProjectsCommand(
            "dotnet.genprojects",
            "(Re)generate the C# IDE project files (.csproj) for every script root.",
            []() { Lumina::DotNet::GenerateScriptProjects(); });
    }
}

// Validates the managed P/Invoke path (LibraryImport "LuminaNative" -> this Runtime module via the
// DllImportResolver). The generated property thunks are exported the same way.
LUMINA_DOTNET_EXPORT(int, NativeSelfTest)(int A, int B)
{
    return A + B;
}

namespace
{
    Lumina::FEntityRegistry* LmRegistryFromWorld(uint64 World)
    {
        Lumina::CWorld* W = reinterpret_cast<Lumina::CWorld*>(World);
        return W ? &Lumina::ECS::GetWorldRegistry(*W) : nullptr;
    }
}

// Resolves a reflected component type name to its op-table token (the C# side caches it per type).
LUMINA_DOTNET_EXPORT(const void*, FindComponentOps)(const char* Name, int Len)
{
    if (Name == nullptr || Len <= 0)
    {
        return nullptr;
    }
    return Lumina::FindComponentOps(Lumina::FStringView(Name, static_cast<size_t>(Len)));
}

LUMINA_DOTNET_EXPORT(void*, GetComponent)(uint64 World, uint32 Entity, const void* Ops)
{
    Lumina::FEntityRegistry* R = LmRegistryFromWorld(World);
    const auto* O = static_cast<const Lumina::FComponentOps*>(Ops);
    return (R && O) ? O->Get(*R, static_cast<entt::entity>(Entity)) : nullptr;
}

LUMINA_DOTNET_EXPORT(int, HasComponent)(uint64 World, uint32 Entity, const void* Ops)
{
    Lumina::FEntityRegistry* R = LmRegistryFromWorld(World);
    const auto* O = static_cast<const Lumina::FComponentOps*>(Ops);
    return (R && O) ? O->Has(*R, static_cast<entt::entity>(Entity)) : 0;
}

// Get-or-emplace a default component, returning the live pointer to configure in place.
LUMINA_DOTNET_EXPORT(void*, EmplaceComponent)(uint64 World, uint32 Entity, const void* Ops)
{
    Lumina::FEntityRegistry* R = LmRegistryFromWorld(World);
    const auto* O = static_cast<const Lumina::FComponentOps*>(Ops);
    return (R && O) ? O->Emplace(*R, static_cast<entt::entity>(Entity)) : nullptr;
}

LUMINA_DOTNET_EXPORT(int, RemoveComponent)(uint64 World, uint32 Entity, const void* Ops)
{
    Lumina::FEntityRegistry* R = LmRegistryFromWorld(World);
    const auto* O = static_cast<const Lumina::FComponentOps*>(Ops);
    return (R && O) ? O->Remove(*R, static_cast<entt::entity>(Entity)) : 0;
}

// Registry signals: connect a managed listener (UnmanagedCallersOnly Thunk + GCHandle Ctx) to a component's
// on_construct/on_destroy/on_update sink. Allocates the listener (its address is the disconnect key) and
// returns it as an opaque subscription handle; null on failure. Kind matches EComponentSignal.
LUMINA_DOTNET_EXPORT(void*, RegistryConnect)(uint64 World, const void* Ops, int32 Kind, void* Thunk, void* Context)
{
    Lumina::FEntityRegistry* R = LmRegistryFromWorld(World);
    const auto* O = static_cast<const Lumina::FComponentOps*>(Ops);
    if (R == nullptr || O == nullptr || O->ConnectSignal == nullptr || Thunk == nullptr)
    {
        return nullptr;
    }
    auto* Listener = new Lumina::FManagedSignalListener{ reinterpret_cast<Lumina::FManagedSignalThunk>(Thunk), Context };
    O->ConnectSignal(*R, static_cast<Lumina::EComponentSignal>(Kind), Listener);
    return Listener;
}

// Disconnect a listener returned by Connect and free it. Safe with a null handle / torn-down world.
LUMINA_DOTNET_EXPORT(void, RegistryDisconnect)(uint64 World, const void* Ops, int32 Kind, void* Handle)
{
    if (Handle == nullptr)
    {
        return;
    }
    Lumina::FEntityRegistry* R = LmRegistryFromWorld(World);
    const auto* O = static_cast<const Lumina::FComponentOps*>(Ops);
    if (R != nullptr && O != nullptr && O->DisconnectSignal != nullptr)
    {
        O->DisconnectSignal(*R, static_cast<Lumina::EComponentSignal>(Kind), static_cast<Lumina::FManagedSignalListener*>(Handle));
    }
    delete static_cast<Lumina::FManagedSignalListener*>(Handle);
}

// Fire on_update<T> for an entity's component (the manual "signal" pulse). No-op for tags or if absent.
LUMINA_DOTNET_EXPORT(void, RegistryPatch)(uint64 World, uint32 Entity, const void* Ops)
{
    Lumina::FEntityRegistry* R = LmRegistryFromWorld(World);
    const auto* O = static_cast<const Lumina::FComponentOps*>(Ops);
    if (R != nullptr && O != nullptr && O->Patch != nullptr)
    {
        O->Patch(*R, static_cast<entt::entity>(Entity));
    }
}

LUMINA_DOTNET_EXPORT(void, SetObjectPtr)(void* Member, void* Value)
{
    if (Member != nullptr)
    {
        *reinterpret_cast<Lumina::TObjectPtr<Lumina::CObject>*>(Member) = static_cast<Lumina::CObject*>(Value);
    }
}

// Weak-handle validity backing the managed NativeObject. GetHandle packs a live CObject*'s array handle
// as (Generation << 32 | Index), Index = -1 if the object isn't array-tracked. Resolve returns the live
// CObject* only if that (Index, Generation) still names a live, non-destroyed object, else null, so a
// managed wrapper to a since-freed object throws on access instead of reading reclaimed memory.
LUMINA_DOTNET_EXPORT(int64, ObjectGetHandle)(void* Object)
{
    const Lumina::FObjectHandle Handle(static_cast<Lumina::CObjectBase*>(static_cast<Lumina::CObject*>(Object)));
    return (static_cast<int64>(static_cast<uint32>(Handle.Generation)) << 32)
         | static_cast<int64>(static_cast<uint32>(Handle.Index));
}

LUMINA_DOTNET_EXPORT(void*, ObjectResolve)(int32 Index, int32 Generation)
{
    return Lumina::FObjectHandle(Index, Generation).Resolve();
}

// Per-CObject managed-instance cache, backing Wrapper<T>.ForObject. Get returns the WEAK GC handle of the
// wrapper already created for this object (or null); Set installs one, freeing whatever it replaces. Passing
// a null handle to Set clears the slot. Weak by design: the cache remembers the wrapper that exists, it never
// keeps one alive, so it cannot pin the collectible script ALC across a hot reload.
LUMINA_DOTNET_EXPORT(void*, ObjectGetManagedInstance)(void* Object)
{
    return Lumina::ManagedInstances::Find(static_cast<Lumina::CObjectBase*>(static_cast<Lumina::CObject*>(Object)));
}

LUMINA_DOTNET_EXPORT(void, ObjectSetManagedInstance)(void* Object, void* Handle)
{
    Lumina::ManagedInstances::Set(static_cast<Lumina::CObjectBase*>(static_cast<Lumina::CObject*>(Object)), Handle);
}

// Drains every cached managed instance. Called by the managed teardown contract (ScriptManager.UnloadCurrent)
// before the collectible ALC unloads: Scriptable subclass instances are held by STRONG handles here.
LUMINA_DOTNET_EXPORT(void, ReleaseAllManagedInstances)()
{
    Lumina::ManagedInstances::ReleaseAll();
}

// Synchronous (blocking) load by virtual path; returns the CObject* or null. Backs Asset.Load<T>.
LUMINA_DOTNET_EXPORT(void*, LoadObject)(const char* Path, int Len)
{
    if (Path == nullptr || Len <= 0)
    {
        return nullptr;
    }
    return Lumina::StaticLoadObject(Lumina::FStringView(Path, static_cast<size_t>(Len)));
}

// Registry probe (no load). Backs Asset.Exists.
LUMINA_DOTNET_EXPORT(int, AssetExists)(const char* Path, int Len)
{
    if (Path == nullptr || Len <= 0)
    {
        return 0;
    }
    return Lumina::FAssetRegistry::Get().GetAssetByPath(Lumina::FStringView(Path, static_cast<size_t>(Len))) != nullptr ? 1 : 0;
}

// Async load; resumes the managed continuation (a GCHandle to an Action) on the game thread with the
// loaded CObject* (or null). Backs Asset.LoadAsync<T>.
LUMINA_DOTNET_EXPORT(void, LoadObjectAsync)(const char* Path, int Len, void* Callback)
{
    if (Path == nullptr || Len <= 0)
    {
        Lumina::DotNet::DispatchAssetCallback(Callback, nullptr);
        return;
    }

    Lumina::FSoftObjectPath Soft{ Lumina::FStringView(Path, static_cast<size_t>(Len)) };
    Soft.LoadAsync([Callback](Lumina::CObject* Object)
    {
        Lumina::MainThread::Enqueue([Callback, Object]()
        {
            Lumina::DotNet::DispatchAssetCallback(Callback, Object);
        });
    });
}

// Reverse lookup: a loaded CObject* -> its asset's virtual path (registry by GUID). Fills Buffer up to
// Capacity and returns the full length. Backs serializing a C# TObjectPtr<T> field back to a path.
LUMINA_DOTNET_EXPORT(int, GetObjectPath)(void* Object, char* Buffer, int Capacity)
{
    if (Object == nullptr)
    {
        return 0;
    }
    Lumina::CObject* O = static_cast<Lumina::CObject*>(Object);
    Lumina::FAssetData* Data = Lumina::FAssetRegistry::Get().GetAssetByGUID(O->GetGUID());
    if (Data == nullptr)
    {
        return 0;
    }
    const char* S = Data->Path.c_str();
    const int L = static_cast<int>(Data->Path.size());
    if (Buffer != nullptr && Capacity > 0)
    {
        const int N = L < Capacity ? L : Capacity;
        for (int i = 0; i < N; ++i)
        {
            Buffer[i] = S[i];
        }
    }
    return L;
}

// Maps a module name ("Runtime", "Editor", "Sandbox", ...) to its loaded native handle so the managed
// binding resolver can route each generated binding's call to the module that exports its thunks. Matches
// the base name up to the build-config suffix ("Editor-Development.dll") or extension, so any module's
// reflected types can carry C# bindings, not just Runtime.
//
// Monolithic (LUMINA_MONOLITHIC): engine + plugin module thunks are whole-archived into the exe's own
// export table (there is no per-module DLL to enumerate), so a name matching no loaded DLL falls back to
// the exe handle. A separately loaded game module DLL (<Project>-<Cfg>.dll) still matches the enumeration
// and keeps its own handle, so its thunks resolve from there.
LUMINA_DOTNET_EXPORT(void*, ResolveModuleHandle)(const char* Name, int Len)
{
    if (Name == nullptr || Len <= 0)
    {
        return nullptr;
    }

#if defined(_WIN32)
    auto Matches = [&](const wchar_t* Base) -> bool
    {
        for (int i = 0; i < Len; ++i)
        {
            const wchar_t Bc = Base[i];
            if (Bc == 0)
            {
                return false; // base name shorter than the queried module name
            }
            const wchar_t A = (Bc >= L'A' && Bc <= L'Z') ? wchar_t(Bc - L'A' + L'a') : Bc;
            const unsigned char Nc = static_cast<unsigned char>(Name[i]);
            const wchar_t B = (Nc >= 'A' && Nc <= 'Z') ? wchar_t(Nc - 'A' + 'a') : wchar_t(Nc);
            if (A != B)
            {
                return false;
            }
        }
        const wchar_t Next = Base[Len]; // must end the stem
        return Next == L'-' || Next == L'.' || Next == 0;
    };

    HANDLE Snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (Snap == INVALID_HANDLE_VALUE)
    {
        return nullptr;
    }

    MODULEENTRY32W Entry;
    Entry.dwSize = sizeof(Entry);
    void* Result = nullptr;
    if (::Module32FirstW(Snap, &Entry))
    {
        do
        {
            if (Matches(Entry.szModule))
            {
                Result = Entry.hModule;
                break;
            }
        }
        while (::Module32NextW(Snap, &Entry));
    }
    ::CloseHandle(Snap);
#ifdef LUMINA_MONOLITHIC
    if (Result == nullptr)
    {
        Result = ::GetModuleHandleW(nullptr); // exe export table holds every whole-archived module's thunks
    }
#endif
    return Result;
#else
    struct FCtx { const char* Name; int Len; void* Result; } Ctx{ Name, Len, nullptr };
    ::dl_iterate_phdr([](struct dl_phdr_info* Info, size_t, void* Data) -> int
    {
        FCtx& C = *static_cast<FCtx*>(Data);
        const char* Path = Info->dlpi_name;
        if (Path == nullptr || Path[0] == 0)
        {
            return 0;
        }
        const char* Base = std::strrchr(Path, '/');
        Base = Base ? Base + 1 : Path;
        if (std::strncmp(Base, "lib", 3) == 0)
        {
            Base += 3; // drop the conventional "lib" prefix
        }
        if (std::strncmp(Base, C.Name, static_cast<size_t>(C.Len)) == 0)
        {
            const char Next = Base[C.Len];
            if (Next == '-' || Next == '.' || Next == 0)
            {
                C.Result = ::dlopen(Path, RTLD_NOW | RTLD_NOLOAD);
                return 1;
            }
        }
        return 0;
    }, &Ctx);
#ifdef LUMINA_MONOLITHIC
    if (Ctx.Result == nullptr)
    {
        Ctx.Result = ::dlopen(nullptr, RTLD_NOW | RTLD_NOLOAD); // main program handle (all static thunks)
    }
#endif
    return Ctx.Result;
#endif
}

// Bootstrap size check against the C# FScriptDiagnostics mirror (Diagnostics.cs).
LE_REGISTER_LAYOUT("FScriptDiagnostics", Lumina::DotNet::FScriptDiagnostics);
