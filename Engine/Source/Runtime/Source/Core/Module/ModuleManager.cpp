#include "RuntimePCH.h"
#include "ModuleManager.h"
#include "ModuleInterface.h"
#include "Core/Delegates/CoreDelegates.h"
#include "Core/Object/ObjectBase.h"
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

        // VFS::FileName returns empty with no slash, so fall back to the input itself.
        FStringView FileNameView = VFS::FileName(ModulePath, true);
        FString BareName = FileNameView.empty()
            ? FString(ModulePath.data(), ModulePath.size())
            : FString(FileNameView.data(), FileNameView.size());

        // Covers a caller passing a bare filename with an extension rather than a full path.
        {
            const size_t Dot = BareName.find_last_of('.');
            if (Dot != FString::npos)
            {
                BareName.erase(Dot);
            }
        }

        const FString BinarySuffix = LUMINA_BINARY_SUFFIX;
        if (BareName.size() > BinarySuffix.size()
            && BareName.substr(BareName.size() - BinarySuffix.size()) == BinarySuffix)
        {
            BareName.erase(BareName.size() - BinarySuffix.size());
        }

        // Leaving the prefix keys the module differently from what PluginManager unloads, firing the assert.
        const FStringView LibPrefix = LUMINA_SHAREDLIB_PREFIX_NAME;
        if (!LibPrefix.empty()
            && BareName.size() > LibPrefix.size()
            && BareName.substr(0, LibPrefix.size()) == FString(LibPrefix.data(), LibPrefix.size()))
        {
            BareName.erase(0, LibPrefix.size());
        }

        const FName BareFName(BareName);

        // The monolithic path uses no LoadLibrary and touches no filesystem.
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

        // Static initializers queue reflected types here; a later refusal must discard them before freeing.
        const FDeferredRegistrationSnapshot RegistrationSnapshot = SnapshotDeferredRegistrations();

        const FString ModulePathStr(ModulePath.data(), ModulePath.size());
        void* ModuleHandle = Platform::GetDLLHandle(UTF8_TO_TCHAR(ModulePathStr.c_str()));

        if (!ModuleHandle)
        {
            // Recorded so LoadProject can tell no C++ module from a module that refused to load.
            LastLoadError = FString("the module or one of its dependencies could not be loaded "
                                    "(see the LoadLibrary error above; a linked plugin or module "
                                    "may be missing or built for another configuration)");
            LOG_ERROR("[Module Manager] - Failed to load module '{}': {}", ModulePath, LastLoadError);
            return nullptr;
        }

        auto RejectModule = [&]
        {
            RollbackDeferredRegistrations(RegistrationSnapshot);
            Platform::FreeDLLHandle(ModuleHandle);
        };


        auto ABIFunctionPtr = Platform::LumGetProcAddress<ModuleABIFunc>(ModuleHandle, "LuminaModuleABISignature");
        const char* ModuleABI = ABIFunctionPtr ? ABIFunctionPtr() : nullptr;
        if (ModuleABI == nullptr || std::strcmp(ModuleABI, LUMINA_MODULE_ABI_SIGNATURE) != 0)
        {
            LastLoadError = FString("ABI mismatch (module '")
                + (ModuleABI ? ModuleABI : "<unsigned>")
                + "' vs engine '" + LUMINA_MODULE_ABI_SIGNATURE + "')";
            LOG_ERROR("[Module Manager] - Refusing to load '{}': {}. Rebuild the project against this engine "
                      "(matching Configuration and Editor/Game platform).", ModulePath, LastLoadError);
            RejectModule();
            return nullptr;
        }

        auto InitFunctionPtr = Platform::LumGetProcAddress<ModuleInitFunc>(ModuleHandle, "InitializeModule");
        if (!InitFunctionPtr)
        {
            LastLoadError = FString("missing InitializeModule export");
            LOG_WARN("Failed to get InitializeModule export: {}", ModulePath);
            RejectModule();
            return nullptr;
        }

        IModuleInterface* ModuleInterface = InitFunctionPtr();

        if (!ModuleInterface)
        {
            LastLoadError = FString("InitializeModule() returned null");
            LOG_WARN("Module returned null from InitializeModule(): {}", ModulePath);
            RejectModule();
            return nullptr;
        }

        // A decorated key is unfindable on unload and fires the assert at shutdown.
        FModuleInfo* ModuleInfo = GetOrCreateModuleInfo(BareFName);
        ModuleInfo->ModuleHandle = ModuleHandle;
        ModuleInfo->ModuleInterface.reset(ModuleInterface);

        ModuleInterface->StartupModule();

        // They differ on any platform with a library prefix, and logging the filename hid a key mismatch.
        LOG_INFO("[Module Manager] - Successfully loaded module {}", BareName);

        // Otherwise NotifyImGuiReady catches it once the context exists, and it is a no-op without the hook.
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

    // Called lazily on first use, by which point the allocators and FName pool are up.
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

    // Zero-initialized in BSS so it is valid before any registration constructor runs.
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
        if (it == ModuleHashMap.end())
        {
            return false;
        }

        DEBUG_ASSERT(it->second.ModuleInterface.get());

        if (it->second.ModuleInterface)
        {
            it->second.ModuleInterface->ShutdownModule();
        }

        // Re-found, since ShutdownModule may have inserted into the registry and invalidated the iterator.
        it = ModuleHashMap.find(ModuleFName);
        if (it == ModuleHashMap.end())
        {
            return true;
        }

        // Both must leave the node before erase frees it, or the read double-frees and jumps through a garbage handle.
        TUniquePtr<IModuleInterface> ModuleInterface = Move(it->second.ModuleInterface);
        void* ModulePtr = it->second.ModuleHandle;

        ModuleHashMap.erase(it);

        ModuleInterface.reset();

        // A statically-linked module has no DLL handle, and its ShutdownModule already ran.
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
