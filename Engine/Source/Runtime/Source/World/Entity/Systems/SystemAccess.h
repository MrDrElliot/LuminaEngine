#pragma once

#include "World/ECS/Registry.h"



#include "ModuleAPI.h"
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "SystemResources.h"

namespace Lumina
{
    // Declared component/resource access for an ECS system, used to decide which systems may run
    // concurrently. Two systems conflict (must serialize) if their writes overlap, or one writes what
    // the other reads. IDs are component type id values for the component or SystemResource:: tag types.
    //
    // A system opts in by adding a static member `Access`, e.g.:
    //     static inline FSystemAccess Access = FSystemAccess{}.Write<SSkeletalMeshComponent>()
    //                                                          .Read<SAnimationGraphComponent>();
    // A system with NO Access member is treated as EXCLUSIVE (conflicts with everything → runs alone),
    // which is the safe default for anything doing structural changes, Lua, or unknown access.
    struct FSystemAccess
    {
        TVector<uint32> Writes;
        TVector<uint32> Reads;

        // One per declared component type. Creating a component pool MUTATES the registry's pool map, and
        // the registry does it lazily inside View(), Get() and GetStorage(), for reads as much as writes, so two systems
        // in the same parallel batch can be inside that dense_map at once, one of them rehashing it, and
        // the reader walks a reallocated bucket array. It does not matter whether the two want the same
        // component: the map is shared. The scheduler runs these on the tick thread before the batch, so
        // every assure() inside the batch is a pure lookup. See CWorld::TickSystems.
        TVector<void(*)(ECS::FRegistry&)> PoolAssurers;

        bool            bExclusive = false;

        template<typename... Ts>
        FSystemAccess& Write()
        {
            (Writes.push_back(static_cast<uint32>(ECS::GetComponentTypeID<Ts>())), ...);
            (AddPoolAssurer<Ts>(), ...);
            return *this;
        }

        template<typename... Ts>
        FSystemAccess& Read()
        {
            (Reads.push_back(static_cast<uint32>(ECS::GetComponentTypeID<Ts>())), ...);
            (AddPoolAssurer<Ts>(), ...);
            return *this;
        }

        template<typename T>
        void AddPoolAssurer()
        {
            if constexpr (!TIsSystemResource<T>)
            {
                PoolAssurers.push_back(+[](ECS::FRegistry& Registry) { (void)Registry.GetStorage<T>(); });
            }
        }

        static FSystemAccess Exclusive()
        {
            FSystemAccess A;
            A.bExclusive = true;
            return A;
        }

        // Sets are tiny (a handful of types), so a nested scan beats hashing.
        static bool Intersects(const TVector<uint32>& X, const TVector<uint32>& Y)
        {
            return Algo::AnyOf(X, [&Y](uint32 A) { return Algo::Contains(Y, A); });
        }

        static bool Conflicts(const FSystemAccess& A, const FSystemAccess& B)
        {
            if (A.bExclusive || B.bExclusive)
            {
                return true;
            }
            return Intersects(A.Writes, B.Writes)
                || Intersects(A.Writes, B.Reads)
                || Intersects(B.Writes, A.Reads);
        }

        // An exclusive system implicitly declares everything; otherwise the id must be in the matching set.
        bool DeclaresWrite(uint32 Id) const
        {
            if (bExclusive)
            {
                return true;
            }
            return Algo::Contains(Writes, Id);
        }

        // A write satisfies a read (you may read what you write), so check both sets.
        bool DeclaresRead(uint32 Id) const
        {
            return DeclaresWrite(Id) || Algo::Contains(Reads, Id);
        }
    };

    // Debug/Development honest-access validation. The stage scheduler binds the executing system's access on
    // the current thread for the duration of its Update (see CWorld::TickSystems); access-implying helpers
    // (transform resolve, SetEntityWorldTransform, physics, structural ECS changes) then call
    // ValidateSystemAccess to assert the system actually declared what it touched. Catches the silent
    // under-declaration race that parallel systems are prone to. Compiled to a no-op in Shipping.
    //
    // bWrite picks the required access kind. What is a human label naming the access to add (e.g.
    // "Write<STransformComponent>"). No-op when no system access is bound (i.e. called outside the scheduler,
    // such as gameplay code or editor tools) or when the bound system is exclusive (declares everything).
    // ConnectComponentAccessValidators hooks the pool signals, catching a structural write through the raw
    // registry that never reaches an FSystemContext helper.
#if defined(LE_SHIPPING)
    inline void SetExecutingSystemAccess(const FSystemAccess*) {}
    inline const FSystemAccess* GetExecutingSystemAccess() { return nullptr; }
    inline void ValidateSystemAccess(uint32, bool, const char*) {}
    inline void RegisterComponentAccessValidator(void (*)(ECS::FRegistry&)) {}
    inline void ConnectComponentAccessValidators(ECS::FRegistry&) {}
#else
    RUNTIME_API void SetExecutingSystemAccess(const FSystemAccess* Access);
    RUNTIME_API const FSystemAccess* GetExecutingSystemAccess();
    RUNTIME_API void ValidateSystemAccess(uint32 ComponentId, bool bWrite, const char* What);
    RUNTIME_API void RegisterComponentAccessValidator(void (*Connect)(ECS::FRegistry&));
    RUNTIME_API void ConnectComponentAccessValidators(ECS::FRegistry& Registry);
#endif

    // A component type id rendered for editor tooling, or NAME_None when the id is unknown.
    RUNTIME_API FName GetAccessTypeName(uint32 Id);
}
