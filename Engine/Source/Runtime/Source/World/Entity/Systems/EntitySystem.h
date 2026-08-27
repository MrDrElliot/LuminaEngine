#pragma once

#include "World/ECS/Registry.h"


#include "SystemContext.h"
#include "SystemAccess.h"
#include "Core/Engine/Engine.h"
#include "Core/UpdateStage.h"


namespace Lumina
{

    #define ENTITY_SYSTEM( ... )\
    FUpdatePriorityList PriorityList = FUpdatePriorityList(__VA_ARGS__);
    
    using FSystemFn = void(*)(void* Self, const FSystemContext& Context);

    namespace Meta
    {
        template<typename TSystem>
        concept HasStartup = requires(TSystem Sys, const FSystemContext& Context)
        {
            { Sys.Startup(Context) } noexcept -> std::same_as<void>;
        };

        template<typename TSystem>
        concept HasUpdate = requires(TSystem Sys, const FSystemContext& Context)
        {
            { Sys.Update(Context) } noexcept -> std::same_as<void>;
        };

        template<typename TSystem>
        concept HasTeardown = requires(TSystem Sys, const FSystemContext& Context)
        {
            { Sys.Teardown(Context) } noexcept -> std::same_as<void>;
        };

        template<typename TSystem>
        concept IsSystem = HasStartup<TSystem> || HasUpdate<TSystem> || HasTeardown<TSystem>;

        // Opt-in: a system declares `static inline FSystemAccess Access = ...;` to allow concurrent
        // execution. Without it the system is treated as exclusive (serial).
        template<typename TSystem>
        concept HasAccess = requires { { TSystem::Access } -> std::convertible_to<const FSystemAccess&>; };
    }

    // One system resolved to raw function pointers once, so the per-frame dispatch is a direct call.
    struct FNativeSystemDesc
    {
        FName               Name;
        uint64              Hash = 0;
        FUpdatePriorityList Priorities;
        FSystemAccess       Access;
        FSystemFn           Startup  = nullptr;
        FSystemFn           Update   = nullptr;
        FSystemFn           Teardown = nullptr;
    };

    // Process-wide table of native systems, populated by RegisterECSSystem<T>() during startup.
    class FSystemRegistry
    {
    public:

        static RUNTIME_API FSystemRegistry& Get();

        RUNTIME_API void Register(const FNativeSystemDesc& Desc);
        const TVector<FNativeSystemDesc>& GetNativeSystems() const { return Systems; }

    private:

        TVector<FNativeSystemDesc> Systems;
    };

    namespace Meta
    {
        template<typename TSystem>
        void RegisterECSSystem()
        {
            FNativeSystemDesc Desc;
            Desc.Name = FName(TSystem::StaticStruct()->GetName().c_str());
            Desc.Hash = static_cast<uint64>(ECS::GetComponentTypeID<TSystem>());

            // Stateless system: a throwaway instance reads the in-class PriorityList initializer.
            TSystem Temp{};
            Desc.Priorities = Temp.PriorityList;

            if constexpr (HasAccess<TSystem>)
            {
                Desc.Access = TSystem::Access;
            }
            else
            {
                Desc.Access = FSystemAccess::Exclusive();
            }

            if constexpr (HasStartup<TSystem>)
            {
                Desc.Startup = [](void*, const FSystemContext& Context) noexcept { TSystem::Startup(Context); };
            }
            if constexpr (HasUpdate<TSystem>)
            {
                Desc.Update = [](void*, const FSystemContext& Context) noexcept { TSystem::Update(Context); };
            }
            if constexpr (HasTeardown<TSystem>)
            {
                Desc.Teardown = [](void*, const FSystemContext& Context) noexcept { TSystem::Teardown(Context); };
            }

            FSystemRegistry::Get().Register(Desc);
        }
    }
}
