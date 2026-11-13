#include "RuntimePCH.h"
#include "CameraSystem.h"
#include "WeatherSystem.h"
#include "WeatherInteractionSystem.h"

#include "Animation/BindPose.h"
#include "Animation/SkeletalMeshUtils.h"
#include "Physics/PhysicsScene.h"
#include "Renderer/SkeletonResource.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Components/WaterComponent.h"
#include "World/Entity/Components/WeatherInteractorComponent.h"

namespace Lumina
{
    namespace WeatherInteractionSystemInternal
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
                if (Lower.Contains(Reject))
                {
                    return 0.0f;
                }
            }
            for (const FAutoContactRule& Rule : kAutoContactRules)
            {
                if (Lower.Contains(Rule.Pattern) && (Rule.Exclude == nullptr || !Lower.Contains(Rule.Exclude)))
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
            FWeatherContactRuntime& Contact = Interactor.Runtime.EmplaceGetRef();
            Contact.BoneIndex = BoneIndex;
            Contact.Offset    = Offset;
            Contact.Radius    = Radius;
        }

        // Turns the authored contacts, or the ones found on the mesh, into bone indices and local offsets.
        void ResolveContacts(ECS::FEntity Entity, SWeatherInteractorComponent& Interactor, const SSkeletalMeshComponent* Skeletal,
                             const FSkeletonResource* Skeleton, const SStaticMeshComponent* Static)
        {
            Interactor.Runtime.Reset();
            Interactor.ResolvedSkeleton     = Skeleton;
            Interactor.ResolvedContactCount = (uint32)Interactor.Contacts.Num();
            Interactor.bResolved            = true;

            if (!Interactor.Contacts.IsEmpty())
            {
                for (const SWeatherContactPoint& Point : Interactor.Contacts)
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
                for (int32 Bone = 0; Bone < Skeleton->GetNumBones(); ++Bone)
                {
                    const float Radius = AutoContactRadius(Skeleton->GetBone(Bone).Name);
                    if (Radius <= 0.0f)
                    {
                        continue;
                    }
                    if (static_cast<uint32>(Interactor.Runtime.Num()) >= kMaxAutoContacts)
                    {
                        LOG_WARN_ONCE("CWeatherInteractionSystem: entity {} has more than {} auto contact bones, the rest are dropped.", Entity.Value, kMaxAutoContacts);
                        break;
                    }
                    AddResolved(Interactor, Bone, FVector3(0.0f), Radius);
                }
                if (!Interactor.Runtime.IsEmpty())
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
            const FMatrix4 BindGlobal = AnimPose::GetBindGlobalMatrix(&Skeleton, Bone);
            return static_cast<size_t>(Skeletal.BoneTransforms.Num()) == (size_t)Skeleton.GetNumBones() ? Skeletal.BoneTransforms[Bone] * BindGlobal : BindGlobal;
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

    void CWeatherInteractionSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameEnd);
        RequireUpdate(EUpdateStage::Paused);

        // Reads this frame's weather and resolved view.
        RunsAfter<CWeatherSystem, CCameraSystem>();
    }

    void CWeatherInteractionSystem::OnUpdate(const SSystemContext& Context)
    {
        using namespace WeatherInteractionSystemInternal;

        FWeatherInteractionState& State = *InteractionState;
        State.Contacts.Reset();
        State.Impacts.Reset();
        State.Interactors.Reset();
        if (Interactors.IsEmpty())
        {
            return;
        }

        // The editor ticks paused, where only the real clock moves while an entity is dragged through water.
        const float StepTime  = Context.GetDeltaTime() > 0.0f ? Context.GetDeltaTime() : Context.GetRealDeltaTime();
        const float DeltaTime = Math::Min(StepTime, kMaxStepSeconds);
        const float Time      = (float)Context.GetTime();

        const SWeatherState& Weather = *WeatherState;
        const bool  bWeather      = Weather.bActive;
        const float RainIntensity = bWeather ? Weather.Params.Rain.Intensity : 0.0f;
        const float MaxWetness    = bWeather ? Weather.Params.Rain.MaxWetness : 1.0f;
        // Ground only matters once it holds water or snow, so a dry world spends no rays on it.
        const bool  bWetGround    = bWeather && (Weather.Wetness > 0.01f || Weather.PuddleLevel > 0.01f || Weather.SnowCover > 0.01f);

        FVector3 FallDirection(0.0f, -1.0f, 0.0f);
        if (bWeather)
        {
            const FRainParams& Rain = Weather.Params.Rain;
            const FVector2 Wind = Weather.Params.Wind.Direction * Weather.Params.Wind.Speed * Rain.WindInfluence;
            FallDirection = Math::Normalize(FVector3(Wind.x, -Math::Max(Rain.FallSpeed, 0.5f), Wind.y));
        }

        const FResolvedSceneView& View = *ResolvedView;
        const bool     bHasCamera = View.bHasView;
        const FVector3 Camera     = bHasCamera ? View.ViewVolume.GetViewPosition() : FVector3(0.0f);

        TInlineVector<FWaterSurfaceSource, kMaxWaterBodies> Waters;
        WaterSources.ForEach([&](ECS::FEntity Entity, const SWaterComponent& Water, const STransformComponent& Transform)
        {
            if (!Water.bInteractive)
            {
                return;
            }
            if (static_cast<uint32>(Waters.Num()) >= kMaxWaterBodies)
            {
                LOG_WARN_ONCE("CWeatherInteractionSystem: more than {} interactive water bodies in one update, entity {} and later are dropped.", kMaxWaterBodies, Entity.Value);
                return;
            }
            Waters.Emplace(&Water, Math::Inverse(WaterToWorld(Water, Transform.GetWorldMatrix())), Transform.GetWorldLocation().y);
        });

        Physics::IPhysicsQueries* Physics = Context.GetPhysics();
        const ECS::FHierarchy& Hierarchy = Context.GetHierarchy();

        Interactors.ForEach([&](ECS::FEntity Entity, ECS::TMut<SWeatherInteractorComponent> InteractorMut, const STransformComponent& Transform)
        {
            if (!InteractorMut.Get().bEnabled)
            {
                return;
            }
            SWeatherInteractorComponent& Interactor = InteractorMut.GetMut();

            const SSkeletalMeshComponent* Skeletal = SkeletalMeshes.TryGet<SSkeletalMeshComponent>(Entity);
            const SStaticMeshComponent*   Static   = StaticMeshes.TryGet<SStaticMeshComponent>(Entity);
            const FSkeletonResource*      Skeleton = Skeletal != nullptr ? SkeletalUtils::GetSkeleton(*Skeletal) : nullptr;
            if (Skeleton != nullptr && !Skeleton->HasBindGlobalMatrices())
            {
                Skeleton = nullptr;
            }

            if (!Interactor.bResolved || Interactor.ResolvedSkeleton != Skeleton || Interactor.ResolvedContactCount != (uint32)Interactor.Contacts.Num())
            {
                ResolveContacts(Entity, Interactor, Skeletal, Skeleton, Static);
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

            const uint32 FirstImpact = (uint32)State.Impacts.Num();
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
                    if (Ground.IsSet() && Ground->bHit && (Hit.Surface != EWeatherSurface::Water || Ground->Location.y > Hit.Y))
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
                    FWeatherContact& Out = State.Contacts.EmplaceGetRef();
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
                    for (uint32 i = FirstImpact; i < (uint32)State.Impacts.Num(); ++i)
                    {
                        bDuplicate |= Math::Distance(State.Impacts[i].Position, Position) < Radius * 2.5f;
                    }

                    if (bEntered && FallSpeed > kMinImpactSpeed && !bDuplicate)
                    {
                        FWeatherImpact& Impact = State.Impacts.EmplaceGetRef();
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
                            FWeatherImpact& Spray = State.Impacts.EmplaceGetRef();
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
                Interactor.RainExposure = Roof.IsSet() && Roof->bHit ? 0.0f : 1.0f;
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
                FWeatherInteractorState& Record = State.Interactors.EmplaceGetRef();
                Record.EntityID         = Target.Value;
                Record.RootY            = RootY;
                Record.Soak             = Interactor.Soak;
                Record.WaterlineHeight  = Interactor.WaterlineHeight;
                Record.WaterlineWetness = Interactor.WaterlineWetness;
            };

            // Characters are often a root entity with their meshes as children, which draw under their own ids.
            AddRecord(Entity);
            uint32 Children = 0u;
            for (ECS::FEntity Child = Hierarchy.GetFirstChild(Entity); !Child.IsNull(); Child = Hierarchy.GetNextSibling(Child))
            {
                if (SkeletalMeshes.TryGet<SSkeletalMeshComponent>(Child) == nullptr && StaticMeshes.TryGet<SStaticMeshComponent>(Child) == nullptr)
                {
                    continue;
                }
                if (Children >= kMaxWetChildren)
                {
                    LOG_WARN_ONCE("CWeatherInteractionSystem: entity {} has more than {} wet mesh children, the rest are not marked wet.", Entity.Value, kMaxWetChildren);
                    break;
                }
                AddRecord(Child);
                ++Children;
            }
        });
    }
}
