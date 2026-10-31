#include "LayoutRegistry.h"
#include "DotNetHost.h"
#include "Core/Delegates/CoreDelegates.h"
#include "Scripting/ManagedTypeRegistry.h"
#include "Scripting/ScriptSchemaCodec.h"

#include <string>
#include <cstring>
#include <cstdio>

#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Engine/Engine.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"
#include "Scripting/ScriptableObject.h"
#include "Core/Plugin/Plugin.h"
#include "Core/Plugin/PluginManager.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"
#include "Core/Profiler/GameplayProfiler.h"
#include "Paths/Paths.h"
#include "Platform/Process/PlatformProcess.h"
#include "Platform/Time/PlatformTime.h"
#include "World/World.h"
#include "World/WorldManager.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Core/Object/ManagedInstance.h"
#include "Scripting/EntityScript.h"
#include "Scripting/DotNet/DotNetExport.h"
#include "Scripting/DotNet/ExportSignature.h"
#include "Scripting/DotNet/ScriptReferences.h"
#include "Scripting/DotNet/ScriptUnits.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectCore.h"
#include "TaskSystem/ThreadedCallback.h"
#include "Tools/UI/ImGui/ImGuiX.h"   // editor toast notifications for script-compile feedback

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
    // Validates the container byte layout the managed marshal reads in place, a no-op in Shipping.
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

        // The managed host mirrors this layout exactly, so any change bumps the ABI version.
        struct FExporterTable
        {
            void (CORECLR_DELEGATE_CALLTYPE* Log)(int32 Level, const char* Utf8, int32 Len);

            // Read in place by managed code, so a log below the threshold never crosses or formats.
            const std::atomic<uint8>* LogThreshold;
            const std::atomic<bool>*  ScriptScopesActive;
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

        // A DllPath is an absolute prebuilt assembly used when there are no sources.
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
            const char*        References;    // ';'-joined absolute paths to third-party assemblies
            int32              ReferencesLen;
        };

        typedef int32 (CORECLR_DELEGATE_CALLTYPE* BootstrapFn)(const FBootstrapArgs*);
        // Native resolves this one entry by name, then uses it to look up every other managed entry.
        typedef void* (CORECLR_DELEGATE_CALLTYPE* ResolveManagedExportFn)(const char*, int32);
        typedef int32 (CORECLR_DELEGATE_CALLTYPE* LoadScriptsFn)(const FSourceAssembly*, int32);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* SetScriptCompileOptimizationFn)(int32);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* TickFn)();
        typedef void  (CORECLR_DELEGATE_CALLTYPE* ShutdownFn)();
        typedef int32 (CORECLR_DELEGATE_CALLTYPE* GetGenerationFn)();
        typedef void  (CORECLR_DELEGATE_CALLTYPE* OnWorldTeardownFn)(uint64);
        typedef int32 (CORECLR_DELEGATE_CALLTYPE* GetRuntimeDiagnosticsFn)(void*, int32);
        typedef void* (CORECLR_DELEGATE_CALLTYPE* CreateScriptableFn)(const char*, int32, uint64);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* ApplyScriptableDefaultsFn)(const char*, int32, uint64);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* GetTypeManifestFn)(void*, void*);
        typedef void  (CORECLR_DELEGATE_CALLTYPE* ManagedFreeHandleFn)(void*);
        typedef int32 (CORECLR_DELEGATE_CALLTYPE* ManagedIsHandleAliveFn)(void*);

        // Resolved by name, so a missing field fails loudly. A feature area declares its own exports with TManagedExport instead.
        struct FManagedExports
        {
            CreateScriptableFn          CreateScriptable;
            ApplyScriptableDefaultsFn   ApplyScriptableDefaults;
            GetTypeManifestFn           GetTypeManifest;
            ManagedFreeHandleFn         FreeHandle;
            ManagedIsHandleAliveFn      IsHandleAlive;
            GetGenerationFn             GetGeneration;
            OnWorldTeardownFn           OnWorldTeardown;
            GetRuntimeDiagnosticsFn     GetRuntimeDiagnostics;
            LoadScriptsFn               LoadScripts;
            SetScriptCompileOptimizationFn SetScriptCompileOptimization;
            ShutdownFn                  Shutdown;
            TickFn                      Tick;

        };

        bool                                        bInitialized = false;

        // Set by a UI trigger or a source watcher, serviced at frame start; see RequestScriptReload.
        TAtomic<bool>                               GScriptReloadRequested{ false };

        // Seconds before which a latched reload must not run, zero meaning the next frame start for an explicit request.
        TAtomic<double>                             GScriptReloadEarliest{ 0.0 };

        // Counts finished reloads whether they compiled or not, so a waiter can tell a failure from one still queued.
        TAtomic<int32>                              GScriptReloadsFinished{ 0 };
        TAtomic<int32>                              GLastScriptReloadResult{ 0 };

        // Long enough to swallow the several events one save produces, short enough to feel immediate.
        constexpr double                            kScriptReloadQuietSeconds = 0.25;

        // Signal listeners per world, since nothing else would destroy one the script never disposed.
        THashMap<CWorld*, TVector<void*>>           GSignalListeners;

        // Scripts can subscribe and dispose from parallel jobs while another world tears down.
        FMutex                                      GSignalListenersMutex;

        hostfxr_get_runtime_delegate_fn             GGetDelegate = nullptr;
        FExporterTable                              GExports{};
        int32                                       GCachedGeneration = 0;
        ResolveManagedExportFn                      GResolveManagedExport = nullptr;
        FManagedExports                             GManaged{};

        // Native function the managed side calls to write into the engine log.
        void CORECLR_DELEGATE_CALLTYPE Export_Log(int32 Level, const char* Utf8, int32 Len)
        {
            Logging::Log(static_cast<ELogLevel>(Level), "[C#] {}", FStringView(Utf8, (size_t)Len));
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

        // Managed resolves every native call from this module by export name, so it is handed over at bootstrap.
        void* GetThunkModuleHandle()
        {
        #if defined(_WIN32)
            HMODULE Module = nullptr;
            ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&Export_Log), &Module);
            return (void*)Module;
        #else
            // The image is already mapped, so this takes a reference rather than loading a second copy.
            Dl_info Info{};
            if (::dladdr(reinterpret_cast<const void*>(&Export_Log), &Info) != 0 && Info.dli_fname != nullptr)
            {
                return ::dlopen(Info.dli_fname, RTLD_LAZY | RTLD_NOLOAD);
            }
            return nullptr;
        #endif
        }

        // Returns false when any file failed to copy, such as a destination locked by another process.
        bool CopyTreeBestEffort(FStringView Src, FStringView Dst)
        {
            return Filesystem::CopyTree(Src, Dst);
        }

        // Core owns the cache but knows nothing about the managed runtime, so it calls back through here.
        void FreeManagedGCHandle(void* Handle)
        {
            if (Handle != nullptr && GManaged.FreeHandle != nullptr)
            {
                GManaged.FreeHandle(Handle);
            }
        }

        bool IsManagedGCHandleAlive(void* Handle)
        {
            return Handle != nullptr && (GManaged.IsHandleAlive == nullptr || GManaged.IsHandleAlive(Handle) != 0);
        }
    }

    void Initialize()
    {
        if (bInitialized)
        {
            return;
        }

        // A packaged game ships the runtime in its data folder, so probe that before the install dir.
        const FString ExeDir = ParentOf(Platform::GetCurrentProcessPath());

        FString Bundled = Join(Paths::GetGameDataDirectory(), FString("Runtime/") + RuntimeRid());
        if (!Filesystem::Exists(Bundled))
        {
            Bundled = Join(Paths::GetEngineInstallDirectory(), FString("External/DotNet/runtime/") + RuntimeRid());
        }
        if (!Filesystem::Exists(Bundled))
        {
            LOG_ERROR("C# scripting disabled: bundled .NET runtime not found in the game's data folder or under the engine install ('{}'). Run Setup.bat to extract External.", Bundled);
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
        const FString PackagedManagedDir = Join(Paths::GetGameDataDirectory(), "Managed");
        const FString ManagedDir = Filesystem::Exists(Join(PackagedManagedDir, "LuminaSharp.dll"))
            ? PackagedManagedDir
            : Join(ExeDir, "DotNet/Managed");

        // Loading from a shadow copy leaves the canonical output free for a packaging build to overwrite.
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
        // A runtime cannot unload, so a second start in one process gets a positive "already initialized" success code.
        int rc = Init(HostBootstrapCfg.Get(), &Params, &Ctx);
        if (rc < 0 || Ctx == nullptr)
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

        // Bootstrap registers every engine export by name rather than handing native a pointer table.
        GExports.Log = &Export_Log;
        GExports.LogThreshold = &Logging::GLevelThreshold;
        GExports.ScriptScopesActive = FGameplayProfiler::Get().GetScriptScopesActive();

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

        // Validates that layout against the real accessors once, in Dev and Debug builds.
        VerifyContainerInteropLayout();

        // Each field is resolved by its own name, and a missing entry leaves that field null.
        rc = LoadAssembly(HostBootstrapDll.Get(), HostType, LSTR("ResolveManagedExport"), UNMANAGEDCALLERSONLY_METHOD, nullptr, (void**)&GResolveManagedExport);
        if (rc != 0 || GResolveManagedExport == nullptr)
        {
            LOG_ERROR("C# scripting disabled: failed to resolve managed ResolveManagedExport entry (0x{:x}).", (uint32)rc);
            return;
        }

        #define LM_RESOLVE(Field, Type) GManaged.Field = (Type)GResolveManagedExport(#Field, (int32)std::strlen(#Field))
        LM_RESOLVE(CreateScriptable,       CreateScriptableFn);      // optional, only when scripts ship Scriptables
        LM_RESOLVE(ApplyScriptableDefaults, ApplyScriptableDefaultsFn);   // optional, initializers only
        LM_RESOLVE(GetTypeManifest,        GetTypeManifestFn);
        LM_RESOLVE(FreeHandle,             ManagedFreeHandleFn);
        LM_RESOLVE(IsHandleAlive,          ManagedIsHandleAliveFn);
        LM_RESOLVE(GetGeneration,          GetGenerationFn);
        LM_RESOLVE(OnWorldTeardown,        OnWorldTeardownFn);
        LM_RESOLVE(GetRuntimeDiagnostics,  GetRuntimeDiagnosticsFn);
        LM_RESOLVE(LoadScripts,            LoadScriptsFn);
        LM_RESOLVE(SetScriptCompileOptimization, SetScriptCompileOptimizationFn);   // optional, packaging only
        LM_RESOLVE(Shutdown,               ShutdownFn);
        LM_RESOLVE(Tick,                   TickFn);
        #undef LM_RESOLVE

        if (GManaged.LoadScripts == nullptr || GManaged.Tick == nullptr || GManaged.Shutdown == nullptr ||
            GManaged.GetGeneration == nullptr)
        {
            LOG_ERROR("C# scripting disabled: managed export resolution missing core script entry points.");
            return;
        }

        bInitialized = true;

        // Core must not know about GC handles, so hand it the free function once managed can service one.
        Lumina::ManagedInstances::SetFreeHandleFn(&FreeManagedGCHandle);
        Lumina::ManagedInstances::SetIsHandleAliveFn(&IsManagedGCHandleAlive);

        LOG_DISPLAY(".NET host initialized (bundled runtime: {}).", Bundled);
    }

    void DiscardBackgroundCompile();

    void Shutdown()
    {
        if (!bInitialized)
        {
            return;
        }

        DiscardBackgroundCompile();

        // After this the table is empty and clearing the free function makes any later set a no-op.
        Lumina::ManagedInstances::ReleaseAll();

        if (GManaged.Shutdown)
        {
            GManaged.Shutdown();
        }

        Lumina::ManagedInstances::SetFreeHandleFn(nullptr);
        Lumina::ManagedInstances::SetIsHandleAliveFn(nullptr);

        bInitialized = false;
        GExports = FExporterTable{};
        GCachedGeneration = 0;
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

    // One unit's gathered sources, which own the strings the marshaled views point at.
    struct FSourceBucket
    {
        FString                  Name;
        FString                  Deps;     // ';'-joined sibling unit names
        TVector<FGatheredSource> Sources;
        FString                  DllPath;  // emit target, or load source
        FString                  References; // ';'-joined third-party assembly paths
    };

    // Everything a load needs from disk, kept until the swap so the post-load steps can see what was built.
    struct FScriptLoadRequest
    {
        TVector<FSourceBucket> Buckets;
        size_t                 TotalFiles = 0;
        bool                   bEditorFollowups = false;
    };

    // A background compile in flight, which ProcessPendingScriptReload swaps in once managed reports it done.
    bool               GCompileInFlight = false;
    FScriptLoadRequest GCompileRequest;

    TManagedExport<int32 (*)(const FSourceAssembly*, int32)> GBeginScriptCompile("BeginScriptCompile");
    TManagedExport<int32 (*)()>                              GPollScriptCompile("PollScriptCompile");
    TManagedExport<int32 (*)()>                              GCommitScriptCompile("CommitScriptCompile");
    TManagedExport<void (*)()>                               GDiscardScriptCompile("DiscardScriptCompile");

    enum class EScriptCompileState : int32
    {
        None      = 0,
        Compiling = 1,
        Ready     = 2,
        Failed    = 3,
    };

    void GatherScriptLoad(const TVector<FScriptUnit>& UnitList, bool bEditorFollowups, FScriptLoadRequest& Out)
    {
        Out = FScriptLoadRequest{};
        Out.bEditorFollowups = bEditorFollowups;

        for (const FScriptUnit& Unit : UnitList)
        {
            FSourceBucket Bucket;
            Bucket.Name    = Unit.Name;
            Bucket.DllPath = Unit.AssemblyPath;

            GatherSourcesUnder(Unit.DiskDir, Bucket.Sources);

            // Loading the emitted DLL would resurrect every deleted class, so the marker identifies and deletes it.
            if (Bucket.Sources.empty() && !Unit.DiskDir.empty() && !Unit.AssemblyPath.empty())
            {
                const FString Marker = CompiledMarkerPath(Unit.AssemblyPath);
                if (Filesystem::Exists(Marker))
                {
                    Filesystem::RemoveFile(Unit.AssemblyPath);
                    Filesystem::RemoveFile(Marker);
                    Filesystem::RemoveFile(Unit.AssemblyPath + ".compilekey");
                    LOG_DISPLAY("C#: unit '{}' no longer has sources; deleted its stale compiled assembly.", Unit.Name.c_str());
                    continue;
                }
            }

            // With no sources a unit can still load a prebuilt assembly at its canonical path.
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

            for (size_t Index = 0; Index < Unit.References.size(); ++Index)
            {
                if (Index != 0)
                {
                    Bucket.References += ";";
                }
                Bucket.References += Unit.References[Index];
            }

            Out.Buckets.push_back(std::move(Bucket));
        }

        for (const FSourceBucket& Bucket : Out.Buckets)
        {
            Out.TotalFiles += Bucket.Sources.size();
        }
    }

    // Views into the request's strings, valid only for the duration of Call.
    template<typename TCall>
    int32 WithMarshaledUnits(const FScriptLoadRequest& Request, TCall&& Call)
    {
        TVector<TVector<FSourceFile>> PerBucketFiles;
        PerBucketFiles.resize(Request.Buckets.size());
        TVector<FSourceAssembly>      Units;
        Units.reserve(Request.Buckets.size());

        for (size_t Index = 0; Index < Request.Buckets.size(); ++Index)
        {
            const FSourceBucket&   Bucket = Request.Buckets[Index];
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
            Unit.References    = Bucket.References.c_str();
            Unit.ReferencesLen = (int32)Bucket.References.size();
            Units.push_back(Unit);
        }

        return Call(Units.empty() ? nullptr : Units.data(), (int32)Units.size());
    }

    // Releases what the outgoing generation holds, returning the generation it replaces.
    int32 PrepareGenerationSwap()
    {
        FCoreDelegates::OnScriptsWillReload.Broadcast();

        // A queued Task.Run body is user code holding a strong handle, so let it finish before the teardown.
        GTaskSystem->WaitForAll();

        // Before the teardown, since the stages are what the teardown runs through.
        Scripting::RegisterBuiltInManagedTypeStages();
        Scripting::FManagedTypeRegistry::Get().PreUnloadAll();

        // A managed handler lives in the context about to be dropped, and releasing the bindings frees the handles pinning it.
        ClearAllManagedDelegateBindings();

        // Zero until the first load completes, which is what tells a fresh start from a replacement.
        return GCachedGeneration;
    }

    int32 FinishGenerationSwap(const FScriptLoadRequest& Request, int32 Result, int32 PreviousGeneration)
    {
        const bool bEditorFollowups = Request.bEditorFollowups;
        const size_t TotalFiles = Request.TotalFiles;
        const TVector<FSourceBucket>& Buckets = Request.Buckets;

        if (Result != 0)
        {
            LOG_ERROR("C# script load/reload returned error {}.", Result);
            if (bEditorFollowups)
            {
                ImGuiX::Notifications::NotifyError("Script compile failed ({} file(s)) -- see the Output Log.", TotalFiles);
            }

            Scripting::FManagedTypeRegistry::Get().UnloadAbortedAll();
            return Result;
        }

        if (bEditorFollowups)
        {
            ImGuiX::Notifications::NotifySuccess("Recompiled {} C# script file(s).", TotalFiles);
        }

        // So a later reload tells our emitted DLL apart from an authored prebuilt assembly.
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

        // The only place it can change, so the per-frame tick never crosses the boundary to read it.
        GCachedGeneration = GManaged.GetGeneration ? GManaged.GetGeneration() : GCachedGeneration;

        // The handles are WEAK, so this exists to recycle slots rather than to make hot reload work.
        Lumina::ManagedInstances::ReleaseAll();

        // Every stage reads this one gather rather than crossing the boundary for its own copy.
        TVector<Scripting::FManagedTypeDefinition> Definitions;
        TVector<Scripting::FManagedTypeAlias> Aliases;
        GatherManagedTypeDefinitions(Definitions, Aliases);

        Scripting::FManagedTypeRegistry::Get().CompileAll(Definitions, Aliases);

        // Last, so a handler runs against the generation now live. On a first load no world holds a script yet.
        EntityScripts::NotifyScriptsReloaded(
            PreviousGeneration == 0 ? EScriptReloadReason::InitialLoad : EScriptReloadReason::Requested,
            GCachedGeneration);
        Scripting::FManagedTypeRegistry::Get().BroadcastReload(GCachedGeneration, Definitions);

        // Idempotent and editor-only, so an absent project self-heals on any reload.
        if (bEditorFollowups)
        {
            GenerateScriptProjects();
        }
        return Result;
    }

    void RecordFinishedReload(int32 Result)
    {
        GLastScriptReloadResult.store(Result, Atomic::MemoryOrderRelease);
        GScriptReloadsFinished.fetch_add(1, Atomic::MemoryOrderAcqRel);
    }

    // Blocking paths supersede a background compile, whose waiter still has to see a reload finish.
    void DiscardBackgroundCompile()
    {
        if (!GCompileInFlight)
        {
            return;
        }

        if (GDiscardScriptCompile)
        {
            GDiscardScriptCompile.Get()();
        }
        GCompileInFlight = false;
        GCompileRequest = FScriptLoadRequest{};
        RecordFinishedReload(-1);
    }

    // Returns the managed load result, where zero is success and a negative could not run.
    int32 LoadScriptUnitsCore(const TVector<FScriptUnit>& UnitList, bool bEditorFollowups)
    {
        if (!bInitialized || GManaged.LoadScripts == nullptr)
        {
            return -1;
        }

        DiscardBackgroundCompile();

        FScriptLoadRequest Request;
        GatherScriptLoad(UnitList, bEditorFollowups, Request);

        LOG_DISPLAY("C#: {} {} script unit(s), {} file(s)...",
            bEditorFollowups ? "compiling" : "loading", Request.Buckets.size(), Request.TotalFiles);

        const int32 PreviousGeneration = PrepareGenerationSwap();
        const int32 Result = WithMarshaledUnits(Request, [](const FSourceAssembly* Units, int32 Count)
        {
            return GManaged.LoadScripts(Units, Count);
        });
        return FinishGenerationSwap(Request, Result, PreviousGeneration);
    }

    void ReloadScripts()
    {
        RecordFinishedReload(LoadScriptUnitsCore(BuildScriptUnits(), /*bEditorFollowups*/true));
    }

    // Gathers sources now and hands them to a worker, so the frame that asked for the reload does not wait on Roslyn.
    bool BeginBackgroundReload()
    {
        if (!bInitialized || !GBeginScriptCompile)
        {
            return false;
        }

        GatherScriptLoad(BuildScriptUnits(), /*bEditorFollowups*/true, GCompileRequest);
        LOG_DISPLAY("C#: compiling {} script unit(s), {} file(s) in the background...",
            GCompileRequest.Buckets.size(), GCompileRequest.TotalFiles);

        const int32 Started = WithMarshaledUnits(GCompileRequest, [](const FSourceAssembly* Units, int32 Count)
        {
            return GBeginScriptCompile.Get()(Units, Count);
        });

        if (Started != 0)
        {
            GCompileRequest = FScriptLoadRequest{};
            return false;
        }

        GCompileInFlight = true;
        return true;
    }

    void FinishBackgroundReload()
    {
        const EScriptCompileState State = static_cast<EScriptCompileState>(GPollScriptCompile.Get()());
        if (State == EScriptCompileState::Compiling)
        {
            return;
        }

        GCompileInFlight = false;
        FScriptLoadRequest Request = Move(GCompileRequest);
        GCompileRequest = FScriptLoadRequest{};

        // The live generation was never touched, so a failed compile leaves every script and binding running.
        if (State != EScriptCompileState::Ready)
        {
            LOG_ERROR("C# script compile failed; the running scripts were kept.");
            ImGuiX::Notifications::NotifyError("Script compile failed ({} file(s)), see the Output Log.", Request.TotalFiles);
            RecordFinishedReload(4);
            return;
        }

        const int32 PreviousGeneration = PrepareGenerationSwap();
        const int32 Result = GCommitScriptCompile.Get()();
        RecordFinishedReload(FinishGenerationSwap(Request, Result, PreviousGeneration));
    }

    int32 GetFinishedScriptReloads()
    {
        return GScriptReloadsFinished.load(Atomic::MemoryOrderAcquire);
    }

    int32 GetLastScriptReloadResult()
    {
        return GLastScriptReloadResult.load(Atomic::MemoryOrderAcquire);
    }

    void TrackSignalListener(CWorld* World, void* Listener)
    {
        if (World != nullptr && Listener != nullptr)
        {
            FScopeLock Lock(GSignalListenersMutex);
            GSignalListeners[World].push_back(Listener);
        }
    }

    bool ForgetSignalListener(CWorld* World, void* Listener)
    {
        FScopeLock Lock(GSignalListenersMutex);
        auto It = GSignalListeners.find(World);
        if (It == GSignalListeners.end())
        {
            return false;
        }

        TVector<void*>& Listeners = It->second;
        bool bFound = false;
        for (size_t Index = 0; Index < Listeners.size(); ++Index)
        {
            if (Listeners[Index] == Listener)
            {
                Listeners[Index] = Listeners.back();
                Listeners.pop_back();
                bFound = true;
                break;
            }
        }

        if (Listeners.empty())
        {
            GSignalListeners.erase(It);
        }
        return bFound;
    }

    void NotifyWorldTeardown(CWorld* World)
    {
        if (World == nullptr)
        {
            return;
        }

        // Taken out under the lock, so a script disposing the same subscription later finds it gone instead of freeing it twice.
        TVector<void*> Listeners;
        {
            FScopeLock Lock(GSignalListenersMutex);
            auto It = GSignalListeners.find(World);
            if (It != GSignalListeners.end())
            {
                Listeners = Move(It->second);
                GSignalListeners.erase(It);
            }
        }

        // Destroying a listener releases its managed binding, and the registry is still alive for the disconnect.
        for (void* Handle : Listeners)
        {
            auto* Listener = static_cast<FManagedSignalListener*>(Handle);
            Listener->Connection.Release();
            delete Listener;
        }

        if (!bInitialized || GManaged.OnWorldTeardown == nullptr)
        {
            return;
        }

        GManaged.OnWorldTeardown(reinterpret_cast<uint64>(World));
    }

    void RequestScriptReload()
    {
        GScriptReloadRequested.store(true, Atomic::MemoryOrderRelease);
        GScriptReloadEarliest.store(0.0, Atomic::MemoryOrderRelease);
    }

    void NotifyScriptSourceChanged(FStringView Path)
    {
        if (!VFS::HasExtension(Path, ".cs"))
        {
            return;
        }

        // Pushed forward by each event, so the reload lands once after a burst settles.
        GScriptReloadEarliest.store(PlatformTime::Seconds() + kScriptReloadQuietSeconds, Atomic::MemoryOrderRelease);
        GScriptReloadRequested.store(true, Atomic::MemoryOrderRelease);
    }

    void ProcessPendingScriptReload()
    {
        // One compile at a time; a request that arrives meanwhile stays latched for the frame after the swap.
        if (GCompileInFlight)
        {
            FinishBackgroundReload();
            return;
        }

        if (!GScriptReloadRequested.load(Atomic::MemoryOrderAcquire))
        {
            return;
        }

        if (PlatformTime::Seconds() < GScriptReloadEarliest.load(Atomic::MemoryOrderAcquire))
        {
            return;
        }

        GScriptReloadRequested.store(false, Atomic::MemoryOrderRelease);

        // Without the exports, an older managed build still reloads, just on this frame.
        if (!BeginBackgroundReload())
        {
            ReloadScripts();
        }
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

        // The cooked game loads these prebuilt rather than running the compiler at boot.
        if (GManaged.SetScriptCompileOptimization != nullptr)
        {
            GManaged.SetScriptCompileOptimization(1);
        }
        ReloadScripts();

        for (const FScriptUnit& Unit : BuildScriptUnits())
        {
            if (Unit.AssemblyPath.empty())
            {
                continue; // no canonical DLL location, so there is nothing to ship
            }
            FPackagedScriptUnit Packaged;
            Packaged.Name          = Unit.Name;
            Packaged.DllSourcePath = Unit.AssemblyPath;
            Packaged.Deps          = Unit.Deps;
            Packaged.References    = Unit.References;
            Out.push_back(std::move(Packaged));
        }

        // The live generation is the optimized one until this puts a debuggable build back in the editor.
        if (GManaged.SetScriptCompileOptimization != nullptr)
        {
            GManaged.SetScriptCompileOptimization(0);
            ReloadScripts();
        }
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
    }

    void GatherManagedTypeDefinitions(TVector<Scripting::FManagedTypeDefinition>& OutTypes, TVector<Scripting::FManagedTypeAlias>& OutAliases)
    {
        OutTypes.clear();
        OutAliases.clear();
        if (!bInitialized || GManaged.GetTypeManifest == nullptr)
        {
            return;
        }

        TVector<uint8> Blob;
        GManaged.GetTypeManifest(reinterpret_cast<void*>(&LmSchemaBlobSink), &Blob);
        if (!Blob.empty())
        {
            Scripting::ParseTypeManifest(Blob, OutTypes, OutAliases);
        }
    }

    bool IsInitialized()
    {
        return bInitialized;
    }

    namespace
    {
        // Manual reload trigger; latched like every other one, so it never runs inside a draw.
        FAutoConsoleCommand GReloadScriptsCommand(
            "dotnet.reload",
            "Recompile and hot-reload all C# scripts across every mounted Scripts/ folder.",
            []() { Lumina::DotNet::RequestScriptReload(); });

        FAutoConsoleCommand GGenProjectsCommand(
            "dotnet.genprojects",
            "(Re)generate the C# IDE project files (.csproj) for every script root.",
            []() { Lumina::DotNet::GenerateScriptProjects(); });
    }
}

// Validates the managed function-pointer path, which the generated property thunks also use.
LUMINA_DOTNET_EXPORT(int, NativeSelfTest)(int A, int B)
{
    return A + B;
}

// Matches up to the build-config suffix, so any module's reflected types can carry C# bindings.
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

    // A concurrent module load fails the snapshot with ERROR_BAD_LENGTH, and a miss here is baked into a permanently null function pointer.
    HANDLE Snap = INVALID_HANDLE_VALUE;
    for (int Attempt = 0; Attempt < 16; ++Attempt)
    {
        Snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
        if (Snap != INVALID_HANDLE_VALUE)
        {
            break;
        }
        if (::GetLastError() != ERROR_BAD_LENGTH)
        {
            return nullptr;
        }
        ::Sleep(1);
    }

    if (Snap == INVALID_HANDLE_VALUE)
    {
        return nullptr;
    }

    MODULEENTRY32W Entry;
    Entry.dwSize = sizeof(Entry);
    void* Result = nullptr;
    void* MatchingExecutable = nullptr;
    const HMODULE Executable = ::GetModuleHandleW(nullptr);
    if (::Module32FirstW(Snap, &Entry))
    {
        do
        {
            if (Matches(Entry.szModule))
            {
                // A packaged game's exe carries the project's name, so the module's own library has to win over it.
                if (Entry.hModule == Executable)
                {
                    MatchingExecutable = Entry.hModule;
                    continue;
                }
                Result = Entry.hModule;
                break;
            }
        }
        while (::Module32NextW(Snap, &Entry));
    }
    ::CloseHandle(Snap);
    if (Result == nullptr)
    {
        Result = MatchingExecutable;
    }
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

LUMINA_DOTNET_SIGNATURES(
    LUMINA_DOTNET_SIG(NativeSelfTest),
    LUMINA_DOTNET_SIG(ResolveModuleHandle)
);
