#pragma once

#include "ModuleInterface.h"
#include "Containers/Array.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Memory/Memory.h"
#include "Memory/SmartPtr.h"


// Module ABI guard
//
// BUMP THIS whenever the engine's binary interface to modules changes: the layout or size of a type that
// crosses the boundary, an enum's values, exported signatures, or the VTABLE SHAPE of a class modules
// derive from. Adding a virtual to FEditorTool shifts every later slot, so a plugin built against the old
// header dispatches through the wrong one -- which surfaces as a crash somewhere unrelated (a garbage
// const char* reaching std::format, say) rather than as a load failure.
//
// 2 -- FEditorTool gained the GetDefaultCameraPose virtual + bIsActiveTool member.
#define LUMINA_MODULE_ABI_VERSION 2

#if defined(WITH_EDITOR) && WITH_EDITOR
    #define LUMINA_MODULE_ABI_PLATFORM "Editor"
#else
    #define LUMINA_MODULE_ABI_PLATFORM "Game"
#endif

#define LUMINA_MODULE_ABI_STR2(x) #x
#define LUMINA_MODULE_ABI_STR(x)  LUMINA_MODULE_ABI_STR2(x)

// Compile-time fingerprint, identical for the engine and any ABI-compatible module.
// Example: "LMABI/1|Development|Editor|MSC1944"
#define LUMINA_MODULE_ABI_SIGNATURE                              \
    "LMABI/" LUMINA_MODULE_ABI_STR(LUMINA_MODULE_ABI_VERSION)    \
    "|" LUMINA_CONFIGURATION_NAME                                \
    "|" LUMINA_MODULE_ABI_PLATFORM                               \
    "|MSC" LUMINA_MODULE_ABI_STR(_MSC_VER)


// IMPLEMENT_MODULE has two flavors (LUMINA_MONOLITHIC): modular exports InitializeModule for
// GetProcAddress; monolithic registers an intrusive FStaticModuleRegistration drained on lookup.
#ifdef LUMINA_MONOLITHIC

#define IMPLEMENT_MODULE(ModuleClass, ModuleName)                                           \
    namespace {                                                                             \
        Lumina::IModuleInterface* Z_LuminaStaticInit()                                      \
        {                                                                                   \
            return Lumina::Memory::New<ModuleClass>();                                      \
        }                                                                                   \
        const Lumina::FStaticModuleRegistration                                             \
            Z_LuminaStaticReg(ModuleName, &Z_LuminaStaticInit);                             \
    }

#else

#define IMPLEMENT_MODULE(ModuleClass, ModuleName)                                           \
    DECLARE_MODULE_ALLOCATOR_OVERRIDES()                                                    \
    extern "C" __declspec(dllexport) const char* LuminaModuleABISignature()                 \
    {                                                                                       \
        return LUMINA_MODULE_ABI_SIGNATURE;                                                 \
    }                                                                                       \
    extern "C" __declspec(dllexport) Lumina::IModuleInterface* InitializeModule()           \
    {                                                                                       \
        Lumina::Memory::InitializeThreadHeap();                                             \
        return Lumina::Memory::New<ModuleClass>();                                          \
    }                                                                                       \
    extern "C" __declspec(dllexport) void ShutdownModule()                                  \
    {                                                                                       \
        Lumina::Memory::ShutdownThreadHeap();                                               \
    }

#endif


namespace Lumina
{
    struct RUNTIME_API FModuleInfo
    {
        FName ModuleName;
        TUniquePtr<IModuleInterface> ModuleInterface;
        // null in monolithic builds where the module is statically linked.
        // UnloadModule must guard FreeDLLHandle on it.
        void* ModuleHandle;
    };

    using ModuleInitFunc = IModuleInterface* (*)();
    using ModuleShutdownFunc = void (*)();
    // ABI guard export (see LUMINA_MODULE_ABI_SIGNATURE); returns a static string, ABI-safe to call.
    using ModuleABIFunc = const char* (*)();

    // Monolithic-build static registration; self-links into an intrusive list during static
    // init. Ctor touches zero runtime state (static-init order across TUs is undefined).
    struct RUNTIME_API FStaticModuleRegistration
    {
        const char*                       Name;
        ModuleInitFunc                    Factory;
        FStaticModuleRegistration*        Next;

        FStaticModuleRegistration(const char* InName, ModuleInitFunc InFactory);

        // Head of the pending-registration list; lives in BSS (zero-init) so it's
        // valid before any constructor runs.
        static FStaticModuleRegistration* Head;
    };

    class FModuleManager
    {
    public:

        RUNTIME_API static FModuleManager& Get();

        // Load a module by DLL path; monolithic builds resolve via the static registry
        // (basename minus -<Config> suffix) with no filesystem touch.
        RUNTIME_API IModuleInterface* LoadModule(FStringView ModulePath);
        RUNTIME_API bool UnloadModule(FStringView ModuleName);

        void UnloadAllModules();

        // Called by FStaticModuleRegistration's ctor at file-scope init; safe before
        // FModuleManager::Get() is otherwise touched (Meyer's singleton resolves on first use).
        void AddStaticModuleFactory(const FName& Name, ModuleInitFunc Factory);

        // True if a module with this bare name is statically linked (monolithic builds).
        bool HasStaticFactory(const FName& Name) const { return FindStaticFactory(Name) != nullptr; }

        // Human-readable reason the most recent LoadModule call failed (e.g. an ABI mismatch), or empty
        // if it succeeded / failed without a recorded reason. Lets callers (the editor project loader)
        // surface a warning. Reset at the start of each LoadModule.
        RUNTIME_API const FString& GetLastLoadError() const { return LastLoadError; }

        // ImGui is a StaticLib, so each module DLL has its own ImGui/ImPlot context + allocator globals.
        // The editor calls this once its ImGui context exists (contexts passed as void* to keep this
        // header ImGui-free); it stores them and syncs every already-loaded module that opted in via
        // LUMINA_MODULE_IMGUI() (see ImGuiModule.h). Modules loaded later are synced at load time.
        RUNTIME_API void NotifyImGuiReady(void* InImGuiContext, void* InImPlotContext);


    private:

        FModuleInfo* GetOrCreateModuleInfo(const FName& ModuleName);

        // Call a loaded module's optional LuminaModuleSetupImGui export with the stored contexts.
        // No-op for statically-linked modules (null handle), modules without the hook, or before
        // NotifyImGuiReady has run.
        void SyncModuleImGui(const FModuleInfo& ModuleInfo);

        // Static-registry lookup by bare name (no config suffix); nullptr if not registered.
        ModuleInitFunc FindStaticFactory(const FName& Name) const;


    private:

        THashMap<FName, FModuleInfo> ModuleHashMap;

        // Static module factories from IMPLEMENT_MODULE in monolithic builds (linear list).
        TVector<TPair<FName, ModuleInitFunc>> StaticModuleFactories;

        // Reason the last LoadModule failed (ABI mismatch, etc.); see GetLastLoadError.
        FString LastLoadError;

        // Engine ImGui/ImPlot contexts (opaque void* to keep ImGui out of this header), null until
        // NotifyImGuiReady. Forwarded to each opted-in module's LuminaModuleSetupImGui export.
        void* ImGuiContextPtr  = nullptr;
        void* ImPlotContextPtr = nullptr;
    };
}
