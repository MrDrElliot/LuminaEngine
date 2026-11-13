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
#include "Platform/Filesystem/FileHelper.h"
#include <cstring>
#include "Log/Log.h"
#include "Core/Json/Json.h"


namespace Lumina
{
    namespace
    {
        // LuminaBuildTool writes one per binaries directory and suffix, naming the engine BuildId each module was linked against.
        FString ReadModuleBuildId(FStringView BinaryPath, FStringView ModuleName)
        {
            const FString ManifestPath = Paths::Parent(BinaryPath) + "/Lumina" LUMINA_BINARY_SUFFIX ".modules";
            FString Json;
            if (!FileHelper::LoadFileIntoString(Json, ManifestPath))
            {
                return {};
            }

            FJsonValue Manifest;
            if (!FJsonValue::Parse(Json.View(), Manifest))
            {
                return {};
            }

            const FJsonValue& Entry = Manifest["Modules"][ModuleName];
            return Entry.IsString() ? FString(Entry.AsString()) : FString();
        }

        const FString& GetEngineBuildId()
        {
            static const FString BuildId = ReadModuleBuildId(Platform::GetRuntimeModulePath(), "Runtime");
            return BuildId;
        }
    }

    FModuleManager& FModuleManager::Get()
    {
        static FModuleManager Instance;
        return Instance;
    }

    bool FModuleManager::AddDLLSearchPath(const FString& Directory)
    {
        return DLLSearchPaths.WithLock([&](TVector<FString>& Paths)
        {
            if (Paths.Contains(Directory))
            {
                return false;
            }

            Paths.Add(Directory);
            return true;
        });
    }

    TVector<FString> FModuleManager::GetDLLSearchPaths() const
    {
        return DLLSearchPaths.WithLock([](const TVector<FString>& Paths) { return Paths; });
    }

    IModuleInterface* FModuleManager::LoadModule(FStringView ModulePath)
    {
        LastLoadError.Reset();

        // VFS::FileName returns empty with no slash, so fall back to the input itself.
        FStringView FileNameView = VFS::FileName(ModulePath, true);
        FString BareName = FileNameView.IsEmpty()
            ? FString(ModulePath.GetData(), ModulePath.Len())
            : FString(FileNameView.GetData(), FileNameView.Len());

        // Covers a caller passing a bare filename with an extension rather than a full path.
        {
            const int32 Dot = BareName.FindLastOf('.');
            if (Dot != Constants::kIndexNone)
            {
                BareName.LeftInline(Dot);
            }
        }

        const FString BinarySuffix = LUMINA_BINARY_SUFFIX;
        if (BareName.Len() > BinarySuffix.Len()
            && BareName.Mid(BareName.Len() - BinarySuffix.Len()) == BinarySuffix)
        {
            BareName.LeftChopInline(BinarySuffix.Len());
        }

        // Leaving the prefix keys the module differently from what PluginManager unloads, firing the assert.
        const FStringView LibPrefix = LUMINA_SHAREDLIB_PREFIX_NAME;
        if (!LibPrefix.IsEmpty()
            && BareName.Len() > LibPrefix.Len()
            && BareName.Mid(0, LibPrefix.Len()) == FString(LibPrefix.GetData(), LibPrefix.Len()))
        {
            BareName.RemoveAt(0, LibPrefix.Len());
        }

        const FName BareFName(BareName);

        // Reloading would strand the first handle, skip its shutdown, and run StartupModule a second time.
        if (const auto Loaded = ModuleHashMap.Find(BareFName);
            Loaded != nullptr && Loaded->ModuleInterface)
        {
            return Loaded->ModuleInterface.get();
        }

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

        // Loading queues the module's type table, which a refusal must drop before the code it points into is freed.
        const int32 PendingTypesSnapshot = SnapshotPendingTypes();

        const FString ModulePathStr(ModulePath.GetData(), ModulePath.Len());
        void* ModuleHandle = Platform::GetDLLHandle(UTF8_TO_TCHAR(*ModulePathStr));

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
            RollbackPendingTypes(PendingTypesSnapshot);
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

        // An engine without a manifest was not linked by LuminaBuildTool, so it has nothing to compare against.
        if (const FString& EngineBuildId = GetEngineBuildId(); !EngineBuildId.IsEmpty())
        {
            const FString ModuleBuildId = ReadModuleBuildId(ModulePath, BareName);
            if (ModuleBuildId != EngineBuildId)
            {
                LastLoadError = FString("built against engine BuildId '")
                    + (ModuleBuildId.IsEmpty() ? FString("<none>") : ModuleBuildId)
                    + "', but the engine is '" + EngineBuildId + "'";
                LOG_ERROR("[Module Manager] - Refusing to load '{}': {}. Rebuild the project against this engine.", ModulePath, LastLoadError);
                RejectModule();
                return nullptr;
            }
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

        for (auto& [Name, Info] : ModuleHashMap)
        {
            SyncModuleImGui(Info);
        }
    }

    void FModuleManager::AddStaticModuleFactory(const FName& Name, ModuleInitFunc Factory)
    {
        StaticModuleFactories.Emplace(Name, Factory);
    }

    ModuleInitFunc FModuleManager::FindStaticFactory(const FName& Name) const
    {
        // Drained lazily on first use, by which point the allocators and FName pool are up.
        if (!bStaticFactoriesDrained)
        {
            bStaticFactoriesDrained = true;

            FStaticModuleRegistration* Node = FStaticModuleRegistration::Head;
            while (Node != nullptr)
            {
                const_cast<FModuleManager&>(*this).AddStaticModuleFactory(FName(Node->Name), Node->Factory);
                Node = Node->Next;
            }
        }
        for (const auto& [FactoryName, Factory] : StaticModuleFactories)
        {
            if (FactoryName == Name)
            {
                return Factory;
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
        auto it = ModuleHashMap.Find(ModuleFName);

        DEBUG_ASSERT(it != nullptr);
        if (it == nullptr)
        {
            return false;
        }

        DEBUG_ASSERT(it->ModuleInterface.get());

        if (it->ModuleInterface)
        {
            it->ModuleInterface->ShutdownModule();
        }

        // Re-found, since ShutdownModule may have inserted into the registry and invalidated the iterator.
        it = ModuleHashMap.Find(ModuleFName);
        if (it == nullptr)
        {
            return true;
        }

        // Both must leave the node before Remove frees it, or the read double-frees and jumps through a garbage handle.
        TUniquePtr<IModuleInterface> ModuleInterface = Move(it->ModuleInterface);
        void* ModulePtr = it->ModuleHandle;

        ModuleHashMap.Remove(ModuleFName);

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
        ModuleHashMap.GenerateKeyArray(Keys);

        for (const FName& Key : Keys)
        {
            UnloadModule(Key.ToString());
        }
    }

    FModuleInfo* FModuleManager::GetOrCreateModuleInfo(const FName& ModuleName)
    {
        auto it = ModuleHashMap.Find(ModuleName);

        if (it != nullptr)
        {
            return &(*it);
        }

        FModuleInfo NewInfo;
        NewInfo.ModuleName = ModuleName;

        ModuleHashMap.TryAdd(ModuleName, Move(NewInfo));

        return &ModuleHashMap[ModuleName];
    }

}
