#include "Platform/GenericPlatform.h"
#include "Scripting/DotNet/LayoutRegistry.h"
#include "Containers/String.h"
#include "Containers/Name.h"
#include "Core/Object/Class.h"
#include "World/World.h"
#include "World/WorldContext.h"
#include "Physics/PhysicsScene.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Systems/SystemContext.h"
#include "World/Entity/Systems/NavMeshSystem.h"
#include "World/Entity/Systems/CameraSystem.h"
#include "Scripting/EntityScript.h"
#include "Scripting/EntityScript.h"
#include "World/Entity/Components/RelationshipComponent.h"
#include "AI/Navigation/NavTypes.h"
#include "GameplayTags/GameplayTagRegistry.h"
#include "GameplayTags/GameplayTagComponent.h"
#include "Core/Engine/Engine.h"
#include "Core/Engine/EngineURL.h"
#include "Core/Profiler/GameplayProfiler.h"
#include "Scripting/DotNet/DotNetExport.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "UI/RmlUiBridge.h"
#include "Input/InputActionMap.h"
#include "Input/InputQuery.h"
#include "Input/InputViewport.h"
#include "Input/InputContext.h"
#include "Input/InputMode.h"
#include "Events/MouseCodes.h"

// Hand-written native -> C# gameplay bindings: the World / Physics / Debug / Net / Navigation surface a
// scripter needs but the Reflector can't auto-generate (CWorld and IPhysicsScene gameplay calls aren't
// reflected FUNCTION()s). Each export uses LUMINA_DOTNET_EXPORT (DotNetExport.h) to stamp the flat
// `extern "C" RUNTIME_API LuminaSharp_<Domain>_<Op>` ABI, resolved on the C# side by a `delegate*` field
// (NativeBindings.Resolve). The matching C# facades live in LuminaSharp/{World,Physics,Debug,Networking,
// AI/Navigation}. World is an opaque CWorld* (uint64), Entity an entt id (uint32) - the same convention the
// component ops use. Game thread only.

using namespace Lumina;
using namespace Lumina::DotNet;   // AsWorld / AsEntity / ToId

// Blittable raycast result mirrored by LuminaSharp.RaycastHit's wire struct. bHit == 0 means no hit.
struct FLmRayHit
{
    int32    bHit;
    uint32   Entity;
    int64    BodyID;
    FVector3 Location;
    FVector3 Normal;
    float    Distance;
    float    Fraction;
    int32    BoneIndex;   // ragdoll per-bone bodies only; -1 otherwise
};
LE_REGISTER_LAYOUT("RaycastHitWire", FLmRayHit);

//================================================================================================
// World (FName-keyed spawn/find + ECS::Utils rotation/scale. The entity transform/time/destroy/count
// methods are GENERATED from CWorld's FUNCTION() declarations now that the Reflector marshals
// entt::entity - see World.generated.{cpp,cs}. Only the not-yet-reflectable surface lives here.)
//================================================================================================

LUMINA_DOTNET_EXPORT(int32, World_IsValidEntity)(uint64 World, uint32 Entity)
{
    CWorld* W = AsWorld(World);
    return (W != nullptr && W->IsValidEntity(AsEntity(Entity))) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(FQuat, World_GetRotation)(uint64 World, uint32 Entity)
{
    CWorld* W = AsWorld(World);
    return W ? ECS::Utils::GetEntityRotation(ECS::GetWorldRegistry(*W), AsEntity(Entity)) : FQuat();
}

//================================================================================================
// Game (engine-level session ops)
//================================================================================================

// Deferred level switch; URL forms per FURL::Parse: "/Game/Maps/Foo", "/Game/Maps/Foo?listen?port=7777",
// or "host:port" to connect. The swap runs at the next FrameStart, so this is safe mid-tick.
LUMINA_DOTNET_EXPORT(void, Game_OpenLevel)(const char* Url, int32 UrlLen)
{
    if (GEngine != nullptr && Url != nullptr && UrlLen > 0)
    {
        GEngine->OpenLevel(FURL::Parse(FStringView(Url, (size_t)UrlLen)));
    }
}

// Ends the PIE session in the editor; exits the process in a packaged game. Safe mid-tick (deferred both ways).
LUMINA_DOTNET_EXPORT(void, Game_Quit)()
{
    if (GEngine != nullptr)
    {
        GEngine->RequestExitGame();
    }
}

//~ Entity-script API over the unified CEntityScript path. Everything is keyed on the script's CClass, so the
//~ same calls find a C++ script and a C# one -- the C# type's full name IS its minted class's name.

// Attaches a script of the named class to an entity. Returns the CEntityScript* (C# wraps it via
// Wrapper<T>.ForObject, so the managed instance is the canonical one for that object).
LUMINA_DOTNET_EXPORT(void*, AddEntityScript)(uint64 World, uint32 Entity, const char* ClassName, int32 ClassLen)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || ClassName == nullptr)
    {
        return nullptr;
    }
    CClass* ScriptClass = FindObject<CClass>(FName(ClassName, (size_t)ClassLen));
    return EntityScripts::Attach(ECS::GetWorldRegistry(*W), AsEntity(Entity), ScriptClass);
}

// The first script on the entity whose class IS-A the named class, or null.
LUMINA_DOTNET_EXPORT(void*, FindEntityScript)(uint64 World, uint32 Entity, const char* ClassName, int32 ClassLen)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || ClassName == nullptr)
    {
        return nullptr;
    }
    CClass* ScriptClass = FindObject<CClass>(FName(ClassName, (size_t)ClassLen));
    return EntityScripts::Find(ECS::GetWorldRegistry(*W), AsEntity(Entity), ScriptClass);
}

// Every script on the entity whose class IS-A the named class, written into a caller buffer. Returns the total
// count, so a caller that under-sized its buffer can retry (mirrors the two-pass string protocol).
LUMINA_DOTNET_EXPORT(int32, FindEntityScripts)(uint64 World, uint32 Entity, const char* ClassName, int32 ClassLen,
    void** OutScripts, int32 Capacity)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || ClassName == nullptr)
    {
        return 0;
    }
    CClass* ScriptClass = FindObject<CClass>(FName(ClassName, (size_t)ClassLen));

    TVector<CEntityScript*> Found;
    EntityScripts::FindAll(ECS::GetWorldRegistry(*W), AsEntity(Entity), ScriptClass, Found);

    const int32 Count = (int32)Found.size();
    if (OutScripts != nullptr)
    {
        for (int32 Index = 0; Index < Count && Index < Capacity; ++Index)
        {
            OutScripts[Index] = Found[Index];
        }
    }
    return Count;
}

// Removes the slot holding the given instance handle, destroying the managed instance.
LUMINA_DOTNET_EXPORT(void, RemoveEntityScript)(uint64 World, uint32 Entity, void* Instance)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || Instance == nullptr)
    {
        return;
    }
    EntityScripts::Remove(ECS::GetWorldRegistry(*W), AsEntity(Entity), static_cast<CEntityScript*>(Instance));
}

LUMINA_DOTNET_EXPORT(FVector3, World_GetScale)(uint64 World, uint32 Entity)
{
    CWorld* W = AsWorld(World);
    return W ? ECS::Utils::GetEntityScale(ECS::GetWorldRegistry(*W), AsEntity(Entity)) : FVector3(1.0f);
}

LUMINA_DOTNET_EXPORT(void, World_SetScale)(uint64 World, uint32 Entity, FVector3 Scale)
{
    if (CWorld* W = AsWorld(World))
    {
        ECS::Utils::SetEntityScale(ECS::GetWorldRegistry(*W), AsEntity(Entity), Scale);
    }
}

LUMINA_DOTNET_EXPORT(void, World_SetActiveCamera)(uint64 World, uint32 Entity)
{
    if (CWorld* W = AsWorld(World))
    {
        W->SetActiveCamera(AsEntity(Entity));
    }
}

// Scene-graph traversal for World.Messages SendUp/SendDown. Parent/first-child/next-sibling read straight
// from FRelationshipComponent so the C# bus can walk ancestors and descendants. entt::null -> 0 (Entity.Null).
LUMINA_DOTNET_EXPORT(uint32, World_GetParentEntity)(uint64 World, uint32 Entity)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return ToId(entt::null);
    }
    const FRelationshipComponent* Rel = W->TryGetComponent<FRelationshipComponent>(AsEntity(Entity));
    return ToId(Rel ? Rel->Parent : entt::null);
}

LUMINA_DOTNET_EXPORT(uint32, World_GetFirstChildEntity)(uint64 World, uint32 Entity)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return ToId(entt::null);
    }
    const FRelationshipComponent* Rel = W->TryGetComponent<FRelationshipComponent>(AsEntity(Entity));
    return ToId(Rel ? Rel->First : entt::null);
}

LUMINA_DOTNET_EXPORT(uint32, World_GetNextSiblingEntity)(uint64 World, uint32 Entity)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return ToId(entt::null);
    }
    const FRelationshipComponent* Rel = W->TryGetComponent<FRelationshipComponent>(AsEntity(Entity));
    return ToId(Rel ? Rel->Next : entt::null);
}

// Batched scene-graph traversal for SendUp/SendDown: one crossing returns the whole chain/subtree instead of a
// P/Invoke per hop. Both write [Entity, ...] into OutIds (up to Max) and return the TOTAL count; if it exceeds
// Max the caller re-fetches with a larger buffer. The relationship graph is engine-maintained (acyclic).
LUMINA_DOTNET_EXPORT(int32, World_GetAncestorChain)(uint64 World, uint32 Entity, uint32* OutIds, int32 Max)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return 0;
    }
    int32 Count = 0;
    for (entt::entity Cur = AsEntity(Entity); Cur != entt::null; )
    {
        if (Count < Max)
        {
            OutIds[Count] = ToId(Cur);
        }
        ++Count;
        const FRelationshipComponent* Rel = W->TryGetComponent<FRelationshipComponent>(Cur);
        Cur = Rel ? Rel->Parent : entt::null;
    }
    return Count;
}

LUMINA_DOTNET_EXPORT(int32, World_GetSubtree)(uint64 World, uint32 Entity, uint32* OutIds, int32 Max)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return 0;
    }
    int32 Count = 0;
    TVector<entt::entity> Stack;
    Stack.push_back(AsEntity(Entity));
    while (!Stack.empty())
    {
        entt::entity Node = Stack.back();
        Stack.pop_back();
        if (Count < Max)
        {
            OutIds[Count] = ToId(Node);
        }
        ++Count;
        const FRelationshipComponent* Rel = W->TryGetComponent<FRelationshipComponent>(Node);
        for (entt::entity Child = Rel ? Rel->First : entt::null; Child != entt::null; )
        {
            Stack.push_back(Child);
            const FRelationshipComponent* CRel = W->TryGetComponent<FRelationshipComponent>(Child);
            Child = CRel ? CRel->Next : entt::null;
        }
    }
    return Count;
}

//================================================================================================
// Camera (World.Camera) -- additive camera shake on the active view. Multiple shakes sum; each Play
// returns a handle to stop it. Game thread only.
//================================================================================================

// Blittable mirror of LuminaSharp.CameraShakeWire (2 FVector3 + 4 float, no padding).
struct FLmCameraShake
{
    FVector3 LocationAmplitude;
    FVector3 RotationAmplitude;
    float    Frequency;
    float    Duration;
    float    BlendInTime;
    float    BlendOutTime;
};
LE_REGISTER_LAYOUT("FCameraShakeWire", FLmCameraShake);

LUMINA_DOTNET_EXPORT(uint32, Camera_PlayShake)(uint64 World, FLmCameraShake Wire)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return 0;
    }

    FCameraShakeParams P;
    P.LocationAmplitude = Wire.LocationAmplitude;
    P.RotationAmplitude = Wire.RotationAmplitude;
    P.Frequency         = Wire.Frequency;
    P.Duration          = Wire.Duration;
    P.BlendInTime       = Wire.BlendInTime;
    P.BlendOutTime      = Wire.BlendOutTime;
    return SCameraSystem::PlayCameraShake(ECS::GetWorldRegistry(*W), P);
}

LUMINA_DOTNET_EXPORT(void, Camera_StopShake)(uint64 World, uint32 Handle)
{
    if (CWorld* W = AsWorld(World)) { SCameraSystem::StopCameraShake(ECS::GetWorldRegistry(*W), Handle); }
}

LUMINA_DOTNET_EXPORT(void, Camera_StopAllShakes)(uint64 World)
{
    if (CWorld* W = AsWorld(World)) { SCameraSystem::StopAllCameraShakes(ECS::GetWorldRegistry(*W)); }
}

//================================================================================================
// Physics (entity-keyed; the scene resolves the body)
//================================================================================================

LUMINA_DOTNET_EXPORT(FLmRayHit, Physics_Raycast)(uint64 World, FVector3 Start, FVector3 End, uint32 IgnoreEntity)
{
    FLmRayHit Hit{};
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return Hit;
    }

    SRayCastSettings Settings;
    Settings.Start = Start;
    Settings.End = End;
    if (IgnoreEntity != ToId(entt::null))
    {
        if (Physics::IPhysicsScene* Scene = W->GetPhysicsScene())
        {
            const uint32 BodyID = Scene->GetEntityBodyID(AsEntity(IgnoreEntity));
            if (BodyID != 0xFFFFFFFFu)
            {
                Settings.IgnoreBodies.push_back(BodyID);
            }
        }
    }

    TOptional<SRayResult> Result = W->CastRay(Settings);
    if (Result.has_value())
    {
        const SRayResult& R = Result.value();
        Hit.bHit = 1;
        Hit.Entity = R.Entity;
        Hit.BodyID = R.BodyID;
        Hit.Location = R.Location;
        Hit.Normal = R.Normal;
        Hit.Distance = R.Distance;
        Hit.Fraction = R.Fraction;
        Hit.BoneIndex = R.BoneIndex;
    }
    return Hit;
}

namespace
{
    FORCEINLINE Physics::IPhysicsScene* SceneOf(uint64 World)
    {
        CWorld* W = AsWorld(World);
        return W ? W->GetPhysicsScene() : nullptr;
    }
}

LUMINA_DOTNET_EXPORT(void, Physics_AddForce)(uint64 World, uint32 Entity, FVector3 Force)
{
    if (Physics::IPhysicsScene* S = SceneOf(World)) { S->AddForce(AsEntity(Entity), Force); }
}

LUMINA_DOTNET_EXPORT(void, Physics_AddImpulse)(uint64 World, uint32 Entity, FVector3 Impulse)
{
    if (Physics::IPhysicsScene* S = SceneOf(World)) { S->AddImpulse(AsEntity(Entity), Impulse); }
}

LUMINA_DOTNET_EXPORT(void, Physics_AddTorque)(uint64 World, uint32 Entity, FVector3 Torque)
{
    if (Physics::IPhysicsScene* S = SceneOf(World)) { S->AddTorque(AsEntity(Entity), Torque); }
}

LUMINA_DOTNET_EXPORT(void, Physics_AddAngularImpulse)(uint64 World, uint32 Entity, FVector3 AngularImpulse)
{
    if (Physics::IPhysicsScene* S = SceneOf(World)) { S->AddAngularImpulse(AsEntity(Entity), AngularImpulse); }
}

LUMINA_DOTNET_EXPORT(void, Physics_AddForceAtPosition)(uint64 World, uint32 Entity, FVector3 Force, FVector3 Position)
{
    if (Physics::IPhysicsScene* S = SceneOf(World)) { S->AddForceAtPosition(AsEntity(Entity), Force, Position); }
}

LUMINA_DOTNET_EXPORT(void, Physics_AddImpulseAtPosition)(uint64 World, uint32 Entity, FVector3 Impulse, FVector3 Position)
{
    if (Physics::IPhysicsScene* S = SceneOf(World)) { S->AddImpulseAtPosition(AsEntity(Entity), Impulse, Position); }
}

LUMINA_DOTNET_EXPORT(void, Physics_SetLinearVelocity)(uint64 World, uint32 Entity, FVector3 Velocity)
{
    if (Physics::IPhysicsScene* S = SceneOf(World)) { S->SetLinearVelocity(AsEntity(Entity), Velocity); }
}

LUMINA_DOTNET_EXPORT(FVector3, Physics_GetLinearVelocity)(uint64 World, uint32 Entity)
{
    Physics::IPhysicsScene* S = SceneOf(World);
    return S ? S->GetLinearVelocity(AsEntity(Entity)) : FVector3(0.0f);
}

LUMINA_DOTNET_EXPORT(void, Physics_SetAngularVelocity)(uint64 World, uint32 Entity, FVector3 Velocity)
{
    if (Physics::IPhysicsScene* S = SceneOf(World)) { S->SetAngularVelocity(AsEntity(Entity), Velocity); }
}

LUMINA_DOTNET_EXPORT(FVector3, Physics_GetAngularVelocity)(uint64 World, uint32 Entity)
{
    Physics::IPhysicsScene* S = SceneOf(World);
    return S ? S->GetAngularVelocity(AsEntity(Entity)) : FVector3(0.0f);
}

LUMINA_DOTNET_EXPORT(FVector3, Physics_GetBodyPosition)(uint64 World, uint32 Entity)
{
    Physics::IPhysicsScene* S = SceneOf(World);
    return S ? S->GetBodyPosition(AsEntity(Entity)) : FVector3(0.0f);
}

LUMINA_DOTNET_EXPORT(FQuat, Physics_GetBodyRotation)(uint64 World, uint32 Entity)
{
    Physics::IPhysicsScene* S = SceneOf(World);
    return S ? S->GetBodyRotation(AsEntity(Entity)) : FQuat();
}

LUMINA_DOTNET_EXPORT(FVector3, Physics_GetVelocityAtPoint)(uint64 World, uint32 Entity, FVector3 Point)
{
    Physics::IPhysicsScene* S = SceneOf(World);
    return S ? S->GetVelocityAtPoint(AsEntity(Entity), Point) : FVector3(0.0f);
}

LUMINA_DOTNET_EXPORT(FVector3, Physics_GetCenterOfMass)(uint64 World, uint32 Entity)
{
    Physics::IPhysicsScene* S = SceneOf(World);
    return S ? S->GetCenterOfMass(AsEntity(Entity)) : FVector3(0.0f);
}

LUMINA_DOTNET_EXPORT(void, Physics_SetGravityFactor)(uint64 World, uint32 Entity, float Factor)
{
    if (Physics::IPhysicsScene* S = SceneOf(World)) { S->SetGravityFactor(AsEntity(Entity), Factor); }
}

LUMINA_DOTNET_EXPORT(uint32, Physics_GetBodyId)(uint64 World, uint32 Entity)
{
    Physics::IPhysicsScene* S = SceneOf(World);
    return S ? S->GetEntityBodyID(AsEntity(Entity)) : 0xFFFFFFFFu;
}

LUMINA_DOTNET_EXPORT(void, Physics_ActivateBody)(uint64 World, uint32 Entity)
{
    if (Physics::IPhysicsScene* S = SceneOf(World))
    {
        const uint32 BodyID = S->GetEntityBodyID(AsEntity(Entity));
        if (BodyID != 0xFFFFFFFFu) { S->ActivateBody(BodyID); }
    }
}

LUMINA_DOTNET_EXPORT(void, Physics_DeactivateBody)(uint64 World, uint32 Entity)
{
    if (Physics::IPhysicsScene* S = SceneOf(World))
    {
        const uint32 BodyID = S->GetEntityBodyID(AsEntity(Entity));
        if (BodyID != 0xFFFFFFFFu) { S->DeactivateBody(BodyID); }
    }
}

// Collect every rigid body constructed until the matching End, then insert them with one broadphase
// prepare/finalize instead of one per body. Nests; game thread only. See LuminaSharp.Physics.Batch().
LUMINA_DOTNET_EXPORT(void, Physics_BeginBodyBatch)(uint64 World)
{
    if (Physics::IPhysicsScene* S = SceneOf(World)) { S->BeginBodyBatch(); }
}

LUMINA_DOTNET_EXPORT(void, Physics_EndBodyBatch)(uint64 World)
{
    if (Physics::IPhysicsScene* S = SceneOf(World)) { S->EndBodyBatch(); }
}

namespace
{
    // entt::entity is a uint32-backed enum, so a C# entity-id buffer IS the query's output buffer.
    static_assert(sizeof(entt::entity) == sizeof(uint32), "Entity id buffers are aliased for overlap queries");
    FORCEINLINE TSpan<entt::entity> AsEntitySpan(uint32* Entities, int32 Count)
    {
        return TSpan<entt::entity>(reinterpret_cast<entt::entity*>(Entities), (size_t)Count);
    }

    // Resolve one entity to its body id and stage it as an ignore list (empty if it has no body).
    template<typename TContainer>
    FORCEINLINE void StageIgnore(Physics::IPhysicsScene* Scene, uint32 IgnoreEntity, TContainer& Out)
    {
        if (Scene && IgnoreEntity != ToId(entt::null))
        {
            const uint32 BodyID = Scene->GetEntityBodyID(AsEntity(IgnoreEntity));
            if (BodyID != 0xFFFFFFFFu) { Out.push_back(BodyID); }
        }
    }
}

// Fills OutEntities (entt ids) with distinct entities whose bodies overlap the sphere; returns the count
// written (clamped to Max). IgnoreEntity excludes one entity (e.g. the querier). See LuminaSharp.Physics.
LUMINA_DOTNET_EXPORT(int32, Physics_OverlapSphere)(uint64 World, FVector3 Center, float Radius, uint32 IgnoreEntity, uint32* OutEntities, int32 Max)
{
    Physics::IPhysicsScene* S = SceneOf(World);
    if (S == nullptr || Max <= 0)
    {
        return 0;
    }
    TFixedVector<uint32, MaxInlineIgnoreBodies> Ignore;
    StageIgnore(S, IgnoreEntity, Ignore);

    return S->OverlapSphere(Center, Radius, Ignore, AsEntitySpan(OutEntities, Max));
}

LUMINA_DOTNET_EXPORT(int32, Physics_OverlapBox)(uint64 World, FVector3 Center, FVector3 HalfExtents, FQuat Rotation, uint32 IgnoreEntity, uint32* OutEntities, int32 Max)
{
    Physics::IPhysicsScene* S = SceneOf(World);
    if (S == nullptr || Max <= 0)
    {
        return 0;
    }
    TFixedVector<uint32, MaxInlineIgnoreBodies> Ignore;
    StageIgnore(S, IgnoreEntity, Ignore);

    return S->OverlapBox(Center, HalfExtents, Rotation, Ignore, AsEntitySpan(OutEntities, Max));
}

// Sphere sweep from Start to End; fills OutHits (blittable FLmRayHit) sorted near-to-far; returns the count
// written (clamped to Max). IgnoreEntity excludes one entity's body.
LUMINA_DOTNET_EXPORT(int32, Physics_SphereCast)(uint64 World, FVector3 Start, FVector3 End, float Radius, uint32 IgnoreEntity, FLmRayHit* OutHits, int32 Max)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || Max <= 0)
    {
        return 0;
    }

    SSphereCastSettings Settings;
    Settings.Start  = Start;
    Settings.End    = End;
    Settings.Radius = Radius;
    StageIgnore(W->GetPhysicsScene(), IgnoreEntity, Settings.IgnoreBodies);

    TVector<SRayResult> Hits;
    W->CastSphere(Settings, Hits);

    int32 Count = (int32)Hits.size();
    if (Count > Max) { Count = Max; }
    for (int32 i = 0; i < Count; ++i)
    {
        const SRayResult& R = Hits[i];
        FLmRayHit& H = OutHits[i];
        H.bHit     = 1;
        H.Entity   = R.Entity;
        H.BodyID   = R.BodyID;
        H.Location = R.Location;
        H.Normal   = R.Normal;
        H.Distance = R.Distance;
        H.Fraction = R.Fraction;
        H.BoneIndex = R.BoneIndex;
    }
    return Count;
}

// Blittable mirror of LuminaSharp.ConstraintDescWire: a joint creation request. Field order/sizes must
// match the C# struct byte for byte (all 4-byte scalars + 12-byte FVector3, no padding).
struct FLmConstraintDesc
{
    int32    Type;
    uint32   BodyA;
    uint32   BodyB;
    FVector3 Anchor;
    FVector3 Axis;
    FVector3 AnchorB;
    float    MinLimit;
    float    MaxLimit;
    float    HalfConeAngle;
    int32    bHasLimits;
    float    LimitFrequency;
    float    LimitDamping;
    float    MaxFriction;
    float    MotorFrequency;
    float    MotorDamping;
    float    MotorForceLimit;
    float    MotorTorqueLimit;
    float    BreakForce;
};
LE_REGISTER_LAYOUT("FConstraintDescWire", FLmConstraintDesc);

LUMINA_DOTNET_EXPORT(uint32, Physics_CreateConstraint)(uint64 World, FLmConstraintDesc Desc)
{
    Physics::IPhysicsScene* S = SceneOf(World);
    if (S == nullptr)
    {
        return 0;
    }

    Physics::FConstraintDesc D;
    D.Type             = (Lumina::EPhysicsConstraintType)Desc.Type;
    D.BodyA            = Desc.BodyA == ToId(entt::null) ? entt::null : AsEntity(Desc.BodyA);
    D.BodyB            = Desc.BodyB == ToId(entt::null) ? entt::null : AsEntity(Desc.BodyB);
    D.Anchor           = Desc.Anchor;
    D.Axis             = Desc.Axis;
    D.AnchorB          = Desc.AnchorB;
    D.MinLimit         = Desc.MinLimit;
    D.MaxLimit         = Desc.MaxLimit;
    D.HalfConeAngle    = Desc.HalfConeAngle;
    D.bHasLimits       = Desc.bHasLimits != 0;
    D.LimitFrequency   = Desc.LimitFrequency;
    D.LimitDamping     = Desc.LimitDamping;
    D.MaxFriction      = Desc.MaxFriction;
    D.MotorFrequency   = Desc.MotorFrequency;
    D.MotorDamping     = Desc.MotorDamping;
    D.MotorForceLimit  = Desc.MotorForceLimit;
    D.MotorTorqueLimit = Desc.MotorTorqueLimit;
    D.BreakForce       = Desc.BreakForce;
    return S->CreateConstraint(D);
}

LUMINA_DOTNET_EXPORT(void, Physics_DestroyConstraint)(uint64 World, uint32 ConstraintID)
{
    if (Physics::IPhysicsScene* S = SceneOf(World)) { S->DestroyConstraint(ConstraintID); }
}

LUMINA_DOTNET_EXPORT(void, Physics_SetConstraintEnabled)(uint64 World, uint32 ConstraintID, int32 bEnabled)
{
    if (Physics::IPhysicsScene* S = SceneOf(World)) { S->SetConstraintEnabled(ConstraintID, bEnabled != 0); }
}

LUMINA_DOTNET_EXPORT(void, Physics_SetConstraintMotor)(uint64 World, uint32 ConstraintID, int32 Mode, float Target)
{
    if (Physics::IPhysicsScene* S = SceneOf(World)) { S->SetConstraintMotor(ConstraintID, (Physics::EConstraintMotorMode)Mode, Target); }
}

LUMINA_DOTNET_EXPORT(int32, Physics_IsConstraintBroken)(uint64 World, uint32 ConstraintID)
{
    Physics::IPhysicsScene* S = SceneOf(World);
    return (S != nullptr && S->IsConstraintBroken(ConstraintID)) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(void, Physics_SetSurfaceVelocity)(uint64 World, uint32 Entity, FVector3 Linear, FVector3 Angular)
{
    if (Physics::IPhysicsScene* S = SceneOf(World)) { S->SetSurfaceVelocity(AsEntity(Entity), Linear, Angular); }
}

// Current driven value of a joint: Hinge angle (radians) or Slider position (meters); 0 for other types.
LUMINA_DOTNET_EXPORT(float, Physics_GetConstraintValue)(uint64 World, uint32 ConstraintID)
{
    Physics::IPhysicsScene* S = SceneOf(World);
    return S ? S->GetConstraintValue(ConstraintID) : 0.0f;
}

// Whether the entity's body is awake (active) vs asleep. 0 if asleep or it has no body.
LUMINA_DOTNET_EXPORT(int32, Physics_IsAwake)(uint64 World, uint32 Entity)
{
    Physics::IPhysicsScene* S = SceneOf(World);
    if (S == nullptr)
    {
        return 0;
    }
    const uint32 BodyID = S->GetEntityBodyID(AsEntity(Entity));
    return (BodyID != 0xFFFFFFFFu && S->IsBodyActive(BodyID)) ? 1 : 0;
}

// Every body the ray crosses, sorted near-to-far (penetrating bullets / all-targets line trace). Fills
// OutHits (blittable FLmRayHit); returns the count written (clamped to Max). IgnoreEntity excludes one body.
LUMINA_DOTNET_EXPORT(int32, Physics_RaycastAll)(uint64 World, FVector3 Start, FVector3 End, uint32 IgnoreEntity, FLmRayHit* OutHits, int32 Max)
{
    Physics::IPhysicsScene* S = SceneOf(World);
    if (S == nullptr || Max <= 0)
    {
        return 0;
    }

    SRayCastSettings Settings;
    Settings.Start = Start;
    Settings.End   = End;
    StageIgnore(S, IgnoreEntity, Settings.IgnoreBodies);

    TVector<SRayResult> Hits;
    S->CastRayAll(Settings, Hits);

    int32 Count = (int32)Hits.size();
    if (Count > Max) { Count = Max; }
    for (int32 i = 0; i < Count; ++i)
    {
        const SRayResult& R = Hits[i];
        FLmRayHit& H = OutHits[i];
        H.bHit     = 1;
        H.Entity   = R.Entity;
        H.BodyID   = R.BodyID;
        H.Location = R.Location;
        H.Normal   = R.Normal;
        H.Distance = R.Distance;
        H.Fraction = R.Fraction;
        H.BoneIndex = R.BoneIndex;
    }
    return Count;
}

// Closest hit, restricted to bodies whose collision layer intersects LayerMask (ECollisionProfiles bits).
LUMINA_DOTNET_EXPORT(FLmRayHit, Physics_RaycastFiltered)(uint64 World, FVector3 Start, FVector3 End, uint32 IgnoreEntity, uint32 LayerMask)
{
    FLmRayHit Hit{};
    Physics::IPhysicsScene* S = SceneOf(World);
    if (S == nullptr)
    {
        return Hit;
    }

    SRayCastSettings Settings;
    Settings.Start = Start;
    Settings.End = End;
    Settings.LayerMask = (Lumina::ECollisionProfiles)LayerMask;
    StageIgnore(S, IgnoreEntity, Settings.IgnoreBodies);

    TOptional<SRayResult> Result = S->CastRay(Settings);
    if (Result.has_value())
    {
        const SRayResult& R = Result.value();
        Hit.bHit     = 1;
        Hit.Entity   = R.Entity;
        Hit.BodyID   = R.BodyID;
        Hit.Location = R.Location;
        Hit.Normal   = R.Normal;
        Hit.Distance = R.Distance;
        Hit.Fraction = R.Fraction;
        Hit.BoneIndex = R.BoneIndex;
    }
    return Hit;
}

// Every hit near-to-far, restricted by collision layer mask. Returns the count written.
LUMINA_DOTNET_EXPORT(int32, Physics_RaycastAllFiltered)(uint64 World, FVector3 Start, FVector3 End, uint32 IgnoreEntity, uint32 LayerMask, FLmRayHit* OutHits, int32 Max)
{
    Physics::IPhysicsScene* S = SceneOf(World);
    if (S == nullptr || Max <= 0)
    {
        return 0;
    }

    SRayCastSettings Settings;
    Settings.Start = Start;
    Settings.End   = End;
    Settings.LayerMask = (Lumina::ECollisionProfiles)LayerMask;
    StageIgnore(S, IgnoreEntity, Settings.IgnoreBodies);

    TVector<SRayResult> Hits;
    S->CastRayAll(Settings, Hits);

    int32 Count = (int32)Hits.size();
    if (Count > Max) { Count = Max; }
    for (int32 i = 0; i < Count; ++i)
    {
        const SRayResult& R = Hits[i];
        FLmRayHit& H = OutHits[i];
        H.bHit     = 1;
        H.Entity   = R.Entity;
        H.BodyID   = R.BodyID;
        H.Location = R.Location;
        H.Normal   = R.Normal;
        H.Distance = R.Distance;
        H.Fraction = R.Fraction;
        H.BoneIndex = R.BoneIndex;
    }
    return Count;
}

// Distinct entities whose bodies contain the world point (volume containment). Returns the count written.
LUMINA_DOTNET_EXPORT(int32, Physics_OverlapPoint)(uint64 World, FVector3 Point, uint32 IgnoreEntity, uint32* OutEntities, int32 Max)
{
    Physics::IPhysicsScene* S = SceneOf(World);
    if (S == nullptr || Max <= 0)
    {
        return 0;
    }
    TFixedVector<uint32, MaxInlineIgnoreBodies> Ignore;
    StageIgnore(S, IgnoreEntity, Ignore);

    return S->CollidePoint(Point, Ignore, AsEntitySpan(OutEntities, Max));
}

//================================================================================================
// Debug draw (World.Draw)
//================================================================================================

LUMINA_DOTNET_EXPORT(void, Debug_DrawLine)(uint64 World, FVector3 Start, FVector3 End, FVector4 Color, float Thickness, float Duration)
{
    if (CWorld* W = AsWorld(World))
    {
        W->DrawLine(Start, End, Color, Thickness, true, Duration);
    }
}

LUMINA_DOTNET_EXPORT(void, Debug_DrawSphere)(uint64 World, FVector3 Center, float Radius, FVector4 Color, float Thickness, float Duration)
{
    if (CWorld* W = AsWorld(World))
    {
        W->GetDebugInterface()->DrawSphere(Center, Radius, Color, TOptional<float>(Thickness), TOptional<bool>(true), TOptional<float>(Duration));
    }
}

LUMINA_DOTNET_EXPORT(void, Debug_DrawBox)(uint64 World, FVector3 Center, FVector3 HalfExtents, FQuat Rotation, FVector4 Color, float Thickness, float Duration)
{
    if (CWorld* W = AsWorld(World))
    {
        W->GetDebugInterface()->DrawBox(Center, HalfExtents, Rotation, Color, TOptional<float>(Thickness), TOptional<bool>(true), TOptional<float>(Duration));
    }
}

LUMINA_DOTNET_EXPORT(void, Debug_DrawText)(uint64 World, const char* Text, int32 Length, FVector4 Color)
{
    if (CWorld* W = AsWorld(World))
    {
        W->DrawDebugText(Length > 0 ? FString(Text, (size_t)Length) : FString(), Color);
    }
}

//================================================================================================
// Net (World.Net) - the role/mode queries; C# derives IsServer/IsClient/etc. from the mode.
//================================================================================================

LUMINA_DOTNET_EXPORT(int32, Net_GetMode)(uint64 World)
{
    CWorld* W = AsWorld(World);
    return W ? (int32)W->GetNetMode() : 0;
}

LUMINA_DOTNET_EXPORT(int32, Net_GetConnectedClients)(uint64 World)
{
    CWorld* W = AsWorld(World);
    return W ? W->GetConnectedClientCount() : 0;
}

//================================================================================================
// SystemContext (the per-tick context handed to a C# EntitySystem). Ctx is the live const
// FSystemContext* the native scheduler passes through the shared managed-system shim; valid only for
// the duration of the OnUpdate crossing. Each shim forwards to the matching FSystemContext method.
//================================================================================================

LUMINA_DOTNET_EXPORT(float, SystemContext_GetDeltaTime)(const FSystemContext* Ctx)
{
    return Ctx ? (float)Ctx->GetDeltaTime() : 0.0f;
}

LUMINA_DOTNET_EXPORT(double, SystemContext_GetTime)(const FSystemContext* Ctx)
{
    return Ctx ? Ctx->GetTime() : 0.0;
}

LUMINA_DOTNET_EXPORT(uint32, SystemContext_Create)(const FSystemContext* Ctx)
{
    return Ctx ? ToId(Ctx->Create()) : ToId(entt::null);
}

LUMINA_DOTNET_EXPORT(void, SystemContext_Destroy)(const FSystemContext* Ctx, uint32 Entity)
{
    if (Ctx)
    {
        Ctx->Destroy(AsEntity(Entity));
    }
}

LUMINA_DOTNET_EXPORT(void, SystemContext_SetEntityLocation)(const FSystemContext* Ctx, uint32 Entity, FVector3 Location)
{
    // SetEntityLocation is a non-const FSystemContext method (it mutates via the registry reference, not
    // the context object). The scheduler owns a non-const FSystemContext, so removing const here is safe.
    if (Ctx)
    {
        const_cast<FSystemContext*>(Ctx)->SetEntityLocation(AsEntity(Entity), Location);
    }
}

LUMINA_DOTNET_EXPORT(void, SystemContext_DrawDebugLine)(const FSystemContext* Ctx, FVector3 Start, FVector3 End, FVector4 Color)
{
    if (Ctx)
    {
        Ctx->DrawDebugLine(Start, End, Color);
    }
}

//================================================================================================
// Navigation (World.Navigation) - navmesh queries + script-driven rebuild. Each query dispatches to
// the first ready SNavMeshComponent in the world (the Nav:: helpers). All queries are safe with no
// navmesh present: they report "not found" rather than crashing. Game thread only.
//================================================================================================

// Blittable path result mirrored by LuminaSharp.NavPathWire. The caller's OutCorners buffer is filled
// up to MaxCorners; Count is clamped to it. bValid == 0 means no path (OutCorners left untouched).
struct FLmNavPath
{
    int32 Count;
    int32 bValid;
    int32 bPartial;
};
LE_REGISTER_LAYOUT("NavPathWire", FLmNavPath);

// Blittable point result mirrored by LuminaSharp.NavPointWire, shared by ProjectPoint / Raycast /
// FindRandomReachablePoint. bFound == 0 means the query missed (Point is zero).
struct FLmNavPoint
{
    int32    bFound;
    FVector3 Point;
};
LE_REGISTER_LAYOUT("NavPointWire", FLmNavPoint);

LUMINA_DOTNET_EXPORT(int32, Nav_IsReady)(uint64 World)
{
    return Nav::IsReady(AsWorld(World)) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(FLmNavPath, Nav_FindPath)(uint64 World, FVector3 Start, FVector3 End, FVector3* OutCorners, int32 MaxCorners)
{
    FLmNavPath Result{};
    FNavPath Path;
    if (!Nav::FindPath(AsWorld(World), Start, End, Path) || !Path.bValid)
    {
        return Result;
    }

    int32 Count = (int32)Path.Corners.size();
    const int32 Cap = MaxCorners > 0 ? MaxCorners : 0;
    if (Count > Cap)
    {
        Count = Cap;
    }
    for (int32 i = 0; i < Count; ++i)
    {
        OutCorners[i] = Path.Corners[i];
    }

    Result.Count    = Count;
    Result.bValid   = 1;
    Result.bPartial = Path.bPartial ? 1 : 0;
    return Result;
}

LUMINA_DOTNET_EXPORT(FLmNavPoint, Nav_ProjectPoint)(uint64 World, FVector3 Point, FVector3 Extents)
{
    FLmNavPoint Result{};
    FVector3 Out;
    if (Nav::ProjectPoint(AsWorld(World), Point, Extents, Out))
    {
        Result.bFound = 1;
        Result.Point  = Out;
    }
    return Result;
}

LUMINA_DOTNET_EXPORT(FLmNavPoint, Nav_Raycast)(uint64 World, FVector3 Start, FVector3 End)
{
    FLmNavPoint Result{};
    FVector3 Out;
    if (Nav::Raycast(AsWorld(World), Start, End, Out))
    {
        Result.bFound = 1;
        Result.Point  = Out;
    }
    return Result;
}

LUMINA_DOTNET_EXPORT(FLmNavPoint, Nav_FindRandomReachablePoint)(uint64 World, FVector3 Origin, float Radius)
{
    FLmNavPoint Result{};
    FVector3 Out;
    if (Nav::FindRandomReachablePoint(AsWorld(World), Origin, Radius, Out))
    {
        Result.bFound = 1;
        Result.Point  = Out;
    }
    return Result;
}

LUMINA_DOTNET_EXPORT(int32, Nav_IsReachable)(uint64 World, FVector3 From, FVector3 To)
{
    return Nav::IsReachable(AsWorld(World), From, To) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(float, Nav_PathLength)(uint64 World, FVector3 From, FVector3 To)
{
    return Nav::PathLength(AsWorld(World), From, To);
}

LUMINA_DOTNET_EXPORT(int32, Nav_RequestRebuild)(uint64 World)
{
    return Nav::RequestRebuild(AsWorld(World));
}

LUMINA_DOTNET_EXPORT(void, Nav_DrawPath)(uint64 World, FVector3 From, FVector3 To, FVector4 Color, float Duration)
{
    Nav::DrawDebugPath(AsWorld(World), From, To, Color, Duration);
}

//================================================================================================
// Gameplay tags (hierarchical, interned). Id-based value API backing the C# GameplayTag struct; the
// process-global FGameplayTagRegistry is the single source of truth. Id 0 == None / invalid.
//================================================================================================

LUMINA_DOTNET_EXPORT(uint32, GameplayTag_Request)(const char* Name, int32 Len)
{
    return (Name != nullptr && Len > 0) ? FGameplayTagRegistry::Get().RequestTag(FStringView(Name, (size_t)Len)) : 0u;
}

LUMINA_DOTNET_EXPORT(int32, GameplayTag_Matches)(uint32 A, uint32 B)
{
    return FGameplayTagRegistry::Get().Matches(A, B) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(int32, GameplayTag_MatchesExact)(uint32 A, uint32 B)
{
    return FGameplayTagRegistry::Get().MatchesExact(A, B) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(uint32, GameplayTag_GetParent)(uint32 A)
{
    return FGameplayTagRegistry::Get().GetParent(A);
}

LUMINA_DOTNET_EXPORT(int32, GameplayTag_IsValid)(uint32 A)
{
    return FGameplayTagRegistry::Get().IsValid(A) ? 1 : 0;
}

// Two-pass string return: (Id, null, 0) sizes; (Id, buffer, capacity) fills. Returns the name length.
LUMINA_DOTNET_EXPORT(int32, GameplayTag_GetName)(uint32 Id, char* Buffer, int32 Capacity)
{
    const FString Name = FGameplayTagRegistry::Get().GetName(Id);
    const int32 Len = (int32)Name.size();
    if (Buffer != nullptr && Capacity > 0)
    {
        const int32 N = Len < Capacity ? Len : Capacity;
        for (int32 i = 0; i < N; ++i)
        {
            Buffer[i] = Name[(size_t)i];
        }
    }
    return Len;
}

//================================================================================================
// Per-entity gameplay tags (World.Tags). Tags live on an SGameplayTagComponent; queries are hierarchical
// (an entity tagged "Status.Burning" matches a "Status" query). TagId 0 is None. Game thread only.
//================================================================================================

namespace
{
    // Build a serializable FGameplayTag (FName-backed) from a registry id. Empty for an invalid id.
    FGameplayTag TagFromId(uint32 TagId)
    {
        FGameplayTag Tag;
        const FString Name = FGameplayTagRegistry::Get().GetName(TagId);
        if (!Name.empty())
        {
            Tag.TagName = FName(Name.c_str());
        }
        return Tag;
    }
}

LUMINA_DOTNET_EXPORT(void, GameplayTags_Add)(uint64 World, uint32 Entity, uint32 TagId)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || TagId == 0)
    {
        return;
    }
    const FGameplayTag Tag = TagFromId(TagId);
    if (Tag.IsValid())
    {
        W->GetOrEmplaceComponent<SGameplayTagComponent>(AsEntity(Entity)).Tags.AddTag(Tag);
    }
}

LUMINA_DOTNET_EXPORT(void, GameplayTags_Remove)(uint64 World, uint32 Entity, uint32 TagId)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || TagId == 0)
    {
        return;
    }
    if (SGameplayTagComponent* C = W->TryGetComponent<SGameplayTagComponent>(AsEntity(Entity)))
    {
        C->Tags.RemoveTag(TagFromId(TagId));
    }
}

LUMINA_DOTNET_EXPORT(int32, GameplayTags_Has)(uint64 World, uint32 Entity, uint32 TagId)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || TagId == 0)
    {
        return 0;
    }
    const SGameplayTagComponent* C = W->TryGetComponent<SGameplayTagComponent>(AsEntity(Entity));
    if (C == nullptr)
    {
        return 0;
    }
    FGameplayTagRegistry& Reg = FGameplayTagRegistry::Get();
    for (const FGameplayTag& Owned : C->Tags.Tags)
    {
        if (Reg.Matches(Reg.RequestTag(FStringView(Owned.TagName.c_str())), TagId))
        {
            return 1;
        }
    }
    return 0;
}

LUMINA_DOTNET_EXPORT(int32, GameplayTags_HasExact)(uint64 World, uint32 Entity, uint32 TagId)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || TagId == 0)
    {
        return 0;
    }
    const SGameplayTagComponent* C = W->TryGetComponent<SGameplayTagComponent>(AsEntity(Entity));
    if (C == nullptr)
    {
        return 0;
    }
    FGameplayTagRegistry& Reg = FGameplayTagRegistry::Get();
    for (const FGameplayTag& Owned : C->Tags.Tags)
    {
        if (Reg.RequestTag(FStringView(Owned.TagName.c_str())) == TagId)
        {
            return 1;
        }
    }
    return 0;
}

LUMINA_DOTNET_EXPORT(void, GameplayTags_Clear)(uint64 World, uint32 Entity)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr)
    {
        return;
    }
    if (SGameplayTagComponent* C = W->TryGetComponent<SGameplayTagComponent>(AsEntity(Entity)))
    {
        C->Tags.Tags.clear();
    }
}

// Fills OutIds with the entity's tag ids (up to Max); returns the count written.
LUMINA_DOTNET_EXPORT(int32, GameplayTags_Get)(uint64 World, uint32 Entity, uint32* OutIds, int32 Max)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || Max <= 0)
    {
        return 0;
    }
    const SGameplayTagComponent* C = W->TryGetComponent<SGameplayTagComponent>(AsEntity(Entity));
    if (C == nullptr)
    {
        return 0;
    }
    FGameplayTagRegistry& Reg = FGameplayTagRegistry::Get();
    int32 Count = 0;
    for (const FGameplayTag& Owned : C->Tags.Tags)
    {
        if (Count >= Max)
        {
            break;
        }
        OutIds[Count++] = Reg.RequestTag(FStringView(Owned.TagName.c_str()));
    }
    return Count;
}

//================================================================================================
// Gameplay profiler. Open/close named scopes that the editor "Gameplay Profiler" aggregates by name.
// IsEnabled lets the managed side skip per-script scope calls entirely when nobody is recording.
//================================================================================================

LUMINA_DOTNET_EXPORT(void, GameplayProfiler_Begin)(const char* Name, int32 Len)
{
    if (Name != nullptr && Len > 0)
    {
        FGameplayProfiler::Get().BeginScope(FStringView(Name, (size_t)Len));
    }
}

LUMINA_DOTNET_EXPORT(void, GameplayProfiler_End)()
{
    FGameplayProfiler::Get().EndScope();
}

LUMINA_DOTNET_EXPORT(int32, GameplayProfiler_IsEnabled)()
{
    return FGameplayProfiler::Get().IsEnabled() ? 1 : 0;
}

//================================================================================================
// UI (World.UI). Screen-space RmlUi documents + element manipulation + event listeners, driving the
// world's own Rml::Context -- the same context FInputViewport already forwards mouse/keyboard to. The
// document/element walking + StateMutex locking lives in RmlUi:: (UI/RmlUiBridge.cpp); these are the
// flat ABI wrappers. Documents/elements/listeners cross as opaque pointers; strings as (ptr,len), with
// getters using the two-pass buffer protocol. Game thread only.
//================================================================================================

namespace
{
    FStringView UIView(const char* P, int32 Len)
    {
        return (P != nullptr && Len > 0) ? FStringView(P, (size_t)Len) : FStringView();
    }

    // Two-pass string return: (..., null, 0) sizes; (..., buffer, capacity) fills. Returns full length.
    int32 UICopyOut(const FString& Value, char* Buffer, int32 Capacity)
    {
        const int32 Len = (int32)Value.size();
        if (Buffer != nullptr && Capacity > 0)
        {
            const int32 N = Len < Capacity ? Len : Capacity;
            for (int32 i = 0; i < N; ++i)
            {
                Buffer[i] = Value[(size_t)i];
            }
        }
        return Len;
    }
}

LUMINA_DOTNET_EXPORT(void*, UI_LoadDocument)(uint64 World, const char* Path, int32 Len)
{
    return RmlUi::LoadScreenDocument(AsWorld(World), UIView(Path, Len));
}

LUMINA_DOTNET_EXPORT(void*, UI_LoadDocumentFromMemory)(uint64 World, const char* Body, int32 BodyLen, const char* Url, int32 UrlLen)
{
    return RmlUi::LoadScreenDocumentFromMemory(AsWorld(World), UIView(Body, BodyLen), UIView(Url, UrlLen));
}

LUMINA_DOTNET_EXPORT(void, UI_UnloadDocument)(uint64 World, void* Document)
{
    RmlUi::UnloadScreenDocument(AsWorld(World), Document);
}

LUMINA_DOTNET_EXPORT(void, UI_ShowDocument)(void* Document, int32 Modal, int32 AutoFocus)
{
    RmlUi::ShowDocument(Document, Modal != 0, AutoFocus != 0);
}

LUMINA_DOTNET_EXPORT(void, UI_HideDocument)(void* Document)
{
    RmlUi::HideDocument(Document);
}

LUMINA_DOTNET_EXPORT(void, UI_PullDocumentToFront)(void* Document)
{
    RmlUi::PullDocumentToFront(Document);
}

LUMINA_DOTNET_EXPORT(void*, UI_GetDocumentRoot)(void* Document)
{
    return RmlUi::GetDocumentRoot(Document);
}

LUMINA_DOTNET_EXPORT(void*, UI_GetElementById)(void* Document, const char* Id, int32 Len)
{
    return RmlUi::DocumentGetElementById(Document, UIView(Id, Len));
}

LUMINA_DOTNET_EXPORT(void*, UI_QuerySelector)(void* Element, const char* Selector, int32 Len)
{
    return RmlUi::ElementQuerySelector(Element, UIView(Selector, Len));
}

LUMINA_DOTNET_EXPORT(void, UI_SetInnerRml)(void* Element, const char* Rml, int32 Len)
{
    RmlUi::ElementSetInnerRml(Element, UIView(Rml, Len));
}

LUMINA_DOTNET_EXPORT(int32, UI_GetInnerRml)(void* Element, char* Buffer, int32 Capacity)
{
    return UICopyOut(RmlUi::ElementGetInnerRml(Element), Buffer, Capacity);
}

LUMINA_DOTNET_EXPORT(void, UI_SetAttribute)(void* Element, const char* Name, int32 NameLen, const char* Value, int32 ValueLen)
{
    RmlUi::ElementSetAttribute(Element, UIView(Name, NameLen), UIView(Value, ValueLen));
}

LUMINA_DOTNET_EXPORT(int32, UI_GetAttribute)(void* Element, const char* Name, int32 NameLen, char* Buffer, int32 Capacity)
{
    return UICopyOut(RmlUi::ElementGetAttribute(Element, UIView(Name, NameLen)), Buffer, Capacity);
}

LUMINA_DOTNET_EXPORT(void, UI_SetProperty)(void* Element, const char* Name, int32 NameLen, const char* Value, int32 ValueLen)
{
    RmlUi::ElementSetProperty(Element, UIView(Name, NameLen), UIView(Value, ValueLen));
}

LUMINA_DOTNET_EXPORT(void, UI_RemoveProperty)(void* Element, const char* Name, int32 NameLen)
{
    RmlUi::ElementRemoveProperty(Element, UIView(Name, NameLen));
}

LUMINA_DOTNET_EXPORT(void, UI_SetClass)(void* Element, const char* Class, int32 ClassLen, int32 Active)
{
    RmlUi::ElementSetClass(Element, UIView(Class, ClassLen), Active != 0);
}

LUMINA_DOTNET_EXPORT(int32, UI_IsClassSet)(void* Element, const char* Class, int32 ClassLen)
{
    return RmlUi::ElementIsClassSet(Element, UIView(Class, ClassLen)) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(void, UI_ElementFocus)(void* Element) { RmlUi::ElementFocus(Element); }
LUMINA_DOTNET_EXPORT(void, UI_ElementBlur)(void* Element)  { RmlUi::ElementBlur(Element); }
LUMINA_DOTNET_EXPORT(void, UI_ElementClick)(void* Element) { RmlUi::ElementClick(Element); }

LUMINA_DOTNET_EXPORT(void*, UI_AddEventListener)(uint64 World, void* Element, const char* Type, int32 Len, void* Thunk, void* Context)
{
    return RmlUi::AddElementEventListener(AsWorld(World), Element, UIView(Type, Len),
        reinterpret_cast<RmlUi::FManagedUIEventThunk>(Thunk), Context);
}

LUMINA_DOTNET_EXPORT(void, UI_RemoveEventListener)(uint64 World, void* Listener)
{
    RmlUi::RemoveElementEventListener(AsWorld(World), Listener);
}

// Data binding (MVVM). Backs LuminaSharp's ViewModel / World.UI.AddModel: a named data model on the world
// context with scalar variables and command callbacks, bound declaratively in RML (data-model, {{ var }},
// data-event-*). Variables are registered before the document loads; values cross as double (coerced to the
// registered type natively) or (ptr,len) strings. SetThunk/EventThunk are managed function pointers.
LUMINA_DOTNET_EXPORT(void*, UI_CreateDataModel)(uint64 World, const char* Name, int32 Len, void* Context, void* SetThunk, void* EventThunk)
{
    return RmlUi::CreateDataModel(AsWorld(World), UIView(Name, Len), Context,
        reinterpret_cast<RmlUi::FManagedDataSetThunk>(SetThunk),
        reinterpret_cast<RmlUi::FManagedDataEventThunk>(EventThunk));
}

LUMINA_DOTNET_EXPORT(int32, UI_ModelBindScalar)(void* Model, const char* Name, int32 Len, int32 Type)
{
    return RmlUi::DataModelBindScalar(Model, UIView(Name, Len), Type);
}

LUMINA_DOTNET_EXPORT(void, UI_ModelBindCommand)(void* Model, const char* Name, int32 Len, int32 CommandId)
{
    RmlUi::DataModelBindCommand(Model, UIView(Name, Len), CommandId);
}

LUMINA_DOTNET_EXPORT(void, UI_ModelSetNumber)(void* Model, int32 Field, double Value)
{
    RmlUi::DataModelSetNumber(Model, Field, Value);
}

LUMINA_DOTNET_EXPORT(void, UI_ModelSetString)(void* Model, int32 Field, const char* Value, int32 Len)
{
    RmlUi::DataModelSetString(Model, Field, UIView(Value, Len));
}

LUMINA_DOTNET_EXPORT(void, UI_ModelDirty)(void* Model, int32 Field)
{
    RmlUi::DataModelDirty(Model, Field);
}

LUMINA_DOTNET_EXPORT(void, UI_ModelDirtyAll)(void* Model)
{
    RmlUi::DataModelDirtyAll(Model);
}

LUMINA_DOTNET_EXPORT(void, UI_DestroyDataModel)(void* Model)
{
    RmlUi::DestroyDataModel(Model);
}

// Lists (data-for): array-of-struct variables with string cells, pushed as a snapshot on change.
LUMINA_DOTNET_EXPORT(int32, UI_ModelBindList)(void* Model, const char* Name, int32 Len)
{
    return RmlUi::DataModelBindList(Model, UIView(Name, Len));
}

LUMINA_DOTNET_EXPORT(int32, UI_ModelBindListMember)(void* Model, int32 ListField, const char* Name, int32 Len)
{
    return RmlUi::DataModelBindListMember(Model, ListField, UIView(Name, Len));
}

LUMINA_DOTNET_EXPORT(void, UI_ModelListResize)(void* Model, int32 ListField, int32 RowCount)
{
    RmlUi::DataModelListResize(Model, ListField, RowCount);
}

LUMINA_DOTNET_EXPORT(void, UI_ModelListSetCell)(void* Model, int32 ListField, int32 Row, int32 Col, const char* Value, int32 Len)
{
    RmlUi::DataModelListSetCell(Model, ListField, Row, Col, UIView(Value, Len));
}

LUMINA_DOTNET_EXPORT(void, UI_ModelListDirty)(void* Model, int32 ListField)
{
    RmlUi::DataModelListDirty(Model, ListField);
}

// Cursor + input routing so a script can switch a game world between "camera look" and "click the UI".
LUMINA_DOTNET_EXPORT(void, UI_SetInputMode)(uint64 World, int32 Mode)
{
    if (FInputViewport* V = FInputViewportRegistry::Get().FindViewportForWorld(AsWorld(World)))
    {
        V->GetContext().SetInputMode((Lumina::EInputMode)Mode);
    }
}

LUMINA_DOTNET_EXPORT(void, UI_SetMouseMode)(uint64 World, int32 Mode)
{
    FInputViewportRegistry& Registry = FInputViewportRegistry::Get();
    if (FInputViewport* V = Registry.FindViewportForWorld(AsWorld(World)))
    {
        V->GetContext().SetMouseMode((Lumina::EMouseMode)Mode);
        Registry.ReapplyActiveCursorMode();
    }
}

//================================================================================================
// Input actions. A script binding resolves its action name to an index once per settings generation
// (the serial the poll carries), then reads state straight out of the per-frame array native hands
// it, so a bound action costs no crossing per frame.
//================================================================================================

LUMINA_DOTNET_EXPORT(int32, Input_FindActionIndex)(const char* Name, int32 Len)
{
    if (Name == nullptr || Len <= 0)
    {
        return INDEX_NONE;
    }
    return FInputActionMap::Get().FindActionIndex(FName(FStringView(Name, (size_t)Len)));
}

// The World.Input facade: the escape hatch for code that wants to poll by name rather than declare an
// SInputAction binding. Every one of these returns the neutral value when the world is not receiving
// input, so a caller never has to test first.
LUMINA_DOTNET_EXPORT(FInputActionState, Input_GetActionState)(uint64 World, const char* Name, int32 Len)
{
    if (Name == nullptr || Len <= 0)
    {
        return FInputActionState{};
    }
    const FInputActionHandle Handle{ FName(FStringView(Name, (size_t)Len)) };
    return Input::GetActionState(AsWorld(World), Handle);
}

// Mapping layers. Push a layer authored in the Input settings to change what input means right now; a
// blocking layer stops any action it does not list from reaching gameplay underneath.
LUMINA_DOTNET_EXPORT(void, Input_PushLayer)(uint64 World, const char* Name, int32 Len)
{
    if (Name != nullptr && Len > 0)
    {
        Input::PushLayer(AsWorld(World), FName(FStringView(Name, (size_t)Len)));
    }
}

LUMINA_DOTNET_EXPORT(int32, Input_PopLayer)(uint64 World, const char* Name, int32 Len)
{
    if (Name == nullptr || Len <= 0)
    {
        return 0;
    }
    return Input::PopLayer(AsWorld(World), FName(FStringView(Name, (size_t)Len))) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(int32, Input_HasLayer)(uint64 World, const char* Name, int32 Len)
{
    if (Name == nullptr || Len <= 0)
    {
        return 0;
    }
    return Input::HasLayer(AsWorld(World), FName(FStringView(Name, (size_t)Len))) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(void, Input_ClearLayers)(uint64 World)
{
    Input::ClearLayers(AsWorld(World));
}

LUMINA_DOTNET_EXPORT(int32, Input_IsReceivingInput)(uint64 World)
{
    return Input::IsReceivingInput(AsWorld(World)) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(int32, Input_IsKeyDown)(uint64 World, int32 Key)
{
    return Input::IsKeyDown(AsWorld(World), (Lumina::EKey)Key) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(int32, Input_IsKeyPressed)(uint64 World, int32 Key)
{
    return Input::IsKeyPressed(AsWorld(World), (Lumina::EKey)Key) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(int32, Input_IsKeyReleased)(uint64 World, int32 Key)
{
    return Input::IsKeyReleased(AsWorld(World), (Lumina::EKey)Key) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(int32, Input_IsMouseButtonDown)(uint64 World, int32 Button)
{
    return Input::IsMouseButtonDown(AsWorld(World), (Lumina::EMouseKey)Button) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(int32, Input_IsMouseButtonPressed)(uint64 World, int32 Button)
{
    return Input::IsMouseButtonPressed(AsWorld(World), (Lumina::EMouseKey)Button) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(int32, Input_IsMouseButtonReleased)(uint64 World, int32 Button)
{
    return Input::IsMouseButtonReleased(AsWorld(World), (Lumina::EMouseKey)Button) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(FVector2, Input_GetMousePosition)(uint64 World)
{
    return Input::GetMousePosition(AsWorld(World));
}

LUMINA_DOTNET_EXPORT(FVector2, Input_GetMouseDelta)(uint64 World)
{
    return Input::GetMouseDelta(AsWorld(World));
}

LUMINA_DOTNET_EXPORT(float, Input_GetMouseWheel)(uint64 World)
{
    return Input::GetMouseWheel(AsWorld(World));
}
