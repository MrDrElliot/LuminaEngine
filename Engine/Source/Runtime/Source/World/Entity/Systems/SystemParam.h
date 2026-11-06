#pragma once

#include "ModuleAPI.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CWorld;
    struct FSystemAccess;

    namespace ECS
    {
        class FRegistry;
        class FEventQueues;
    }

    // What a system does with one of its members: binds it to the world, declares its access, asks whether it has work, and lets go of the world when the system is dropped.
    struct FSystemParamOps
    {
        void (*Bind)(void* Param, CWorld& World)                  = nullptr;
        void (*Declare)(const void* Param, FSystemAccess& Access) = nullptr;
        bool (*HasWork)(const void* Param)                        = nullptr;
        void (*Unbind)(void* Param)                               = nullptr;
    };

    // Called from a member's constructor; it registers only while that member is being built as part of a system.
    RUNTIME_API void RegisterSystemParam(void* Param, const FSystemParamOps& Ops);

    RUNTIME_API ECS::FRegistry&    GetSystemRegistry(CWorld& World);
    RUNTIME_API ECS::FEventQueues& GetSystemEvents(CWorld& World);

    // Membership is what DeclareParamMembership returned for the param, or kIndexNone when it may reach any entity.
    RUNTIME_API void DeclareParamRead(FSystemAccess& Access, uint32 TypeId, bool bComponent, int32 Membership = Constants::kIndexNone);
    RUNTIME_API void DeclareParamWrite(FSystemAccess& Access, uint32 TypeId, bool bComponent, int32 Membership = Constants::kIndexNone);

    // The components a param's entities must hold and lack, so its accesses do not conflict with ones on disjoint entities.
    RUNTIME_API int32 DeclareParamMembership(FSystemAccess& Access, const uint32* With, int32 NumWith, const uint32* Without, int32 NumWithout);
    RUNTIME_API void DeclareParamTracked(FSystemAccess& Access, uint32 TypeId);

    // A pool the param tests membership of without touching its data, created up front so a worker never has to.
    RUNTIME_API void DeclareParamPool(FSystemAccess& Access, uint32 TypeId);
    RUNTIME_API void DeclareParamSends(FSystemAccess& Access, uint32 TypeId);
}
