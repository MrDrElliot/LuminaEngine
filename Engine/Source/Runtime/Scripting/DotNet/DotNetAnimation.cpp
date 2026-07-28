#include "Platform/GenericPlatform.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "World/World.h"
#include "World/Entity/Components/SimpleAnimationComponent.h"
#include "World/Entity/Components/AnimationGraphComponent.h"
#include "World/Entity/Components/BlackboardComponent.h"
#include "Scripting/DotNet/DotNetExport.h"

//================================================================================================
// World.Animation: drive an entity's animation from script (LuminaSharp.Animation). Two backends share one
// facade: SSimpleAnimationComponent (single-clip play/pause/stop/scrub) and SAnimationGraphComponent
// (named float/bool parameters that gate the graph's state machine, stored on the entity's
// SBlackboardComponent when it has one). Play auto-adds the simple component;
// every other call is a safe no-op when the relevant component is absent. The clip is a CAnimation* passed
// as a uint64 (the loaded asset handle). Game thread only.
//================================================================================================

using namespace Lumina;
using namespace Lumina::DotNet;

namespace
{
    // Where a graph parameter really lives. When the entity has a blackboard, SAnimationSystem refills the
    // VM's parameter registers from it every evaluation, so writing the graph component directly would be
    // overwritten before the next tick - the blackboard is the source of truth. Without one the graph
    // component's own parameter table is authoritative. Returns null when the parameter isn't the graph's
    // (keeping the documented "setting an undeclared parameter is a no-op" behavior).
    SBlackboardComponent* ResolveParameterStore(CWorld* World, entt::entity Entity, const FName& Name)
    {
        const SAnimationGraphComponent* Graph = World->TryGetComponent<SAnimationGraphComponent>(Entity);
        if (Graph == nullptr || !Graph->HasParameter(Name))
        {
            return nullptr;
        }
        return World->TryGetComponent<SBlackboardComponent>(Entity);
    }
}

LUMINA_DOTNET_EXPORT(void, Animation_Play)(uint64 World, uint32 Entity, void* AnimationPtr, int32 bLoop, float Speed)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return;
    }
    CAnimation* Clip = static_cast<CAnimation*>(AnimationPtr);
    SSimpleAnimationComponent& Comp = W->GetOrEmplaceComponent<SSimpleAnimationComponent>(AsEntity(Entity));
    Comp.PlayAnimation(Clip, bLoop != 0, Speed);
}

LUMINA_DOTNET_EXPORT(void, Animation_Stop)(uint64 World, uint32 Entity)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return;
    }
    if (SSimpleAnimationComponent* Comp = W->TryGetComponent<SSimpleAnimationComponent>(AsEntity(Entity)))
    {
        Comp->Stop();
    }
}

LUMINA_DOTNET_EXPORT(void, Animation_Pause)(uint64 World, uint32 Entity)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return;
    }
    if (SSimpleAnimationComponent* Comp = W->TryGetComponent<SSimpleAnimationComponent>(AsEntity(Entity)))
    {
        Comp->Pause();
    }
}

LUMINA_DOTNET_EXPORT(void, Animation_Resume)(uint64 World, uint32 Entity)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return;
    }
    if (SSimpleAnimationComponent* Comp = W->TryGetComponent<SSimpleAnimationComponent>(AsEntity(Entity)))
    {
        Comp->Resume();
    }
}

LUMINA_DOTNET_EXPORT(int32, Animation_IsPlaying)(uint64 World, uint32 Entity)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return 0;
    }
    const SSimpleAnimationComponent* Comp = W->TryGetComponent<SSimpleAnimationComponent>(AsEntity(Entity));
    return (Comp != nullptr && Comp->IsPlaying()) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(int32, Animation_IsFinished)(uint64 World, uint32 Entity)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return 0;
    }
    const SSimpleAnimationComponent* Comp = W->TryGetComponent<SSimpleAnimationComponent>(AsEntity(Entity));
    return (Comp != nullptr && Comp->IsFinished()) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(void, Animation_SetSpeed)(uint64 World, uint32 Entity, float Speed)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return;
    }
    if (SSimpleAnimationComponent* Comp = W->TryGetComponent<SSimpleAnimationComponent>(AsEntity(Entity)))
    {
        Comp->PlaybackSpeed = Speed;
    }
}

LUMINA_DOTNET_EXPORT(void, Animation_SetTime)(uint64 World, uint32 Entity, float Time)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return;
    }
    if (SSimpleAnimationComponent* Comp = W->TryGetComponent<SSimpleAnimationComponent>(AsEntity(Entity)))
    {
        Comp->CurrentTime = Time;
        Comp->bDirty = true;
    }
}

LUMINA_DOTNET_EXPORT(float, Animation_GetTime)(uint64 World, uint32 Entity)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return 0.0f;
    }
    const SSimpleAnimationComponent* Comp = W->TryGetComponent<SSimpleAnimationComponent>(AsEntity(Entity));
    return Comp != nullptr ? Comp->CurrentTime : 0.0f;
}

//~ Graph parameters (SAnimationGraphComponent). Names cross as UTF-8 (char*, len).

LUMINA_DOTNET_EXPORT(void, Animation_SetFloat)(uint64 World, uint32 Entity, const char* Name, int32 Length, float Value)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || Name == nullptr)
    {
        return;
    }
    const FName Key(FStringView(Name, (size_t)Length));
    if (SBlackboardComponent* Blackboard = ResolveParameterStore(W, AsEntity(Entity), Key))
    {
        Blackboard->SetFloat(Key, Value);
        return;
    }
    if (SAnimationGraphComponent* Comp = W->TryGetComponent<SAnimationGraphComponent>(AsEntity(Entity)))
    {
        Comp->SetFloat(Key, Value);
    }
}

LUMINA_DOTNET_EXPORT(float, Animation_GetFloat)(uint64 World, uint32 Entity, const char* Name, int32 Length, float Default)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || Name == nullptr)
    {
        return Default;
    }
    const FName Key(FStringView(Name, (size_t)Length));
    if (const SBlackboardComponent* Blackboard = ResolveParameterStore(W, AsEntity(Entity), Key))
    {
        return Blackboard->GetFloat(Key, Default);
    }
    const SAnimationGraphComponent* Comp = W->TryGetComponent<SAnimationGraphComponent>(AsEntity(Entity));
    return Comp != nullptr ? Comp->GetFloat(Key, Default) : Default;
}

LUMINA_DOTNET_EXPORT(void, Animation_SetBool)(uint64 World, uint32 Entity, const char* Name, int32 Length, int32 bValue)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || Name == nullptr)
    {
        return;
    }
    const FName Key(FStringView(Name, (size_t)Length));
    if (SBlackboardComponent* Blackboard = ResolveParameterStore(W, AsEntity(Entity), Key))
    {
        Blackboard->SetBool(Key, bValue != 0);
        return;
    }
    if (SAnimationGraphComponent* Comp = W->TryGetComponent<SAnimationGraphComponent>(AsEntity(Entity)))
    {
        Comp->SetBool(Key, bValue != 0);
    }
}

LUMINA_DOTNET_EXPORT(int32, Animation_GetBool)(uint64 World, uint32 Entity, const char* Name, int32 Length, int32 bDefault)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || Name == nullptr)
    {
        return bDefault;
    }
    const FName Key(FStringView(Name, (size_t)Length));
    if (const SBlackboardComponent* Blackboard = ResolveParameterStore(W, AsEntity(Entity), Key))
    {
        return Blackboard->GetBool(Key, bDefault != 0) ? 1 : 0;
    }
    const SAnimationGraphComponent* Comp = W->TryGetComponent<SAnimationGraphComponent>(AsEntity(Entity));
    if (Comp == nullptr)
    {
        return bDefault;
    }
    return Comp->GetBool(Key, bDefault != 0) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(int32, Animation_HasParameter)(uint64 World, uint32 Entity, const char* Name, int32 Length)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || Name == nullptr)
    {
        return 0;
    }
    const SAnimationGraphComponent* Comp = W->TryGetComponent<SAnimationGraphComponent>(AsEntity(Entity));
    return (Comp != nullptr && Comp->HasParameter(FName(FStringView(Name, (size_t)Length)))) ? 1 : 0;
}
