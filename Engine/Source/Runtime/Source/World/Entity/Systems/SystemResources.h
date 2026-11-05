#pragma once

#include "World/ECS/Registry.h"
#include "World/Entity/Systems/ResourceRegistry.h"
#include "World/Entity/Systems/SystemParam.h"

#include <type_traits>

namespace Lumina::ECS
{
    // A world singleton derives from this, so systems declare it like a component without it ever getting a pool.
    struct FResource {};

    template<typename T>
    concept CResource = std::is_base_of_v<FResource, T>;
}

namespace Lumina::Meta
{
    // Emitted for every reflected FResource, so a script finds it by name and reaches the world's instance.
    template<ECS::CResource T>
    void RegisterResourceMeta()
    {
        ECS::FResourceTypeRegistry::Get().Register(ECS::FResourceType
        {
            .Struct       = T::StaticStruct(),
            .TypeId       = static_cast<uint32>(ECS::GetComponentTypeID<T>()),
            .GetOrEmplace = [](ECS::FRegistry& Registry) -> void* { return &Registry.Ctx().template GetOrEmplace<T>(); },
            .Find         = [](ECS::FRegistry& Registry) -> void* { return Registry.Ctx().template Find<T>(); },
        });
    }
}

namespace Lumina
{
    // A system member reading the world's T, created on the main thread when the system is set up.
    template<ECS::CResource T>
    class TRes
    {
    public:

        TRes() { RegisterSystemParam(this, GetOps()); }

        NODISCARD const T& Get() const { return std::as_const(*Registry).Ctx().template Get<T>(); }
        NODISCARD const T& operator*() const { return Get(); }
        NODISCARD const T* operator->() const { return &Get(); }

    private:

        ECS::FRegistry* Registry = nullptr;

        static const FSystemParamOps& GetOps()
        {
            static const FSystemParamOps Ops
            {
                .Bind = [](void* Param, CWorld& World)
                {
                    ECS::FRegistry& Bound = GetSystemRegistry(World);
                    Bound.Ctx().template GetOrEmplace<T>();
                    static_cast<TRes*>(Param)->Registry = &Bound;
                },
                .Declare = [](const void*, FSystemAccess& Access) { DeclareParamRead(Access, static_cast<uint32>(ECS::GetComponentTypeID<T>()), false); },
            };
            return Ops;
        }
    };

    // A system member writing the world's T; only one system writing it runs at a time.
    template<ECS::CResource T>
    class TResMut
    {
    public:

        TResMut() { RegisterSystemParam(this, GetOps()); }

        NODISCARD T& Get() const { return Registry->Ctx().template Get<T>(); }
        NODISCARD T& operator*() const { return Get(); }
        NODISCARD T* operator->() const { return &Get(); }

    private:

        ECS::FRegistry* Registry = nullptr;

        static const FSystemParamOps& GetOps()
        {
            static const FSystemParamOps Ops
            {
                .Bind = [](void* Param, CWorld& World)
                {
                    ECS::FRegistry& Bound = GetSystemRegistry(World);
                    Bound.Ctx().template GetOrEmplace<T>();
                    static_cast<TResMut*>(Param)->Registry = &Bound;
                },
                .Declare = [](const void*, FSystemAccess& Access) { DeclareParamWrite(Access, static_cast<uint32>(ECS::GetComponentTypeID<T>()), false); },
            };
            return Ops;
        }
    };
}
