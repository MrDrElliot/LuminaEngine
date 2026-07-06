#include "DotNetHost.h"
#include "ManagedCall.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <cstring>
#include <cstdio>

#include "Containers/Array.h"
#include "Containers/String.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Delegates/ScriptDelegate.h"
#include "Core/Engine/Engine.h"
#include "Scripting/ScriptStruct.h"
#include "Scripting/ScriptableObject.h"
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
    // Defined in CSharpLayoutChecks.cpp: validates the EASTL FString/TVector byte layout that
    // LuminaSharp.NativeMarshal reads in place (Dev/Debug self-test; a no-op in Shipping).
    void VerifyEASTLInteropLayout();
}

namespace Lumina::DotNet
{
    namespace
    {
    #if defined(_WIN32)
        #define LSTR(s) L##s
        constexpr const char* kSharedExt = ".dll";
    #elif defined(__APPLE__)
        #define LSTR(s) s
        constexpr const char* kSharedExt = ".dylib";
    #else
        #define LSTR(s) s
        constexpr const char* kSharedExt = ".so";
    #endif

        namespace fs = std::filesystem;

        // Defined further down; the project-generation helpers above it normalize paths through it.
        fs::path NativePath(fs::path P);

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
        typedef void* (CORECLR_DELEGATE_CALLTYPE* CreateEntityScriptFn)(const char*, int32, uint64, uint32);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* OnReadyScriptFn)(void*);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* UpdateScriptsFn)(void* const*, int32, float);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* FixedUpdateScriptsFn)(void* const*, int32, float);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* DestroyEntityScriptFn)(void*);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* EnumerateEntityScriptsFn)(void*, void*);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* EnumerateEntitySystemsFn)(void*, void*);
        typedef void* (CORECLR_DELEGATE_CALLTYPE* CreateScriptableFn)(const char*, int32, uint64, int32*);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* DestroyScriptableFn)(void*);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* EnumerateScriptablesFn)(void*, void*);
        typedef void* (CORECLR_DELEGATE_CALLTYPE* CreateEntitySystemFn)(const char*, int32, uint64);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* TickEntitySystemFn)(void*, void*);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* DestroyEntitySystemFn)(void*);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* DispatchInputFn)(void*, int32, int32, int32, int32, int32, double, double, double, double, double);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* OnNativeDelegateDestroyedFn)(void*);
        typedef int32 (CORECLR_DELEGATE_CALLTYPE* GetCallbackFlagsFn)(void*);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* GetScriptSchemaFn)(const char*, int32, void*, void*);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* GetScriptButtonsFn)(const char*, int32, void*, void*);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* ResolveEntityScriptNameFn)(const char*, int32, void*, void*);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* ApplyScriptPropertiesFn)(void*, const uint8*, int32);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* InvokeAssetCallbackFn)(void*, void*);
        typedef void* (CORECLR_DELEGATE_CALLTYPE* ManagedClassFindFn)(const char*, int32);
        typedef void* (CORECLR_DELEGATE_CALLTYPE* ManagedObjectNewFn)(void*);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* ManagedFreeHandleFn)(void*);
        typedef int32 (CORECLR_DELEGATE_CALLTYPE* ManagedInvokeFn)(void*, uint8, const char*, int32, const uint8*, int32, void*, void*);
        typedef int32 (CORECLR_DELEGATE_CALLTYPE* ManagedFieldGetFn)(void*, const char*, int32, void*, void*);
        typedef int32 (CORECLR_DELEGATE_CALLTYPE* ManagedFieldSetFn)(void*, const char*, int32, const uint8*, int32);

        // A LOCAL typed cache of the engine's managed entries (call sites stay typed). NOT an ABI mirror: native
        // fills each field at bootstrap by resolving it by name via ResolveManagedExport, so field set/order is
        // a local concern only, no C# struct to match, no drift hash. A missing entry fails loudly per-name.
        struct FManagedExports
        {
            ApplyScriptPropertiesFn     ApplyScriptProperties;
            ManagedClassFindFn          ClassFind;
            CreateEntityScriptFn        CreateEntityScript;
            CreateEntitySystemFn        CreateEntitySystem;
            DestroyEntityScriptFn       DestroyEntityScript;
            DestroyEntitySystemFn       DestroyEntitySystem;
            DispatchInputFn             DispatchInput;
            OnNativeDelegateDestroyedFn OnNativeDelegateDestroyed;
            EnumerateEntityScriptsFn    EnumerateEntityScripts;
            EnumerateEntitySystemsFn    EnumerateEntitySystems;
            CreateScriptableFn          CreateScriptable;
            DestroyScriptableFn         DestroyScriptable;
            EnumerateScriptablesFn      EnumerateScriptables;
            ManagedFieldGetFn           FieldGet;
            ManagedFieldSetFn           FieldSet;
            ManagedFreeHandleFn         FreeHandle;
            GetGenerationFn             GetGeneration;
            GetRuntimeDiagnosticsFn     GetRuntimeDiagnostics;
            GetCallbackFlagsFn          GetScriptCallbackFlags;
            GetScriptSchemaFn           GetScriptSchema;
            GetScriptButtonsFn          GetScriptButtons;
            ResolveEntityScriptNameFn   ResolveEntityScriptName;
            ManagedInvokeFn             Invoke;
            InvokeAssetCallbackFn       InvokeAssetCallback;
            LoadScriptsFn               LoadScripts;
            ManagedObjectNewFn          ObjectNew;
            OnReadyScriptFn             OnReadyScript;
            ShutdownFn                  Shutdown;
            TickFn                      Tick;
            TickEntitySystemFn          TickEntitySystem;
            UpdateScriptsFn             UpdateScripts;
            FixedUpdateScriptsFn        FixedUpdateScripts;
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

        // Sink the managed EnumerateScriptables calls once per Scriptable C# type; Ctx is the out desc vector.
        void LmScriptableSink(void* Ctx, const char* Name, int NameLen, const char* Base, int BaseLen)
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
            Out->emplace_back(eastl::move(Desc));
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
                Out->push_back(eastl::move(Desc));
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

            std::error_code Ec;
            const fs::path Root = NativePath(fs::path(DiskDir.c_str()));
            if (!fs::exists(Root, Ec) || !fs::is_directory(Root, Ec))
            {
                return;
            }

            for (fs::recursive_directory_iterator It(Root, fs::directory_options::skip_permission_denied, Ec), End;
                 It != End && !Ec; It.increment(Ec))
            {
                std::error_code FileEc;
                if (!It->is_regular_file(FileEc) || FileEc)
                {
                    continue;
                }
                const fs::path& P = It->path();
                if (P.extension() != ".cs")
                {
                    continue;
                }
                const std::string PathStr = P.string();
                if (PathStr.find("\\obj\\") != std::string::npos || PathStr.find("/obj/") != std::string::npos
                    || PathStr.find("\\bin\\") != std::string::npos || PathStr.find("/bin/") != std::string::npos)
                {
                    continue;
                }

                std::ifstream In(P, std::ios::binary);
                if (!In)
                {
                    continue;
                }
                const std::string Text((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());

                FGatheredSource Src;
                Src.Path = FString(PathStr.c_str());
                Src.Text = FString(Text.data(), Text.size());
                Out.push_back(eastl::move(Src));
            }
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
                Units.push_back(eastl::move(Unit));
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
                Units.push_back(eastl::move(Game));
            }

            {
                FScriptUnit Engine;
                Engine.Name = "Engine";
                Engine.DiskDir = Paths::GetEngineResourceDirectory() + "/Scripts";
                // Engine example scripts live next to the engine binaries (a sibling of DotNet/Managed, not in it).
                const fs::path ExeDir = NativePath(fs::path(Platform::GetCurrentProcessPath().c_str())).parent_path();
                Engine.BinaryDir       = FString((ExeDir / "DotNet").string().c_str());
                Engine.IntermediateDir = FString((ExeDir / "DotNet" / "obj" / "Engine").string().c_str());
                Engine.AssemblyPath    = Engine.BinaryDir + "/Engine.dll";
                Units.push_back(eastl::move(Engine));
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

            const fs::path ExeDir      = NativePath(fs::path(Platform::GetCurrentProcessPath().c_str())).parent_path();
            const fs::path ScriptsDir  = ExeDir / "DotNet" / "Scripts";
            const fs::path ManifestPath = ScriptsDir / "scripts.manifest.json";

            std::error_code Ec;
            if (!fs::exists(ManifestPath, Ec))
            {
                return Units;
            }

            std::ifstream In(ManifestPath, std::ios::binary);
            if (!In)
            {
                return Units;
            }
            const std::string Text((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());

            nlohmann::json J;
            try
            {
                J = nlohmann::json::parse(Text);
            }
            catch (const std::exception& Err)
            {
                LOG_ERROR("C#: failed to parse cooked script manifest '{}': {}", ManifestPath.string().c_str(), Err.what());
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
                Unit.AssemblyPath = FString((ScriptsDir / Dll).string().c_str());

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
                Units.push_back(eastl::move(Unit));
            }
            return Units;
        }

        // Writes Content to Path only when it differs from what is on disk (so frequent reloads don't churn
        // the files and the IDE doesn't reload projects on every hot-reload). Creates parent dirs as needed.
        void WriteTextIfChanged(const fs::path& Path, const std::string& Content)
        {
            std::error_code Ec;
            fs::create_directories(Path.parent_path(), Ec);

            {
                std::ifstream In(Path, std::ios::binary);
                if (In)
                {
                    std::string Existing((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
                    if (Existing == Content)
                    {
                        return;
                    }
                }
            }

            std::ofstream Out(Path, std::ios::binary | std::ios::trunc);
            if (Out)
            {
                Out.write(Content.data(), static_cast<std::streamsize>(Content.size()));
                LOG_DISPLAY("Generated C# script project: {}", Path.string().c_str());
            }
        }

        // Sidecar stamped next to a unit's DLL when WE compiled it from sources. It is what tells a stale
        // emitted artifact (sources since deleted -> delete the DLL, never load it) apart from an authored
        // prebuilt assembly at the same canonical path (a code-only plugin -> load it).
        fs::path CompiledMarkerPath(const FString& DllPath)
        {
            return NativePath(fs::path((DllPath + ".compiled").c_str()));
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
        fs::path UnitProjectPath(const FScriptUnit& Unit)
        {
            return NativePath(fs::path(Unit.DiskDir.c_str()) / (std::string(Unit.Name.c_str()) + ".Scripts.csproj"));
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
                std::error_code Ec;
                if (Dep == nullptr || Dep->DiskDir.empty() || !fs::exists(NativePath(fs::path(Dep->DiskDir.c_str())), Ec))
                {
                    continue; // the dependency ships no scripts on disk; nothing to reference
                }
                Refs += "    <ProjectReference Include=\"" + UnitProjectPath(*Dep).string() + "\" />\n";
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
                std::error_code Ec;
                if (!fs::exists(NativePath(fs::path(Unit.DiskDir.c_str())), Ec))
                {
                    continue;
                }
                Entries.push_back({ std::string(Unit.Name.c_str()) + ".Scripts", UnitProjectPath(Unit).string(), MakeStableGuid(Unit.Name) });
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
            const fs::path SlnPath = NativePath(fs::path(FString(GEngine->GetProjectPath().data(), GEngine->GetProjectPath().size()).c_str())
                / (std::string(ProjectName.c_str()) + ".GameScripts.sln"));
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
        
        fs::path NativePath(fs::path P)
        {
            P.make_preferred();
            return P;
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

        void* LoadShared(const fs::path& Path)
        {
        #if defined(_WIN32)
            return (void*)::LoadLibraryW(Path.c_str());
        #else
            return ::dlopen(Path.c_str(), RTLD_LAZY | RTLD_LOCAL);
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
            return nullptr; // POSIX: dladdr-based, added with the POSIX port
        #endif
        }

        // Best-effort recursive copy (overwriting). Returns false if any file failed to copy (e.g. a
        // destination locked by another process). Used to shadow the managed dir so the build output stays free.
        bool CopyTreeBestEffort(const fs::path& Src, const fs::path& Dst)
        {
            std::error_code Ec;
            fs::create_directories(Dst, Ec);
            bool bAllOk = true;
            for (fs::recursive_directory_iterator It(Src, fs::directory_options::skip_permission_denied, Ec), End;
                 It != End && !Ec; It.increment(Ec))
            {
                std::error_code FileEc;
                const fs::path Rel    = fs::relative(It->path(), Src, FileEc);
                const fs::path Target = Dst / Rel;
                if (It->is_directory(FileEc))
                {
                    fs::create_directories(Target, FileEc);
                }
                else if (It->is_regular_file(FileEc))
                {
                    fs::create_directories(Target.parent_path(), FileEc);
                    fs::copy_file(It->path(), Target, fs::copy_options::overwrite_existing, FileEc);
                    if (FileEc)
                    {
                        bAllOk = false;
                    }
                }
            }
            return bAllOk;
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
        const fs::path ExeDir = NativePath(fs::path(Platform::GetCurrentProcessPath().c_str())).parent_path();
        fs::path Bundled = NativePath(ExeDir / "External" / "DotNet" / "runtime" / RuntimeRid());
        if (!fs::exists(Bundled))
        {
            const fs::path Install = fs::path(Paths::GetEngineInstallDirectory().c_str());
            Bundled = NativePath(Install / "External" / "DotNet" / "runtime" / RuntimeRid());
        }
        if (!fs::exists(Bundled))
        {
            LOG_ERROR("C# scripting disabled: bundled .NET runtime not found next to the exe or under the engine install ('{}'). Run Setup.bat to extract External.", Bundled.string().c_str());
            return;
        }

        // Locate host/fxr/<version>/hostfxr.<ext> under the bundled root.
        fs::path HostfxrPath;
        std::error_code Ec;
        for (const auto& Entry : fs::directory_iterator(Bundled / "host" / "fxr", Ec))
        {
            const fs::path Candidate = Entry.path() / (FString("hostfxr") + kSharedExt).c_str();
            if (fs::exists(Candidate))
            {
                HostfxrPath = NativePath(Candidate);
                break;
            }
        }
        if (HostfxrPath.empty())
        {
            LOG_ERROR("C# scripting disabled: hostfxr not found under '{}'.", (Bundled / "host" / "fxr").string().c_str());
            return;
        }

        void* Lib = LoadShared(HostfxrPath);
        if (!Lib)
        {
            LOG_ERROR("C# scripting disabled: failed to load hostfxr at '{}'.", HostfxrPath.string().c_str());
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
        const fs::path ExePath      = NativePath(fs::path(Platform::GetCurrentProcessPath().c_str()));
        const fs::path ManagedDir   = ExePath.parent_path() / "DotNet" / "Managed";

        // In the editor, CoreCLR keeps LuminaSharp.dll (and the Roslyn assemblies the script compiler pulls
        // from the same folder) open for the whole session. That locks the build output, so a packaging
        // MSBuild can't overwrite it ("LuminaSharp.dll is in use"). Load from a shadow copy instead, leaving
        // the canonical DotNet/Managed output free to rebuild. A cooked game has no build step (and may sit in
        // a read-only dir), so it loads in place. Multi-instance: if the shadow is locked by another editor,
        // the copy fails and we fall back to loading in place.
        fs::path LoadDir = ManagedDir;
    #if WITH_EDITOR
        {
            const fs::path ShadowDir = ExePath.parent_path() / "DotNet" / "ManagedShadow";
            if (CopyTreeBestEffort(ManagedDir, ShadowDir)
                && fs::exists(ShadowDir / "LuminaSharp.dll")
                && fs::exists(ShadowDir / "LuminaSharp.runtimeconfig.json"))
            {
                LoadDir = ShadowDir;
            }
            else
            {
                LOG_WARN("C#: managed shadow copy incomplete; loading in place (packaging may report LuminaSharp.dll in use).");
            }
        }
    #endif

        const fs::path BootstrapDll = NativePath(LoadDir / "LuminaSharp.dll");
        const fs::path BootstrapCfg = NativePath(LoadDir / "LuminaSharp.runtimeconfig.json");
        if (!fs::exists(BootstrapDll) || !fs::exists(BootstrapCfg))
        {
            LOG_ERROR("C# scripting disabled: managed bootstrap missing under '{}'. Did LuminaSharp build?", LoadDir.string().c_str());
            return;
        }

        hostfxr_initialize_parameters Params{};
        Params.size = sizeof(Params);
        Params.host_path = ExePath.c_str();
        Params.dotnet_root = Bundled.c_str();

        hostfxr_handle Ctx = nullptr;
        int rc = Init(BootstrapCfg.c_str(), &Params, &Ctx);
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

        BootstrapFn Bootstrap = nullptr;
        rc = LoadAssembly(BootstrapDll.c_str(), HostType, LSTR("Bootstrap"), UNMANAGEDCALLERSONLY_METHOD, nullptr, (void**)&Bootstrap);
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

        // C# now reads reflected FString/TVector properties in place by hard-coding the EASTL layout
        // (LuminaSharp.NativeMarshal). Validate that layout against the real accessors once (Dev/Debug).
        VerifyEASTLInteropLayout();

        // Resolve the managed export resolver by name (like Bootstrap itself), then fill the engine entry cache
        // through it. There is no hand-mirrored table: each field is resolved by its own name, and a missing
        // entry simply leaves that field null (the mandatory ones are checked below).
        rc = LoadAssembly(BootstrapDll.c_str(), HostType, LSTR("ResolveManagedExport"), UNMANAGEDCALLERSONLY_METHOD, nullptr, (void**)&GResolveManagedExport);
        if (rc != 0 || GResolveManagedExport == nullptr)
        {
            LOG_ERROR("C# scripting disabled: failed to resolve managed ResolveManagedExport entry (0x{:x}).", (uint32)rc);
            return;
        }

        #define LM_RESOLVE(Field, Type) GManaged.Field = (Type)GResolveManagedExport(#Field, (int32)std::strlen(#Field))
        LM_RESOLVE(ApplyScriptProperties,  ApplyScriptPropertiesFn);
        LM_RESOLVE(ClassFind,              ManagedClassFindFn);
        LM_RESOLVE(CreateEntityScript,     CreateEntityScriptFn);
        LM_RESOLVE(CreateEntitySystem,     CreateEntitySystemFn);
        LM_RESOLVE(DestroyEntityScript,    DestroyEntityScriptFn);
        LM_RESOLVE(DestroyEntitySystem,    DestroyEntitySystemFn);
        LM_RESOLVE(DispatchInput,          DispatchInputFn);
        LM_RESOLVE(EnumerateEntityScripts, EnumerateEntityScriptsFn);
        LM_RESOLVE(EnumerateEntitySystems, EnumerateEntitySystemsFn);
        LM_RESOLVE(CreateScriptable,       CreateScriptableFn);      // optional: only when scripts ship Scriptables
        LM_RESOLVE(DestroyScriptable,      DestroyScriptableFn);
        LM_RESOLVE(EnumerateScriptables,   EnumerateScriptablesFn);
        LM_RESOLVE(FieldGet,               ManagedFieldGetFn);
        LM_RESOLVE(FieldSet,               ManagedFieldSetFn);
        LM_RESOLVE(FreeHandle,             ManagedFreeHandleFn);
        LM_RESOLVE(GetGeneration,          GetGenerationFn);
        LM_RESOLVE(GetRuntimeDiagnostics,  GetRuntimeDiagnosticsFn);
        LM_RESOLVE(GetScriptCallbackFlags, GetCallbackFlagsFn);
        LM_RESOLVE(GetScriptSchema,        GetScriptSchemaFn);
        LM_RESOLVE(GetScriptButtons,       GetScriptButtonsFn);
        LM_RESOLVE(ResolveEntityScriptName, ResolveEntityScriptNameFn);
        LM_RESOLVE(Invoke,                 ManagedInvokeFn);
        LM_RESOLVE(InvokeAssetCallback,    InvokeAssetCallbackFn);
        LM_RESOLVE(LoadScripts,            LoadScriptsFn);
        LM_RESOLVE(OnNativeDelegateDestroyed, OnNativeDelegateDestroyedFn);
        LM_RESOLVE(ObjectNew,              ManagedObjectNewFn);
        LM_RESOLVE(OnReadyScript,          OnReadyScriptFn);
        LM_RESOLVE(Shutdown,               ShutdownFn);
        LM_RESOLVE(Tick,                   TickFn);
        LM_RESOLVE(TickEntitySystem,       TickEntitySystemFn);
        LM_RESOLVE(UpdateScripts,          UpdateScriptsFn);
        LM_RESOLVE(FixedUpdateScripts,     FixedUpdateScriptsFn);   // optional: null -> fixed update skipped
        #undef LM_RESOLVE

        // The core script entries are mandatory.
        if (GManaged.LoadScripts == nullptr || GManaged.Tick == nullptr || GManaged.Shutdown == nullptr ||
            GManaged.GetGeneration == nullptr || GManaged.CreateEntityScript == nullptr ||
            GManaged.UpdateScripts == nullptr || GManaged.DestroyEntityScript == nullptr)
        {
            LOG_ERROR("C# scripting disabled: managed export resolution missing core script entry points.");
            return;
        }

        bInitialized = true;
        GOnScriptDelegateDestroyed = &NotifyManagedDelegateDestroyed;
        LOG_DISPLAY(".NET host initialized (bundled runtime: {}).", Bundled.string().c_str());
    }

    void Shutdown()
    {
        if (!bInitialized)
        {
            return;
        }

        if (GManaged.Shutdown)
        {
            GManaged.Shutdown();
        }

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
                const fs::path Marker = CompiledMarkerPath(Unit.AssemblyPath);
                if (fs::exists(Marker, Ec))
                {
                    fs::remove(NativePath(fs::path(Unit.AssemblyPath.c_str())), Ec);
                    fs::remove(Marker, Ec);
                    LOG_DISPLAY("C#: unit '{}' no longer has sources; deleted its stale compiled assembly.", Unit.Name.c_str());
                    continue;
                }
            }

            // With no .cs, the unit can still load a prebuilt managed DLL sitting at its canonical path (a
            // code-only plugin that ships a compiled assembly). With neither, there is nothing to do.
            const bool bHasPrebuilt = Bucket.Sources.empty() && !Unit.AssemblyPath.empty()
                && fs::exists(NativePath(fs::path(Unit.AssemblyPath.c_str())));
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

            Buckets.push_back(eastl::move(Bucket));
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
                    const fs::path Marker = CompiledMarkerPath(Bucket.DllPath);
                    std::error_code Ec;
                    if (!fs::exists(Marker, Ec))
                    {
                        fs::create_directories(Marker.parent_path(), Ec);
                        std::ofstream Out(Marker, std::ios::binary | std::ios::trunc);
                        if (Out)
                        {
                            static constexpr const char Text[] =
                                "Compiled from this unit's C# sources by the engine. Both this marker and the DLL are deleted when the sources are removed.\n";
                            Out.write(Text, sizeof(Text) - 1);
                        }
                    }
                }
            }
        }

        // Refresh the native generation mirror once here (the only place it can change), so the
        // per-frame tick never crosses the boundary to read it.
        GCachedGeneration = GManaged.GetGeneration ? GManaged.GetGeneration() : GCachedGeneration;

        GScriptStructs.Clear();

        // Mint a real CClass for every C# subclass of a REFLECT(Scriptable) native class, so FindObject<CClass>
        // / NewObject / editor pickers see them like any native class (minted classes are reused by name across
        // reloads; the managed instance behind each rebinds via the per-instance FScriptableBridge generation gate).
        FScriptableRegistry::RefreshMintedClasses();

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
            Out.push_back(eastl::move(Packaged));
        }
    }

    void GenerateScriptProjects()
    {
        if (!bInitialized)
        {
            return;
        }

        const fs::path ExePath = NativePath(fs::path(Platform::GetCurrentProcessPath().c_str()));
        const FString Dll((ExePath.parent_path() / "DotNet" / "Managed" / "LuminaSharp.dll").string().c_str());

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
            if (!fs::exists(NativePath(fs::path(Unit.DiskDir.c_str())), Ec))
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

    void* CreateEntityScript(FStringView TypeName, uint64 World, uint32 Entity)
    {
        if (!bInitialized || GManaged.CreateEntityScript == nullptr)
        {
            return nullptr;
        }
        return GManaged.CreateEntityScript(TypeName.data(), (int32)TypeName.size(), World, Entity);
    }

    void OnReadyScript(void* Instance)
    {
        if (bInitialized && GManaged.OnReadyScript && Instance)
        {
            GManaged.OnReadyScript(Instance);
        }
    }

    void UpdateScripts(void* const* Instances, int32 Count, float DeltaSeconds)
    {
        if (bInitialized && GManaged.UpdateScripts && Count > 0)
        {
            GManaged.UpdateScripts(Instances, Count, DeltaSeconds);
        }
    }

    void FixedUpdateScripts(void* const* Instances, int32 Count, float FixedDeltaSeconds)
    {
        if (bInitialized && GManaged.FixedUpdateScripts && Count > 0)
        {
            GManaged.FixedUpdateScripts(Instances, Count, FixedDeltaSeconds);
        }
    }

    void DestroyEntityScript(void* Instance)
    {
        if (bInitialized && GManaged.DestroyEntityScript && Instance)
        {
            GManaged.DestroyEntityScript(Instance);
        }
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

    void* CreateScriptable(FStringView TypeName, uint64 NativePtr, int32& OutOverrideFlags)
    {
        OutOverrideFlags = 0;
        if (!bInitialized || GManaged.CreateScriptable == nullptr)
        {
            return nullptr;
        }
        return GManaged.CreateScriptable(TypeName.data(), (int32)TypeName.size(), NativePtr, &OutOverrideFlags);
    }

    void DestroyScriptable(void* Instance)
    {
        if (bInitialized && GManaged.DestroyScriptable && Instance)
        {
            GManaged.DestroyScriptable(Instance);
        }
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

    void DispatchScriptInput(void* Instance, int32 Type, int32 KeyCode, int32 bMouse, int32 Mods, int32 bRepeat,
        double MouseX, double MouseY, double DeltaX, double DeltaY, double Scroll)
    {
        if (bInitialized && GManaged.DispatchInput && Instance)
        {
            GManaged.DispatchInput(Instance, Type, KeyCode, bMouse, Mods, bRepeat, MouseX, MouseY, DeltaX, DeltaY, Scroll);
        }
    }

    int32 GetScriptCallbackFlags(void* Instance)
    {
        return (bInitialized && GManaged.GetScriptCallbackFlags && Instance) ? GManaged.GetScriptCallbackFlags(Instance) : 0;
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

        TSharedPtr<Scripting::FScriptExportType> ReadType(FBlobReader& R)
        {
            auto Type = MakeShared<Scripting::FScriptExportType>();
            Type->Kind = static_cast<EPropertyTypeFlags>(R.U8());
            Type->bEntity = R.U8() != 0;
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
                        F.Type = ReadType(R);
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
                            F.Type = ReadType(R);
                            Candidate.Fields.push_back(F);
                        }
                        Type->Candidates.push_back(eastl::move(Candidate));
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
                        Out.Items.push_back(eastl::move(K));
                        Scripting::FScriptPropertyValue Val;
                        ReadValue(R, Val);
                        Out.Items.push_back(eastl::move(Val));
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

        // Captures a sink-delivered blob into a TVector<uint8> (the invoke / field-get return path).
        void LmInvokeResultSink(void* Ctx, const char* Data, int Len)
        {
            auto* Out = static_cast<TVector<uint8>*>(Ctx);
            if (Out != nullptr && Data != nullptr && Len > 0)
            {
                Out->assign(reinterpret_cast<const uint8*>(Data), reinterpret_cast<const uint8*>(Data) + Len);
            }
        }

        // One self-describing value (kind byte + payload), wire-compatible with the managed ReadBoxed.
        void WriteManagedArg(FBlobWriter& W, const FManagedValue& V)
        {
            using K = Scripting::EScriptValueKind;
            switch (V.GetKind())
            {
                case EManagedValueKind::Bool:   W.U8((uint8)K::Bool);   W.U8(V.AsBool() ? 1 : 0); break;
                case EManagedValueKind::Int:    W.U8((uint8)K::Int);    W.I64(V.AsInt64()); break;
                case EManagedValueKind::Double: W.U8((uint8)K::Double); W.F64(V.AsDouble()); break;
                case EManagedValueKind::String:
                {
                    const FString& S = V.AsString();
                    W.U8((uint8)K::String);
                    W.Str(FStringView(S.c_str(), S.size()));
                    break;
                }
                default: W.U8((uint8)K::Nil); break;
            }
        }

        // Mirror of WriteManagedArg / the managed WriteBoxed: decode one self-describing value.
        FManagedValue ReadManagedArg(FBlobReader& R)
        {
            using K = Scripting::EScriptValueKind;
            switch ((K)R.U8())
            {
                case K::Bool:   return FManagedValue(R.U8() != 0);
                case K::Int:    return FManagedValue(R.I64());
                case K::Double: return FManagedValue(R.F64());
                case K::String: return FManagedValue(R.Str());
                default:        return FManagedValue();
            }
        }

        // Shared instance/static invoke: packs args, calls the managed entry, decodes the boxed return.
        FManagedValue InvokeManaged(void* Target, bool bStatic, FStringView Method, std::initializer_list<FManagedValue> Args)
        {
            if (!bInitialized || GManaged.Invoke == nullptr || Target == nullptr)
            {
                return FManagedValue();
            }

            TVector<uint8> ArgBlob;
            FBlobWriter W{ ArgBlob };
            W.I32((int32)Args.size());
            for (const FManagedValue& A : Args)
            {
                WriteManagedArg(W, A);
            }

            TVector<uint8> RetBlob;
            const int32 Rc = GManaged.Invoke(Target, bStatic ? 1 : 0, Method.data(), (int32)Method.size(),
                ArgBlob.data(), (int32)ArgBlob.size(), reinterpret_cast<void*>(&LmInvokeResultSink), &RetBlob);
            if (Rc != 0 || RetBlob.empty())
            {
                return FManagedValue();
            }

            FBlobReader R{ RetBlob.data(), RetBlob.data() + RetBlob.size() };
            return ReadManagedArg(R);
        }
    }
    
    
    FManagedClass::FManagedClass(FStringView TypeName)
    {
        if (bInitialized && GManaged.ClassFind != nullptr && !TypeName.empty())
        {
            TypeHandle = GManaged.ClassFind(TypeName.data(), (int32)TypeName.size());
        }
    }

    FManagedClass::~FManagedClass()
    {
        if (TypeHandle != nullptr && GManaged.FreeHandle != nullptr)
        {
            GManaged.FreeHandle(TypeHandle);
        }
        TypeHandle = nullptr;
    }

    FManagedClass& FManagedClass::operator=(FManagedClass&& Other) noexcept
    {
        if (this != &Other)
        {
            if (TypeHandle != nullptr && GManaged.FreeHandle != nullptr)
            {
                GManaged.FreeHandle(TypeHandle);
            }
            TypeHandle = Other.TypeHandle;
            Other.TypeHandle = nullptr;
        }
        return *this;
    }

    FManagedObject FManagedClass::New()
    {
        if (TypeHandle == nullptr || GManaged.ObjectNew == nullptr)
        {
            return FManagedObject();
        }
        return FManagedObject(GManaged.ObjectNew(TypeHandle));
    }

    FManagedValue FManagedClass::InvokeStatic(FStringView Method, std::initializer_list<FManagedValue> Args)
    {
        return InvokeManaged(TypeHandle, true, Method, Args);
    }

    FManagedObject::~FManagedObject()
    {
        Free();
    }

    void FManagedObject::Free()
    {
        if (Handle != nullptr && GManaged.FreeHandle != nullptr)
        {
            GManaged.FreeHandle(Handle);
        }
        Handle = nullptr;
    }

    FManagedObject& FManagedObject::operator=(FManagedObject&& Other) noexcept
    {
        if (this != &Other)
        {
            Free();
            Handle = Other.Handle;
            Other.Handle = nullptr;
        }
        return *this;
    }

    FManagedValue FManagedObject::Invoke(FStringView Method, std::initializer_list<FManagedValue> Args)
    {
        return InvokeManaged(Handle, false, Method, Args);
    }

    FManagedValue FManagedObject::GetField(FStringView Name)
    {
        if (!bInitialized || GManaged.FieldGet == nullptr || Handle == nullptr)
        {
            return FManagedValue();
        }

        TVector<uint8> RetBlob;
        const int32 Rc = GManaged.FieldGet(Handle, Name.data(), (int32)Name.size(),
            reinterpret_cast<void*>(&LmInvokeResultSink), &RetBlob);
        if (Rc != 0 || RetBlob.empty())
        {
            return FManagedValue();
        }

        FBlobReader R{ RetBlob.data(), RetBlob.data() + RetBlob.size() };
        return ReadManagedArg(R);
    }

    bool FManagedObject::SetField(FStringView Name, const FManagedValue& Value)
    {
        if (!bInitialized || GManaged.FieldSet == nullptr || Handle == nullptr)
        {
            return false;
        }

        TVector<uint8> ValBlob;
        FBlobWriter W{ ValBlob };
        WriteManagedArg(W, Value);
        return GManaged.FieldSet(Handle, Name.data(), (int32)Name.size(), ValBlob.data(), (int32)ValBlob.size()) == 0;
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

            const FString Category = R.Str();
            const FString Tooltip = R.Str();
            const FString Units = R.Str();
            if (!Category.empty())
            {
                Field.Meta.Set("Category", Category);
            }
            if (!Tooltip.empty())
            {
                Field.Meta.Set("ToolTip", Tooltip);
            }
            if (!Units.empty())
            {
                Field.Meta.Set("Units", Units);
            }
            if (R.U8())
            {
                Field.Meta.Set("ClampMin", NumberToString(R.F64()));
            }
            if (R.U8())
            {
                Field.Meta.Set("ClampMax", NumberToString(R.F64()));
            }
            if (R.U8())
            {
                Field.Meta.Set("Color", FString());
            }
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

    void ApplyScriptProperties(void* Instance, const TVector<Scripting::FScriptPropertyEntry>& Values)
    {
        if (!bInitialized || GManaged.ApplyScriptProperties == nullptr || Instance == nullptr)
        {
            return;
        }

        // Serialize the values to one value blob; managed deserializes onto the live instance via reflection.
        TVector<uint8> Blob;
        FBlobWriter W{ Blob };
        W.I32((int32)Values.size());
        for (const Scripting::FScriptPropertyEntry& Entry : Values)
        {
            W.Str(FStringView(Entry.Name.c_str()));
            WriteValue(W, Entry.Value);
        }
        GManaged.ApplyScriptProperties(Instance, Blob.data(), (int32)Blob.size());
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
            OutButtons.push_back(eastl::move(Button));
        }
    }

    bool InvokeScriptButton(void* Instance, FStringView Method)
    {
        if (!bInitialized || Instance == nullptr || Method.empty())
        {
            return false;
        }
        InvokeManaged(Instance, false, Method, {});
        return true;
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
