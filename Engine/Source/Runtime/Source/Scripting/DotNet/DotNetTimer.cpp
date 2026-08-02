#include "Platform/GenericPlatform.h"
#include "World/World.h"
#include "World/Subsystems/TimerManager.h"
#include "Scripting/DotNet/DotNetExport.h"

//================================================================================================
// World.Timers: one-shot + looping callbacks against world time (LuminaSharp.Timers). Binds the per-world
// FTimerManager. A timer's callback is a managed Action reached through a C# trampoline: the export stores
// the (Thunk, Context) pair and the native FTimerCallback simply calls Thunk(Context) when it fires, so the
// timer system stays free of any managed coupling. The returned uint32 is the FTimerHandle's entt id
// (0xFFFFFFFF on failure); it is generational, so a stale id safely reports inactive after recycling.
//================================================================================================

using namespace Lumina;
using namespace Lumina::DotNet;

namespace
{
    using FTimerThunk = void (*)(void*);

    // Wraps the managed trampoline + its context as the native timer callback. Context is a GCHandle (as a
    // pointer) to the C# callback object; the C# side owns its lifetime (one-shots self-free, loops free on
    // Clear), so this lambda never frees it.
    FTimerManager::FTimerCallback MakeCallback(void* Thunk, void* Context)
    {
        FTimerThunk Fn = reinterpret_cast<FTimerThunk>(Thunk);
        return [Fn, Context]() { Fn(Context); };
    }
}

LUMINA_DOTNET_EXPORT(uint32, Timer_Set)(uint64 World, float Rate, int32 bLoop, float FirstDelay, void* Thunk, void* Context)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || Thunk == nullptr)
    {
        return ToId(entt::null);
    }
    const FTimerHandle Handle = W->GetTimerManager().SetTimer(Rate, MakeCallback(Thunk, Context), bLoop != 0, FirstDelay);
    return ToId(Handle.Handle);
}

// As Timer_Set, but owned by Owner: the timer is auto-cleared when that entity is destroyed.
LUMINA_DOTNET_EXPORT(uint32, Timer_SetForEntity)(uint64 World, uint32 Owner, float Rate, int32 bLoop, float FirstDelay, void* Thunk, void* Context)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || Thunk == nullptr)
    {
        return ToId(entt::null);
    }
    const FTimerHandle Handle = W->GetTimerManager().SetTimerForEntity(AsEntity(Owner), Rate, MakeCallback(Thunk, Context), bLoop != 0, FirstDelay);
    return ToId(Handle.Handle);
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
