#pragma once

#include "ModuleInterface.h"
#include "Containers/HashTable.h"
#include "Containers/Pair.h"
#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Memory/Memory.h"
#include "Memory/SmartPtr.h"


// BUMP whenever the binary interface to modules changes: type layout, enum values, exported
// signatures, or vtable shape. A stale plugin then crashes somewhere unrelated instead of failing to load.
#define LUMINA_MODULE_ABI_VERSION 2

#if defined(WITH_EDITOR) && WITH_EDITOR
    #define LUMINA_MODULE_ABI_PLATFORM "Editor"
#else
    #define LUMINA_MODULE_ABI_PLATFORM "Game"
#endif

#define LUMINA_MODULE_ABI_STR2(x) #x
#define LUMINA_MODULE_ABI_STR(x)  LUMINA_MODULE_ABI_STR2(x)

#if defined(_MSC_VER)
    #define LUMINA_MODULE_ABI_COMPILER "MSC" LUMINA_MODULE_ABI_STR(_MSC_VER)
#elif defined(__clang__)
    #define LUMINA_MODULE_ABI_COMPILER "CLANG" LUMINA_MODULE_ABI_STR(__clang_major__)
#elif defined(__GNUC__)
    #define LUMINA_MODULE_ABI_COMPILER "GCC" LUMINA_MODULE_ABI_STR(__GNUC__)
#else
    #define LUMINA_MODULE_ABI_COMPILER "UNKNOWNCC"
#endif

#if defined(_LIBCPP_VERSION)
    #define LUMINA_MODULE_ABI_STDLIB "LIBCXX"
#elif defined(__GLIBCXX__)
    #define LUMINA_MODULE_ABI_STDLIB "GLIBCXX" LUMINA_MODULE_ABI_STR(_GLIBCXX_USE_CXX11_ABI)
#elif defined(_MSVC_STL_VERSION)
    #define LUMINA_MODULE_ABI_STDLIB "MSSTL"
#else
    #define LUMINA_MODULE_ABI_STDLIB "UNKNOWNSTL"
#endif

// Compile-time fingerprint, identical for the engine and any ABI-compatible module.
#define LUMINA_MODULE_ABI_SIGNATURE                              \
    "LMABI/" LUMINA_MODULE_ABI_STR(LUMINA_MODULE_ABI_VERSION)    \
    "|" LUMINA_CONFIGURATION_NAME                                \
    "|" LUMINA_MODULE_ABI_PLATFORM                               \
    "|" LUMINA_MODULE_ABI_COMPILER                               \
    "|" LUMINA_MODULE_ABI_STDLIB


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
    }                                                                                       \
    static_assert(true, "IMPLEMENT_MODULE consumes the semicolon at the call site")

#else

#define IMPLEMENT_MODULE(ModuleClass, ModuleName)                                           \
    extern "C" DLL_EXPORT const char* LuminaModuleABISignature()                            \
    {                                                                                       \
        return LUMINA_MODULE_ABI_SIGNATURE;                                                 \
    }                                                                                       \
    extern "C" DLL_EXPORT Lumina::IModuleInterface* InitializeModule()                      \
    {                                                                                       \
        Lumina::Memory::InitializeThreadHeap();                                             \
        return Lumina::Memory::New<ModuleClass>();                                          \
    }                                                                                       \
    extern "C" DLL_EXPORT void ShutdownModule()                                             \
    {                                                                                       \
        Lumina::Memory::ShutdownThreadHeap();                                               \
    }                                                                                       \
    static_assert(true, "IMPLEMENT_MODULE consumes the semicolon at the call site")

#endif


namespace Lumina
{
    struct RUNTIME_API FModuleInfo
    {
        FName ModuleName;
        TUniquePtr<IModuleInterface> ModuleInterface;
        // null in monolithic builds where the module is statically linked.
        // UnloadModule must guard FreeDLLHandle on it.
        void* ModuleHandle = nullptr;
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

        // Why the most recent LoadModule failed, or empty on success. Reset at the start of each call.
        RUNTIME_API const FString& GetLastLoadError() const { return LastLoadError; }

        // ImGui is a StaticLib, so each module DLL has its own context and allocator globals.
        // Contexts are void* to keep this header ImGui-free; modules opt in via LUMINA_MODULE_IMGUI().
        RUNTIME_API void NotifyImGuiReady(void* InImGuiContext, void* InImPlotContext);


    private:

        FModuleInfo* GetOrCreateModuleInfo(const FName& ModuleName);

        // No-op for static modules, modules without the hook, or before NotifyImGuiReady has run.
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
