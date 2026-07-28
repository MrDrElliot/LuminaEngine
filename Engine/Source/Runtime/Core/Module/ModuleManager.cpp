#include "pch.h"
#include "ModuleManager.h"
#include "ModuleInterface.h"
#include "Core/Delegates/CoreDelegates.h"
#include "Core/Templates/LuminaTemplate.h"
#include "FileSystem/FileSystem.h"
#include "Paths/Paths.h"
#include "Platform/Platform.h"
#include "Platform/Process/PlatformProcess.h"
#include <cstring>
#include "Log/Log.h"


namespace Lumina
{
    FModuleManager& FModuleManager::Get()
    {
        static FModuleManager Instance;
        return Instance;
    }

    IModuleInterface* FModuleManager::LoadModule(FStringView ModulePath)
    {
        LastLoadError.clear();

        // Reduce "Foo-Development.dll" (or an already-bare name) to the registry key "Foo".
        // VFS::FileName returns empty with no slash, so fall back to the input itself.
        FStringView FileNameView = VFS::FileName(ModulePath, true);
        FString BareName = FileNameView.empty()
            ? FString(ModulePath.data(), ModulePath.size())
            : FString(FileNameView.data(), FileNameView.size());

        // Strip an optional ".dll"/".so" if the caller passed a bare filename
        // with extension (e.g. "Foo.dll") rather than a full path.
        {
            const size_t Dot = BareName.find_last_of('.');
            if (Dot != FString::npos)
            {
                BareName.erase(Dot);
            }
        }

        const FString CfgSuffix = FString("-") + LUMINA_CONFIGURATION_NAME;
        if (BareName.size() > CfgSuffix.size()
            && BareName.substr(BareName.size() - CfgSuffix.size()) == CfgSuffix)
        {
            BareName.erase(BareName.size() - CfgSuffix.size());
        }
        const FName BareFName(BareName);

        // Monolithic / pre-registered path: no LoadLibrary, no filesystem.
        if (ModuleInitFunc Factory = FindStaticFactory(BareFName))
        {
            IModuleInterface* ModuleInterface = Factory();
            if (!ModuleInterface)
            {
                LOG_WARN("Static module factory returned null: {}", BareName);
                return nullptr;
            }

            FModuleInfo* ModuleInfo = GetOrCreateModuleInfo(BareFName);
            ModuleInfo->ModuleHandle = nullptr;
            ModuleInfo->ModuleInterface.reset(ModuleInterface);

            ModuleInterface->StartupModule();

            LOG_INFO("[Module Manager] - Successfully linked static module {}", BareName);
            FCoreDelegates::OnModuleLoaded.Broadcast(ModuleInfo);
            return ModuleInterface;
        }

        void* ModuleHandle = Platform::GetDLLHandle(StringUtils::ToWideString(ModulePath).c_str());

        if (!ModuleHandle)
        {
            LOG_WARN("Failed to load module: {}", ModulePath);
            return nullptr;
        }

        // ABI guard: a module built against an incompatible engine ABI (config, editor/game platform,
        // compiler, or a breaking change) would corrupt memory once its code runs. Read the tiny signature
        // export -- a const char* return, ABI-safe regardless of how the module was built -- and refuse the
        // module before calling InitializeModule if it doesn't match this engine's. Catches the mismatches
        // a filename can't (editor/game, toolset, ABI version) as well as a stale wrong-config DLL.
        auto ABIFunctionPtr = Platform::LumGetProcAddress<ModuleABIFunc>(ModuleHandle, "LuminaModuleABISignature");
        const char* ModuleABI = ABIFunctionPtr ? ABIFunctionPtr() : nullptr;
        if (ModuleABI == nullptr || std::strcmp(ModuleABI, LUMINA_MODULE_ABI_SIGNATURE) != 0)
        {
            LastLoadError = FString("ABI mismatch (module '")
                + (ModuleABI ? ModuleABI : "<unsigned>")
                + "' vs engine '" + LUMINA_MODULE_ABI_SIGNATURE + "')";
            LOG_ERROR("[Module Manager] - Refusing to load '{}': {}. Rebuild the project against this engine "
                      "(matching Configuration and Editor/Game platform).", ModulePath, LastLoadError);
            Platform::FreeDLLHandle(ModuleHandle);
            return nullptr;
        }

        auto InitFunctionPtr = Platform::LumGetProcAddress<ModuleInitFunc>(ModuleHandle, "InitializeModule");
        if (!InitFunctionPtr)
        {
            LastLoadError = FString("missing InitializeModule export");
            LOG_WARN("Failed to get InitializeModule export: {}", ModulePath);
            return nullptr;
        }

        IModuleInterface* ModuleInterface = InitFunctionPtr();

        if (!ModuleInterface)
        {
            LOG_WARN("Module returned null from InitializeModule(): {}", ModulePath);
            return nullptr;
        }

        FStringView ModuleName = VFS::FileName(ModulePath, true);

        // Key the map by the bare name (no "-<Config>" suffix), matching the static path above and the
        // name UnloadModule/PluginManager use (the descriptor's module name). Keying by the suffixed
        // DLL filename instead made a dynamically-loaded module impossible to find on unload -> the
        // DEBUG_ASSERT(it != end()) in UnloadModule fired at shutdown.
        FModuleInfo* ModuleInfo = GetOrCreateModuleInfo(BareFName);
        ModuleInfo->ModuleHandle = ModuleHandle;
        ModuleInfo->ModuleInterface.reset(ModuleInterface);

        ModuleInterface->StartupModule();

        LOG_INFO("[Module Manager] - Successfully loaded module {}", ModuleName);

        // If ImGui is already up (module loaded after the editor UI), sync this module's ImGui copy
        // now. Otherwise NotifyImGuiReady will catch it once the context exists. No-op without the hook.
        SyncModuleImGui(*ModuleInfo);

        FCoreDelegates::OnModuleLoaded.Broadcast(ModuleInfo);

        return ModuleInterface;
    }

    void FModuleManager::SyncModuleImGui(const FModuleInfo& ModuleInfo)
    {
        if (ModuleInfo.ModuleHandle == nullptr || ImGuiContextPtr == nullptr)
        {
            return;
        }

        using FSetupImGuiFunc = void (*)(void*, void*);
        auto Setup = Platform::LumGetProcAddress<FSetupImGuiFunc>(ModuleInfo.ModuleHandle, "LuminaModuleSetupImGui");
        if (Setup)
        {
            Setup(ImGuiContextPtr, ImPlotContextPtr);
        }
    }

    void FModuleManager::NotifyImGuiReady(void* InImGuiContext, void* InImPlotContext)
    {
        ImGuiContextPtr  = InImGuiContext;
        ImPlotContextPtr = InImPlotContext;

        for (auto& Pair : ModuleHashMap)
        {
            SyncModuleImGui(Pair.second);
        }
    }

    void FModuleManager::AddStaticModuleFactory(const FName& Name, ModuleInitFunc Factory)
    {
        StaticModuleFactories.emplace_back(Name, Factory);
    }

    // Drain pending static registrations into the FName map; called lazily on first
    // FindStaticFactory use, by which point allocators / FName pool are up.
    static void DrainStaticRegistrationsOnce(FModuleManager& Mgr)
    {
        static bool bDrained = false;
        if (bDrained) return;
        bDrained = true;

        FStaticModuleRegistration* Node = FStaticModuleRegistration::Head;
        while (Node != nullptr)
        {
            Mgr.AddStaticModuleFactory(FName(Node->Name), Node->Factory);
            Node = Node->Next;
        }
    }

    ModuleInitFunc FModuleManager::FindStaticFactory(const FName& Name) const
    {
        DrainStaticRegistrationsOnce(const_cast<FModuleManager&>(*this));
        for (const auto& Entry : StaticModuleFactories)
        {
            if (Entry.first == Name)
            {
                return Entry.second;
            }
        }
        return nullptr;
    }

    // Defined in the header struct as a static. Zero-init in BSS so it's
    // valid before any FStaticModuleRegistration ctor runs.
    FStaticModuleRegistration* FStaticModuleRegistration::Head = nullptr;

    FStaticModuleRegistration::FStaticModuleRegistration(const char* InName, ModuleInitFunc InFactory)
        : Name(InName)
        , Factory(InFactory)
        , Next(Head)
    {
        Head = this;
    }

    bool FModuleManager::UnloadModule(FStringView ModuleName)
    {
        FName ModuleFName = FName(ModuleName);
        auto it = ModuleHashMap.find(ModuleFName);
        
        DEBUG_ASSERT(it != ModuleHashMap.end());

        FModuleInfo& Info = it->second;
        DEBUG_ASSERT(Info.ModuleInterface.get());
        
        Info.ModuleInterface->ShutdownModule();

        ModuleHashMap.erase(it);
        Info.ModuleInterface.reset();
        void* ModulePtr = Info.ModuleHandle;

        // Statically-linked modules have no DLL handle; skip the
        // DLL-side teardown. The IModule's ShutdownModule above already ran.
        if (ModulePtr != nullptr)
        {
            auto ShutdownFunctionPtr = Platform::LumGetProcAddress<ModuleShutdownFunc>(ModulePtr, "ShutdownModule");
            if (ShutdownFunctionPtr)
            {
                ShutdownFunctionPtr();
            }
            Platform::FreeDLLHandle(ModulePtr);
        }

        LOG_INFO("[Module Manager] - Successfully un-loaded module {}", ModuleName);

        return true;
    }

    void FModuleManager::UnloadAllModules()
    {
        TVector<FName> Keys;
        for (const auto& Pair : ModuleHashMap)
        {
            Keys.push_back(Pair.first);
        }

        //@TODO This causes a crash with the heap.
        for (const FName& Key : Keys)
        {
            UnloadModule(Key.ToString());
        }
    }

    FModuleInfo* FModuleManager::GetOrCreateModuleInfo(const FName& ModuleName)
    {
        auto it = ModuleHashMap.find(ModuleName);

        if (it != ModuleHashMap.end())
        {
            return &it->second;
        }

        FModuleInfo NewInfo;
        NewInfo.ModuleName = ModuleName;

        ModuleHashMap.emplace(ModuleName, Move(NewInfo));

        return &ModuleHashMap[ModuleName];
    }

}
