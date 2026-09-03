#include "Platform/GenericPlatform.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Memory/SmartPtr.h"
#include "Scripting/DotNet/DotNetExport.h"
#include "Scripting/DotNet/ManagedContextRegistry.h"
#include "World/ECS/Registry.h"
#include "World/Subsystems/TweenManager.h"
#include "World/World.h"

using namespace Lumina;
using namespace Lumina::DotNet;

namespace
{
    using FTweenThunk      = void (*)(void*);
    using FTweenValueThunk = void (*)(void*, float);
    using FTweenFreeThunk  = void (*)(void*);

    struct FManagedTweenContext;

    using FTweenRegistry = TManagedContextRegistry<FManagedTweenContext>;

    struct FManagedTweenContext
    {
        void*           Fn      = nullptr;
        FTweenFreeThunk FreeFn  = nullptr;
        void*           Context = nullptr;

        FManagedTweenContext(void* InThunk, void* InFree, void* InContext)
            : Fn(InThunk)
            , FreeFn(reinterpret_cast<FTweenFreeThunk>(InFree))
            , Context(InContext)
        {
            FTweenRegistry::Add(this);
        }

        ~FManagedTweenContext()
        {
            FTweenRegistry::Remove(this);
            Release();
        }

        LE_NO_COPYMOVE(FManagedTweenContext);

        // Frees the delegate now, for a generation unloading while its tweens are still queued.
        void Release()
        {
            if (FreeFn != nullptr && Context != nullptr)
            {
                FreeFn(Context);
            }
            Fn      = nullptr;
            FreeFn  = nullptr;
            Context = nullptr;
        }

        void Invoke() const
        {
            if (Fn != nullptr)
            {
                reinterpret_cast<FTweenThunk>(Fn)(Context);
            }
        }

        void InvokeValue(float Value) const
        {
            if (Fn != nullptr)
            {
                reinterpret_cast<FTweenValueThunk>(Fn)(Context, Value);
            }
        }
    };

    FTweenManager* ManagerOf(uint64 World)
    {
        CWorld* W = AsWorld(World);
        return W != nullptr ? &W->GetTweenManager() : nullptr;
    }

    // Rebuilt per call, since the builder is just a manager pointer plus a generational id.
    bool ResolveTween(uint64 World, uint32 Id, FTween& Out)
    {
        FTweenManager* Manager = ManagerOf(World);
        if (Manager == nullptr)
        {
            return false;
        }

        FTweenHandle Handle;
        Handle.Handle = ECS::FEntity::FromPacked(Id);
        Out = FTween(Manager, Handle);
        return true;
    }
}

// Frees every tween delegate before its generation unloads, since each one roots that generation.
LUMINA_DOTNET_EXPORT(void, Tween_ClearAllManaged)()
{
    // The tween itself keeps running; only its call back into managed code goes away.
    FTweenRegistry::ForEachSnapshot([](FManagedTweenContext* Ctx) { Ctx->Release(); });
}

LUMINA_DOTNET_EXPORT(uint32, Tween_Create)(uint64 World, uint32 OwnerEntity, int32 bHasOwner)
{
    FTweenManager* Manager = ManagerOf(World);
    if (Manager == nullptr)
    {
        return ToId(ECS::NullEntity);
    }

    const FTween Tween = (bHasOwner != 0)
        ? Manager->CreateForEntity(ECS::FEntity::FromPacked(OwnerEntity))
        : Manager->Create();

    return ToId(Tween.GetHandle().Handle);
}

LUMINA_DOTNET_EXPORT(void, Tween_MoveTo)(uint64 World, uint32 Id, uint32 Entity, FVector3 Target, float Duration)
{
    FTween Tween;
    if (ResolveTween(World, Id, Tween))
    {
        Tween.MoveTo(ECS::FEntity::FromPacked(Entity), Target, Duration);
    }
}

LUMINA_DOTNET_EXPORT(void, Tween_RotateTo)(uint64 World, uint32 Id, uint32 Entity, FQuat Target, float Duration)
{
    FTween Tween;
    if (ResolveTween(World, Id, Tween))
    {
        Tween.RotateTo(ECS::FEntity::FromPacked(Entity), Target, Duration);
    }
}

LUMINA_DOTNET_EXPORT(void, Tween_ScaleTo)(uint64 World, uint32 Id, uint32 Entity, FVector3 Target, float Duration)
{
    FTween Tween;
    if (ResolveTween(World, Id, Tween))
    {
        Tween.ScaleTo(ECS::FEntity::FromPacked(Entity), Target, Duration);
    }
}

LUMINA_DOTNET_EXPORT(void, Tween_ValueTo)(uint64 World, uint32 Id, float From, float To, float Duration,
                                          void* Thunk, void* FreeThunk, void* Context)
{
    FTween Tween;
    if (!ResolveTween(World, Id, Tween) || Thunk == nullptr)
    {
        if (FreeThunk != nullptr && Context != nullptr)
        {
            reinterpret_cast<FTweenFreeThunk>(FreeThunk)(Context);
        }
        return;
    }

    TSharedPtr<FManagedTweenContext> Ctx = MakeShared<FManagedTweenContext>(Thunk, FreeThunk, Context);
    Tween.To(From, To, Duration, [Ctx](const float& Value) { Ctx->InvokeValue(Value); });
}

LUMINA_DOTNET_EXPORT(void, Tween_Interval)(uint64 World, uint32 Id, float Duration)
{
    FTween Tween;
    if (ResolveTween(World, Id, Tween))
    {
        Tween.Interval(Duration);
    }
}

LUMINA_DOTNET_EXPORT(void, Tween_Call)(uint64 World, uint32 Id, void* Thunk, void* FreeThunk, void* Context)
{
    FTween Tween;
    if (!ResolveTween(World, Id, Tween) || Thunk == nullptr)
    {
        if (FreeThunk != nullptr && Context != nullptr)
        {
            reinterpret_cast<FTweenFreeThunk>(FreeThunk)(Context);
        }
        return;
    }

    TSharedPtr<FManagedTweenContext> Ctx = MakeShared<FManagedTweenContext>(Thunk, FreeThunk, Context);
    Tween.Call([Ctx] { Ctx->Invoke(); });
}

LUMINA_DOTNET_EXPORT(void, Tween_OnFinished)(uint64 World, uint32 Id, void* Thunk, void* FreeThunk, void* Context)
{
    FTween Tween;
    if (!ResolveTween(World, Id, Tween) || Thunk == nullptr)
    {
        if (FreeThunk != nullptr && Context != nullptr)
        {
            reinterpret_cast<FTweenFreeThunk>(FreeThunk)(Context);
        }
        return;
    }

    TSharedPtr<FManagedTweenContext> Ctx = MakeShared<FManagedTweenContext>(Thunk, FreeThunk, Context);
    Tween.OnFinished([Ctx] { Ctx->Invoke(); });
}

LUMINA_DOTNET_EXPORT(void, Tween_Trans)(uint64 World, uint32 Id, int32 Transition)
{
    FTween Tween;
    if (ResolveTween(World, Id, Tween))
    {
        Tween.Trans((EEaseTransition)Transition);
    }
}

LUMINA_DOTNET_EXPORT(void, Tween_Ease)(uint64 World, uint32 Id, int32 Ease)
{
    FTween Tween;
    if (ResolveTween(World, Id, Tween))
    {
        Tween.Ease((EEaseType)Ease);
    }
}

LUMINA_DOTNET_EXPORT(void, Tween_Delay)(uint64 World, uint32 Id, float Seconds)
{
    FTween Tween;
    if (ResolveTween(World, Id, Tween))
    {
        Tween.Delay(Seconds);
    }
}

LUMINA_DOTNET_EXPORT(void, Tween_Parallel)(uint64 World, uint32 Id)
{
    FTween Tween;
    if (ResolveTween(World, Id, Tween))
    {
        Tween.Parallel();
    }
}

LUMINA_DOTNET_EXPORT(void, Tween_SetLoops)(uint64 World, uint32 Id, int32 Count)
{
    FTween Tween;
    if (ResolveTween(World, Id, Tween))
    {
        Tween.SetLoops(Count);
    }
}

LUMINA_DOTNET_EXPORT(void, Tween_SetSpeedScale)(uint64 World, uint32 Id, float Scale)
{
    FTween Tween;
    if (ResolveTween(World, Id, Tween))
    {
        Tween.SetSpeedScale(Scale);
    }
}

LUMINA_DOTNET_EXPORT(void, Tween_SetPaused)(uint64 World, uint32 Id, int32 bPaused)
{
    FTween Tween;
    if (ResolveTween(World, Id, Tween))
    {
        Tween.SetPaused(bPaused != 0);
    }
}

LUMINA_DOTNET_EXPORT(void, Tween_Kill)(uint64 World, uint32 Id)
{
    FTween Tween;
    if (ResolveTween(World, Id, Tween))
    {
        Tween.Kill();
    }
}

LUMINA_DOTNET_EXPORT(int32, Tween_IsRunning)(uint64 World, uint32 Id)
{
    FTween Tween;
    return (ResolveTween(World, Id, Tween) && Tween.IsRunning()) ? 1 : 0;
}
