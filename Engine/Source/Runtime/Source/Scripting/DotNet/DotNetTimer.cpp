#include "Platform/GenericPlatform.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Memory/SmartPtr.h"
#include "World/World.h"
#include "World/Subsystems/TimerManager.h"
#include "Scripting/DotNet/DotNetExport.h"

// The returned id is generational, so a stale one safely reports inactive after recycling.

using namespace Lumina;
using namespace Lumina::DotNet;

namespace
{
    using FTimerThunk     = void (*)(void*);
    using FTimerFreeThunk = void (*)(void*);

    struct FManagedTimerContext;

    // Cleared wholesale before a script generation unloads, since each context roots a user delegate.
    THashSet<FManagedTimerContext*> GLiveManagedTimers;

    // Native owns the managed context, so every path that destroys a timer entry also frees its GC handle.
    struct FManagedTimerContext
    {
        FTimerThunk            Fn      = nullptr;
        FTimerFreeThunk        FreeFn  = nullptr;
        void*                  Context = nullptr;
        TWeakObjectPtr<CWorld> World;
        FTimerHandle           Handle;

        FManagedTimerContext(void* InThunk, void* InFree, void* InContext, CWorld* InWorld)
            : Fn(reinterpret_cast<FTimerThunk>(InThunk))
            , FreeFn(reinterpret_cast<FTimerFreeThunk>(InFree))
            , Context(InContext)
            , World(InWorld)
        {
            GLiveManagedTimers.insert(this);
        }

        ~FManagedTimerContext()
        {
            GLiveManagedTimers.erase(this);
            Release();
        }

        LE_NO_COPYMOVE(FManagedTimerContext);

        void Release()
        {
            if (FreeFn != nullptr && Context != nullptr)
            {
                FreeFn(Context);
            }
            Detach();
        }

        // Gives ownership back to the caller, for a timer that never got scheduled.
        void Detach()
        {
            Fn      = nullptr;
            FreeFn  = nullptr;
            Context = nullptr;
        }

        void Invoke() const
        {
            if (Fn != nullptr)
            {
                Fn(Context);
            }
        }
    };

    // Shared, since a looping timer moves its callback out and back on every fire.
    FTimerManager::FTimerCallback MakeCallback(const TSharedPtr<FManagedTimerContext>& Ctx)
    {
        return [Ctx]() { Ctx->Invoke(); };
    }

    uint32 SetManagedTimer(uint64 World, entt::entity Owner, bool bHasOwner, float Rate, int32 bLoop,
        float FirstDelay, void* Thunk, void* FreeThunk, void* Context)
    {
        CWorld* W = AsWorld(World);
        if (W == nullptr || Thunk == nullptr)
        {
            return ToId(entt::null);
        }

        TSharedPtr<FManagedTimerContext> Ctx = MakeShared<FManagedTimerContext>(Thunk, FreeThunk, Context, W);

        const FTimerHandle Handle = bHasOwner
            ? W->GetTimerManager().SetTimerForEntity(Owner, Rate, MakeCallback(Ctx), bLoop != 0, FirstDelay)
            : W->GetTimerManager().SetTimer(Rate, MakeCallback(Ctx), bLoop != 0, FirstDelay);

        // Native owns the context only when a real id comes back, which is exactly what the caller frees on.
        if (Handle.Handle == entt::null)
        {
            Ctx->Detach();
            return ToId(entt::null);
        }

        Ctx->Handle = Handle;
        return ToId(Handle.Handle);
    }
}

LUMINA_DOTNET_EXPORT(uint32, Timer_Set)(uint64 World, float Rate, int32 bLoop, float FirstDelay, void* Thunk, void* FreeThunk, void* Context)
{
    return SetManagedTimer(World, entt::null, false, Rate, bLoop, FirstDelay, Thunk, FreeThunk, Context);
}

// As the plain setter, but owned by an entity so the timer clears when that entity is destroyed.
LUMINA_DOTNET_EXPORT(uint32, Timer_SetForEntity)(uint64 World, uint32 Owner, float Rate, int32 bLoop, float FirstDelay, void* Thunk, void* FreeThunk, void* Context)
{
    return SetManagedTimer(World, AsEntity(Owner), true, Rate, bLoop, FirstDelay, Thunk, FreeThunk, Context);
}

// Clears every managed timer before its generation unloads, freeing the delegates that root that context.
LUMINA_DOTNET_EXPORT(void, Timer_ClearAllManaged)()
{
    // Snapshotted, since clearing a timer destroys its context and mutates the registry.
    TVector<FManagedTimerContext*> Snapshot;
    Snapshot.reserve(GLiveManagedTimers.size());
    for (FManagedTimerContext* Ctx : GLiveManagedTimers)
    {
        Snapshot.push_back(Ctx);
    }

    for (FManagedTimerContext* Ctx : Snapshot)
    {
        if (CWorld* W = Ctx->World.Get())
        {
            FTimerHandle Handle = Ctx->Handle;
            W->GetTimerManager().ClearTimer(Handle);
        }

        // Released even when the world is gone, so a delegate never outlives the code that owns it.
        Ctx->Release();
    }
}

LUMINA_DOTNET_EXPORT(void, Timer_Clear)(uint64 World, uint32 Timer)
{
    CWorld* W = AsWorld(World);
    if (W != nullptr)
    {
        FTimerHandle Handle{ AsEntity(Timer) };
        W->GetTimerManager().ClearTimer(Handle);
    }
}

LUMINA_DOTNET_EXPORT(int32, Timer_IsActive)(uint64 World, uint32 Timer)
{
    CWorld* W = AsWorld(World);
    return (W != nullptr && W->GetTimerManager().IsTimerActive(FTimerHandle{ AsEntity(Timer) })) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(float, Timer_GetRemaining)(uint64 World, uint32 Timer)
{
    CWorld* W = AsWorld(World);
    return W != nullptr ? W->GetTimerManager().GetTimerRemaining(FTimerHandle{ AsEntity(Timer) }) : 0.0f;
}

LUMINA_DOTNET_EXPORT(float, Timer_GetElapsed)(uint64 World, uint32 Timer)
{
    CWorld* W = AsWorld(World);
    return W != nullptr ? W->GetTimerManager().GetTimerElapsed(FTimerHandle{ AsEntity(Timer) }) : 0.0f;
}

LUMINA_DOTNET_EXPORT(void, Timer_SetPaused)(uint64 World, uint32 Timer, int32 bPaused)
{
    CWorld* W = AsWorld(World);
    if (W != nullptr)
    {
        W->GetTimerManager().SetTimerPaused(FTimerHandle{ AsEntity(Timer) }, bPaused != 0);
    }
}
