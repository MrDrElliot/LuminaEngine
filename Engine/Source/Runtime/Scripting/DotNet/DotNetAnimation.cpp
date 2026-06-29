#include "Platform/GenericPlatform.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "World/World.h"
#include "World/Entity/Components/SimpleAnimationComponent.h"
#include "World/Entity/Components/AnimationGraphComponent.h"
#include "Scripting/DotNet/DotNetExport.h"

//================================================================================================
// World.Animation: drive an entity's animation from script (LuminaSharp.Animation). Two backends share one
// facade: SSimpleAnimationComponent (single-clip play/pause/stop/scrub) and SAnimationGraphComponent
// (named float/bool parameters that gate the graph's state machine). Play auto-adds the simple component;
// every other call is a safe no-op when the relevant component is absent. The clip is a CAnimation* passed
// as a uint64 (the loaded asset handle). Game thread only.
//================================================================================================

using namespace Lumina;
using namespace Lumina::DotNet;

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
    if (SAnimationGraphComponent* Comp = W->TryGetComponent<SAnimationGraphComponent>(AsEntity(Entity)))
    {
        Comp->SetFloat(FName(FStringView(Name, (size_t)Length)), Value);
    }
}

LUMINA_DOTNET_EXPORT(float, Animation_GetFloat)(uint64 World, uint32 Entity, const char* Name, int32 Length, float Default)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || Name == nullptr)
    {
        return Default;
    }
    const SAnimationGraphComponent* Comp = W->TryGetComponent<SAnimationGraphComponent>(AsEntity(Entity));
    return Comp != nullptr ? Comp->GetFloat(FName(FStringView(Name, (size_t)Length)), Default) : Default;
}

LUMINA_DOTNET_EXPORT(void, Animation_SetBool)(uint64 World, uint32 Entity, const char* Name, int32 Length, int32 bValue)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || Name == nullptr)
    {
        return;
    }
    if (SAnimationGraphComponent* Comp = W->TryGetComponent<SAnimationGraphComponent>(AsEntity(Entity)))
    {
        Comp->SetBool(FName(FStringView(Name, (size_t)Length)), bValue != 0);
    }
}

LUMINA_DOTNET_EXPORT(int32, Animation_GetBool)(uint64 World, uint32 Entity, const char* Name, int32 Length, int32 bDefault)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || Name == nullptr)
    {
        return bDefault;
    }
    const SAnimationGraphComponent* Comp = W->TryGetComponent<SAnimationGraphComponent>(AsEntity(Entity));
    if (Comp == nullptr)
    {
        return bDefault;
    }
    return Comp->GetBool(FName(FStringView(Name, (size_t)Length)), bDefault != 0) ? 1 : 0;
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
