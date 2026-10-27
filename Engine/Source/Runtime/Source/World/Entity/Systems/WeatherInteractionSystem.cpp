#include "RuntimePCH.h"
#include "WeatherInteractionSystem.h"

#include "SystemContext.h"
#include "SystemResources.h"
#include "SystemSingletons.h"
#include "WeatherState.h"
#include "Animation/SkeletalMeshUtils.h"
#include "Physics/PhysicsScene.h"
#include "Renderer/SkeletonResource.h"
#include "World/World.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Components/WaterComponent.h"
#include "World/Entity/Components/WeatherInteractorComponent.h"

namespace Lumina
{
    namespace
    {
        constexpr float  kMaxStepSeconds         = 0.25f;
        // Beyond this from the camera a contact neither ripples nor splashes anything the player could see.
        constexpr float  kMaxContactDistance     = 80.0f;
        // Standing water a puddle holds above the ground, which a foot has to break to splash.
        constexpr float  kPuddleDepth            = 0.015f;
        constexpr float  kGroundProbe            = 0.6f;
        constexpr float  kMinImpactSpeed         = 0.35f;
        constexpr float  kSprayMinSpeed          = 1.0f;
        constexpr float  kSprayDistance          = 0.4f;
        // A contact that jumped this far in a frame was teleported, so it must not sweep a wake across the gap.
        constexpr float  kTeleportDistance       = 3.0f;
        constexpr float  kExposureRayLength      = 40.0f;
        constexpr uint32 kExposureIntervalFrames = 8u;
        constexpr float  kWaterlineDrainPerSecond = 0.01f;
        constexpr uint32 kMaxAutoContacts        = 8u;
        constexpr uint32 kMaxWetChildren         = 16u;
        constexpr uint32 kMaxWaterBodies         = 16u;

        struct FAutoContactRule
        {
            const char* Pattern;
            const char* Exclude;
            float       Radius;
        };

        // Feet and toes splash, shins wade and hips push the bow wave, which together cover walking, wading and swimming.
        constexpr FAutoContactRule kAutoContactRules[] =
        {
            { "toe",    nullptr, 0.06f },
            { "ball",   "eye",   0.06f },
            { "foot",   nullptr, 0.09f },
            { "calf",   nullptr, 0.08f },
            { "shin",   nullptr, 0.08f },
            { "leg",    "up",    0.08f },
            { "pelvis", nullptr, 0.16f },
            { "hips",   nullptr, 0.16f },
        };

        // Helper and end bones sit at odd places or never move, so a rule never picks one.
        constexpr const char* kAutoContactRejects[] = { "end", "twist", "ik", "pole", "target", "roll", "nub", "tip" };

        float AutoContactRadius(const FName& BoneName)
        {
            FString Lower = BoneName.ToString();
            Lower.ToLower();
            for (const char* Reject : kAutoContactRejects)
            {
                if (Lower.contains(Reject))
                {
                    return 0.0f;
                }
            }
            for (const FAutoContactRule& Rule : kAutoContactRules)
            {
                if (Lower.contains(Rule.Pattern) && (Rule.Exclude == nullptr || !Lower.contains(Rule.Exclude)))
                {
                    return Rule.Radius;
                }
            }
            return 0.0f;
        }

        float MaxAxisScale(const FMatrix4& Matrix)
        {
            const float X = Math::Length(FVector3(Matrix * FVector4(1.0f, 0.0f, 0.0f, 0.0f)));
            const float Y = Math::Length(FVector3(Matrix * FVector4(0.0f, 1.0f, 0.0f, 0.0f)));
            const float Z = Math::Length(FVector3(Matrix * FVector4(0.0f, 0.0f, 1.0f, 0.0f)));
            return Math::Max(Math::Max(X, Y), Z);
        }

        void AddResolved(SWeatherInteractorComponent& Interactor, int32 BoneIndex, const FVector3& Offset, float Radius)
        {
            FWeatherContactRuntime& Contact = Interactor.Runtime.emplace_back();
            Contact.BoneIndex = BoneIndex;
            Contact.Offset    = Offset;
            Contact.Radius    = Radius;
        }

        // Turns the authored contacts, or the ones found on the mesh, into bone indices and local offsets.
        void ResolveContacts(SWeatherInteractorComponent& Interactor, const SSkeletalMeshComponent* Skeletal,
                             const FSkeletonResource* Skeleton, const SStaticMeshComponent* Static)
        {
            Interactor.Runtime.clear();
            Interactor.ResolvedSkeleton     = Skeleton;
            Interactor.ResolvedContactCount = (uint32)Interactor.Contacts.size();
            Interactor.bResolved            = true;

            if (!Interactor.Contacts.empty())
            {
                for (const FWeatherContactPoint& Point : Interactor.Contacts)
                {
                    int32    BoneIndex    = Constants::kIndexNone;
                    FMatrix4 SocketOffset = FMatrix4(1.0f);
                    if (!Point.Bone.IsNone() && Skeletal != nullptr && !SkeletalUtils::ResolveSocket(*Skeletal, Point.Bone, BoneIndex, SocketOffset))
                    {
                        BoneIndex = Constants::kIndexNone;
                    }
                    const FVector3 Offset = BoneIndex != Constants::kIndexNone ? FVector3(SocketOffset * FVector4(Point.Offset, 1.0f)) : Point.Offset;
                    AddResolved(Interactor, BoneIndex, Offset, Point.Radius);
                }
                return;
            }

            if (Skeleton != nullptr)
            {
                for (int32 Bone = 0; Bone < Skeleton->GetNumBones() && Interactor.Runtime.size() < kMaxAutoContacts; ++Bone)
                {
                    const float Radius = AutoContactRadius(Skeleton->GetBone(Bone).Name);
                    if (Radius > 0.0f)
                    {
                        AddResolved(Interactor, Bone, FVector3(0.0f), Radius);
                    }
                }
                if (!Interactor.Runtime.empty())
                {
                    return;
                }
            }

            if (Static != nullptr)
            {
                const SAABB Bounds = Static->GetAABB();
                const FVector3 Half = (Bounds.Max - Bounds.Min) * 0.5f;
                if (Half.x > 0.0f || Half.y > 0.0f || Half.z > 0.0f)
                {
                    AddResolved(Interactor, Constants::kIndexNone, (Bounds.Min + Bounds.Max) * 0.5f, Math::Max((Half.x + Half.y + Half.z) / 3.0f, 0.02f));
                    return;
                }
            }

            AddResolved(Interactor, Constants::kIndexNone, FVector3(0.0f), 0.25f);
        }

        // Component-space bone origin from the live pose, or the bind pose before the first one is evaluated.
        FMatrix4 BoneComponentMatrix(const SSkeletalMeshComponent& Skeletal, const FSkeletonResource& Skeleton, int32 Bone)
        {
            const FMatrix4& BindGlobal = Skeleton.BindGlobalMatrices[Bone];
            return Skeletal.BoneTransforms.size() == (size_t)Skeleton.GetNumBones() ? Skeletal.BoneTransforms[Bone] * BindGlobal : BindGlobal;
        }

        struct FWaterSurfaceSource
        {
            const SWaterComponent* Water;
            FMatrix4               WorldToWater;
            float                  PlaneY;
        };

        struct FSurfaceHit
        {
            EWeatherSurface Surface = EWeatherSurface::None;
            float           Y       = 0.0f;
            FVector3        Normal  = FVector3(0.0f, 1.0f, 0.0f);
        };
    }

    void SWeatherInteractionSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameEnd);
        RequireUpdate(EUpdateStage::Paused);
        Writes<SWeatherInteractorComponent>();
        Reads<STransformComponent, SSkeletalMeshComponent, SStaticMeshComponent, SWaterComponent, SystemResource::Hierarchy>();
        Writes<SystemResource::PhysicsQuery>();
    }

    void SWeatherInteractionSystem::OnStartup(const SSystemContext& Context)
    {
        GetWorld()->GetOrEmplaceSingleton<FWeatherInteractionState>();
    }

    bool SWeatherInteractionSystem::HasWork(const SSystemContext& Context)
    {
        CWorld* World = GetWorld();
        const ECS::TComponentStorage<SWeatherInteractorComponent> Interactors = World != nullptr
            ? World->GetEntityRegistry().FindStorage<SWeatherInteractorComponent>()
            : ECS::TComponentStorage<SWeatherInteractorComponent>();
        if (Interactors && !Interactors.IsEmpty())
        {
            return true;
        }

        // Last frame's contacts would otherwise linger and keep rippling water nobody is touching.
        FWeatherInteractionState* State = World != nullptr ? World->TryGetSingleton<FWeatherInteractionState>() : nullptr;
        if (State != nullptr)
        {
            State->Contacts.clear();
            State->Impacts.clear();
            State->Interactors.clear();
        }
        return false;
    }

    void SWeatherInteractionSystem::OnUpdate(const SSystemContext& Context)
    {
        LUMINA_PROFILE_SCOPE();

        CWorld* World = Context.GetWorld();
        FWeatherInteractionState* State = World != nullptr ? World->TryGetSingleton<FWeatherInteractionState>() : nullptr;
        if (State == nullptr)
        {
            return;
        }

        State->Contacts.clear();
        State->Impacts.clear();
        State->Interactors.clear();
        ++FrameCounter;

        // The editor ticks paused, where only the real clock moves while an entity is dragged through water.
        const double StepTime  = Context.GetDeltaTime() > 0.0 ? Context.GetDeltaTime() : World->GetRealDeltaTime();
        const float  DeltaTime = Math::Min((float)StepTime, kMaxStepSeconds);
        const float Time      = (float)Context.GetTime();

        const FWeatherState* Weather = World->TryGetSingleton<FWeatherState>();
        const bool  bWeather      = Weather != nullptr && Weather->bActive;
        const float RainIntensity = bWeather ? Weather->Params.Rain.Intensity : 0.0f;
        const float MaxWetness    = bWeather ? Weather->Params.Rain.MaxWetness : 1.0f;
        // Ground only matters once it holds water or snow, so a dry world spends no rays on it.
        const bool  bWetGround    = bWeather && (Weather->Wetness > 0.01f || Weather->PuddleLevel > 0.01f || Weather->SnowCover > 0.01f);

        FVector3 FallDirection(0.0f, -1.0f, 0.0f);
        if (bWeather)
        {
            const FRainParams& Rain = Weather->Params.Rain;
            const FVector2 Wind = Weather->Params.Wind.Direction * Weather->Params.Wind.Speed * Rain.WindInfluence;
            FallDirection = Math::Normalize(FVector3(Wind.x, -Math::Max(Rain.FallSpeed, 0.5f), Wind.y));
        }

        const FResolvedSceneView* View = World->TryGetSingleton<FResolvedSceneView>();
        const bool     bHasCamera = View != nullptr && View->bHasView;
        const FVector3 Camera     = bHasCamera ? View->ViewVolume.GetViewPosition() : FVector3(0.0f);

        TFixedVector<FWaterSurfaceSource, kMaxWaterBodies> Waters;
        Context.CreateView<SWaterComponent, STransformComponent>(ECS::TExclude<SDisabledTag>{}).ForEach(
            [&](const SWaterComponent& Water, const STransformComponent& Transform)
            {
                if (!Water.bInteractive || Waters.size() >= kMaxWaterBodies)
                {
                    return;
                }
                Waters.push_back({ &Water, Math::Inverse(WaterToWorld(Water, Transform.GetWorldMatrix())), Transform.GetWorldLocation().y });
            });

        Physics::IPhysicsScene* Physics = Context.GetPhysicsScene();
        const ECS::FHierarchy& Hierarchy = Context.GetHierarchy();

        Context.CreateView<SWeatherInteractorComponent, STransformComponent>(ECS::TExclude<SDisabledTag>{}).ForEach(
            [&](ECS::FEntity Entity, SWeatherInteractorComponent& Interactor, const STransformComponent& Transform)
            {
                if (!Interactor.bEnabled)
                {
                    return;
                }

                const SSkeletalMeshComponent* Skeletal = Context.TryGet<SSkeletalMeshComponent>(Entity);
                const SStaticMeshComponent*   Static   = Context.TryGet<SStaticMeshComponent>(Entity);
                const FSkeletonResource*      Skeleton = Skeletal != nullptr ? SkeletalUtils::GetSkeleton(*Skeletal) : nullptr;
                if (Skeleton != nullptr && !Skeleton->HasBindGlobalMatrices())
                {
                    Skeleton = nullptr;
                }

                if (!Interactor.bResolved || Interactor.ResolvedSkeleton != Skeleton || Interactor.ResolvedContactCount != (uint32)Interactor.Contacts.size())
                {
                    ResolveContacts(Interactor, Skeletal, Skeleton, Static);
                }

                const FMatrix4 EntityWorld = Transform.GetWorldMatrix();
                const float    Scale       = MaxAxisScale(EntityWorld);
                const float    RootY       = Transform.GetWorldLocation().y;
                const bool     bNearCamera = !bHasCamera || Math::Distance(Transform.GetWorldLocation(), Camera) < kMaxContactDistance;

                // The ray from a foot must pass through the body carrying it, so its own entity and every ancestor are ignored.
                SRayCastSettings Ray;
                for (ECS::FEntity Ancestor = Entity; !Ancestor.IsNull(); Ancestor = Hierarchy.GetParent(Ancestor))
                {
                    Ray.AddIgnoredEntity(Ancestor);
                }

                const uint32 FirstImpact = (uint32)State->Impacts.size();
                float Submerged = -1.0f;

                for (FWeatherContactRuntime& Contact : Interactor.Runtime)
                {
                    FMatrix4 ContactWorld = EntityWorld;
                    if (Contact.BoneIndex != Constants::kIndexNone && Skeleton != nullptr && Contact.BoneIndex < Skeleton->GetNumBones())
                    {
                        ContactWorld = EntityWorld * BoneComponentMatrix(*Skeletal, *Skeleton, Contact.BoneIndex);
                    }
                    const FVector3 Position = FVector3(ContactWorld * FVector4(Contact.Offset, 1.0f));
                    const float    Radius   = Contact.Radius * Scale;

                    if (!Contact.bHasPrev || Math::Distance(Position, Contact.PrevPosition) > kTeleportDistance)
                    {
                        Contact.PrevPosition = Position;
                        Contact.bHasPrev     = false;
                    }

                    FSurfaceHit Hit;
                    for (const FWaterSurfaceSource& Water : Waters)
                    {
                        const FVector4 Local = Water.WorldToWater * FVector4(Position, 1.0f);
                        if (Math::Abs(Local.x) > 0.5f || Math::Abs(Local.z) > 0.5f)
                        {
                            continue;
                        }
                        const float SurfaceY = Water.PlaneY + WaterWaveHeight(*Water.Water, Position.x, Position.z, Time);
                        if (Hit.Surface != EWeatherSurface::Water || SurfaceY > Hit.Y)
                        {
                            Hit.Surface = EWeatherSurface::Water;
                            Hit.Y       = SurfaceY;
                        }
                    }

                    // A dry floor only matters as somewhere a puddle could be, so no ray is cast for it in dry weather.
                    if (bWetGround && bNearCamera && Physics != nullptr && Position.y - Radius < (Hit.Surface == EWeatherSurface::Water ? Hit.Y : 1e30f))
                    {
                        Ray.Start = Position + FVector3(0.0f, Radius, 0.0f);
                        Ray.End   = Position - FVector3(0.0f, Radius + kGroundProbe, 0.0f);
                        const TOptional<SRayResult> Ground = Physics->CastRay(Ray);
                        if (Ground.has_value() && Ground->bHit && (Hit.Surface != EWeatherSurface::Water || Ground->Location.y > Hit.Y))
                        {
                            Hit.Surface = EWeatherSurface::Ground;
                            Hit.Y       = Ground->Location.y;
                            Hit.Normal  = Ground->Normal;
                        }
                    }

                    if (!Contact.bHasPrev)
                    {
                        Contact.PrevSurfaceY = Hit.Y;
                    }

                    if (Hit.Surface == EWeatherSurface::Water)
                    {
                        Submerged = Math::Max(Submerged, Hit.Y - RootY);
                    }

                    if (Hit.Surface != EWeatherSurface::None && bNearCamera)
                    {
                        FWeatherContact& Out = State->Contacts.emplace_back();
                        Out.Position       = Position;
                        Out.Radius         = Radius;
                        Out.PrevPosition   = Contact.PrevPosition;
                        Out.SurfaceY       = Hit.Y;
                        Out.PrevSurfaceY   = Contact.PrevSurfaceY;
                        Out.RippleStrength = Interactor.RippleStrength;
                        Out.FoamStrength   = Interactor.FoamStrength;
                        Out.TrailStrength  = Interactor.TrailStrength;
                        Out.Surface        = Hit.Surface;

                        const float WaterDepth  = Hit.Surface == EWeatherSurface::Ground ? kPuddleDepth : 0.0f;
                        const float SurfaceNow  = Hit.Y + WaterDepth;
                        const float SurfacePrev = Contact.PrevSurfaceY + WaterDepth;
                        const float FallSpeed   = DeltaTime > 0.0f ? (Contact.PrevPosition.y - Position.y) / DeltaTime : 0.0f;
                        const bool  bEntered    = Contact.bHasPrev && Contact.PrevPosition.y - Radius > SurfacePrev && Position.y - Radius <= SurfaceNow;

                        // Two contacts of one foot landing together make one splash, not a cluster of them.
                        bool bDuplicate = false;
                        for (uint32 i = FirstImpact; i < (uint32)State->Impacts.size(); ++i)
                        {
                            bDuplicate |= Math::Distance(State->Impacts[i].Position, Position) < Radius * 2.5f;
                        }

                        if (bEntered && FallSpeed > kMinImpactSpeed && !bDuplicate)
                        {
                            FWeatherImpact& Impact = State->Impacts.emplace_back();
                            Impact.Position = FVector3(Position.x, SurfaceNow, Position.z);
                            Impact.Normal   = Hit.Normal;
                            Impact.Speed    = FallSpeed;
                            Impact.Radius   = Radius;
                            Impact.Scale    = Interactor.SplashScale * Math::Clamp(FallSpeed / 3.0f, 0.25f, 2.5f);
                            Impact.Surface  = Hit.Surface;
                            Impact.Entity   = Entity.Value;
                        }

                        // Running through water throws spray off the shins at the waterline, spaced by distance so it reads the same at any frame rate.
                        const FVector2 Moved(Position.x - Contact.PrevPosition.x, Position.z - Contact.PrevPosition.z);
                        const float HorizontalSpeed = DeltaTime > 0.0f ? Math::Length(Moved) / DeltaTime : 0.0f;
                        const bool  bAtWaterline    = Hit.Surface == EWeatherSurface::Water && Math::Abs(Position.y - Hit.Y) < Radius;
                        if (bAtWaterline && HorizontalSpeed > kSprayMinSpeed)
                        {
                            Contact.SprayDistance += Math::Length(Moved);
                            if (Contact.SprayDistance > kSprayDistance && !bDuplicate)
                            {
                                Contact.SprayDistance = 0.0f;
                                FWeatherImpact& Spray = State->Impacts.emplace_back();
                                Spray.Position = FVector3(Position.x, Hit.Y, Position.z);
                                Spray.Speed    = HorizontalSpeed;
                                Spray.Radius   = Radius;
                                Spray.Scale    = Interactor.SplashScale * Math::Clamp(HorizontalSpeed / 6.0f, 0.15f, 1.0f);
                                Spray.Surface  = EWeatherSurface::Water;
                                Spray.Entity   = Entity.Value;
                            }
                        }
                    }

                    Contact.PrevPosition = Position;
                    Contact.PrevSurfaceY = Hit.Y;
                    Contact.bHasPrev     = true;
                }

                if (!Interactor.bGetsWet)
                {
                    Interactor.Soak             = 0.0f;
                    Interactor.WaterlineWetness = 0.0f;
                    return;
                }

                // Cover rarely changes from one frame to the next, so the roof test is spread over several.
                if (RainIntensity > 0.01f && Physics != nullptr && Interactor.ExposureCountdown-- == 0u)
                {
                    Interactor.ExposureCountdown = kExposureIntervalFrames - 1u + (Entity.Value % 3u);
                    SRayCastSettings Cover = Ray;
                    Cover.Start = Transform.GetWorldLocation() + FVector3(0.0f, 1.5f * Scale, 0.0f);
                    Cover.End   = Cover.Start - FallDirection * kExposureRayLength;
                    const TOptional<SRayResult> Roof = Physics->CastRay(Cover);
                    Interactor.RainExposure = Roof.has_value() && Roof->bHit ? 0.0f : 1.0f;
                }

                if (RainIntensity > 0.01f && Interactor.RainExposure > 0.0f)
                {
                    Interactor.Soak = Math::MoveTowards(Interactor.Soak, MaxWetness, Interactor.SoakRate * RainIntensity * Interactor.RainExposure * DeltaTime);
                }
                else
                {
                    Interactor.Soak = Math::MoveTowards(Interactor.Soak, 0.0f, Interactor.DryingRate * DeltaTime);
                }

                if (Submerged > 0.0f)
                {
                    Interactor.WaterlineHeight  = Interactor.WaterlineWetness > 0.0f ? Math::Max(Interactor.WaterlineHeight, Submerged) : Submerged;
                    Interactor.WaterlineWetness = 1.0f;
                }
                else
                {
                    // Soaked cloth dries slower than a rain sheen and the line creeps down as the water wicks out.
                    Interactor.WaterlineWetness = Math::MoveTowards(Interactor.WaterlineWetness, 0.0f, Interactor.DryingRate * 0.5f * DeltaTime);
                    Interactor.WaterlineHeight  = Math::MoveTowards(Interactor.WaterlineHeight, 0.0f, kWaterlineDrainPerSecond * DeltaTime);
                }

                if (Interactor.Soak <= 0.0f && Interactor.WaterlineWetness <= 0.0f)
                {
                    return;
                }

                const auto AddRecord = [&](ECS::FEntity Target)
                {
                    FWeatherInteractorState& Record = State->Interactors.emplace_back();
                    Record.EntityID         = Target.Value;
                    Record.RootY            = RootY;
                    Record.Soak             = Interactor.Soak;
                    Record.WaterlineHeight  = Interactor.WaterlineHeight;
                    Record.WaterlineWetness = Interactor.WaterlineWetness;
                };

                // Characters are often a root entity with their meshes as children, which draw under their own ids.
                AddRecord(Entity);
                uint32 Children = 0u;
                for (ECS::FEntity Child = Hierarchy.GetFirstChild(Entity); !Child.IsNull() && Children < kMaxWetChildren; Child = Hierarchy.GetNextSibling(Child))
                {
                    if (Context.TryGet<SSkeletalMeshComponent>(Child) != nullptr || Context.TryGet<SStaticMeshComponent>(Child) != nullptr)
                    {
                        AddRecord(Child);
                        ++Children;
                    }
                }
            });
    }
}
