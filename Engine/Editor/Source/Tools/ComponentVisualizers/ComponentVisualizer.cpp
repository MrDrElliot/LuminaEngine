#include "Core/Threading/Thread.h"
#include "World/ECS/Registry.h"
#include "EditorPCH.h"
#include "ComponentVisualizer.h"
#include "ComponentVisualizerContext.h"
#include "Core/Math/Color.h"
#include "Renderer/PrimitiveDrawInterface.h"
#include "Tools/Import/ImportHelpers.h"
#include "Audio/AudioTypes.h"
#include "Assets/AssetTypes/Audio/SoundBase.h"
#include "World/Entity/Components/AudioSourceComponent.h"
#include "World/Entity/Components/ProceduralAudioComponent.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/DecalComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/PerceptionComponent.h"
#include "World/Entity/Components/ReflectionProbeComponent.h"
#include "World/Entity/Components/SplineComponent.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina
{
    static CComponentVisualizerRegistry* Singleton = nullptr;
    
    
    CComponentVisualizerRegistry& CComponentVisualizerRegistry::Get()
    {
        static FOnceFlag Flag;
        CallOnce(Flag, []()
        {
            Singleton = NewObject<CComponentVisualizerRegistry>();
        });

        return *Singleton;
    }

    void CComponentVisualizerRegistry::RegisterComponentVisualizer(CComponentVisualizer* Visualizer)
    {
        if (CStruct* SupportedType = Visualizer->GetSupportedComponentType())
        {
            Visualizers.emplace(SupportedType, Visualizer);
        }
    }

    CComponentVisualizer* CComponentVisualizerRegistry::GetComponentVisualizer(CStruct* Component)
    {
        auto It = Visualizers.find(Component);
        if (It != Visualizers.end())
        {
            return It->second;
        }
        
        return nullptr;
    }

    void CComponentVisualizer::PostCreateCDO()
    {
        CComponentVisualizerRegistry::Get().RegisterComponentVisualizer(this);
    }

    namespace
    {
        // Solid colliders draw blue; triggers (sensors) draw green so the two read differently at a glance.
        FVector4 ColliderColor(bool bIsTrigger)
        {
            return bIsTrigger ? FColor::Green : FColor::Blue;
        }

        struct FBoxFace
        {
            FVector3 Center;
            FVector3 Normal;
            FVector3 HalfU;
            FVector3 HalfV;
            int32    Axis = 0;
            float    Sign = 1.0f;
        };

        void BuildBoxFaces(const FVector3& Center, const FQuat& Rotation, const FVector3& HalfExtent, FBoxFace (&OutFaces)[6])
        {
            const FVector3 Axes[3] =
            {
                Rotation * FVector3(1.0f, 0.0f, 0.0f),
                Rotation * FVector3(0.0f, 1.0f, 0.0f),
                Rotation * FVector3(0.0f, 0.0f, 1.0f),
            };

            int32 Index = 0;
            for (int32 Axis = 0; Axis < 3; ++Axis)
            {
                const int32 U = (Axis + 1) % 3;
                const int32 V = (Axis + 2) % 3;

                for (int32 Side = 0; Side < 2; ++Side)
                {
                    const float Sign = (Side == 0) ? 1.0f : -1.0f;

                    FBoxFace& Face = OutFaces[Index++];
                    Face.Axis   = Axis;
                    Face.Sign   = Sign;
                    Face.Normal = Axes[Axis] * Sign;
                    Face.Center = Center + Face.Normal * HalfExtent[Axis];
                    Face.HalfU  = Axes[U] * HalfExtent[U];
                    Face.HalfV  = Axes[V] * HalfExtent[V];
                }
            }
        }

        constexpr float kMinColliderExtent = 0.001f;

        FVector3 UnitAxis(int32 Axis)
        {
            FVector3 Result(0.0f);
            Result[Axis] = 1.0f;
            return Result;
        }

        const char* BoxFaceName(int32 Axis, float Sign)
        {
            static const char* Names[6] = { "-X", "+X", "-Y", "+Y", "-Z", "+Z" };
            return Names[Axis * 2 + (Sign > 0.0f ? 1 : 0)];
        }

        FVisualizerHandleStyle FaceStyle(bool bIsTrigger)
        {
            FVisualizerHandleStyle Style;
            Style.Color = bIsTrigger ? FVector4(0.25f, 0.90f, 0.45f, 1.0f) : FVector4(0.30f, 0.62f, 1.00f, 1.0f);
            Style.Shape = EVisualizerHandleShape::Square;
            Style.PixelRadius = 4.5f;
            Style.GrabPixelRadius = 9.0f;
            Style.SurfaceOpacity = 0.07f;
            Style.Tooltip = "Drag to move this face; the opposite face stays put.";
            return Style;
        }

        FVisualizerHandleStyle ScalarStyle(const char* Tooltip)
        {
            FVisualizerHandleStyle Style;
            Style.Shape = EVisualizerHandleShape::Diamond;
            Style.PixelRadius = 6.0f;
            Style.GrabPixelRadius = 12.0f;
            Style.Tooltip = Tooltip;
            return Style;
        }

        // Screen-right at the point, so a radius handle stays grabbable from any camera angle.
        FVector3 ScreenRightAt(const FVisualizerView& View, const FVector3& Point)
        {
            const FVector3 ToCamera = View.DirectionToCamera(Point);
            const FVector3 Right = Math::Cross(View.CameraUp, ToCamera);
            return Math::LengthSquared(Right) > 1e-8f ? Math::Normalize(Right) : View.CameraRight;
        }

        constexpr FVector4 kAudioInner(0.25f, 0.85f, 1.00f, 1.0f);
        constexpr FVector4 kAudioOuter(0.20f, 0.45f, 0.85f, 1.0f);
        constexpr FVector4 kAudioFlat(0.75f, 0.75f, 0.80f, 1.0f);
        constexpr FVector4 kAudioActive(0.40f, 1.00f, 0.80f, 1.0f);
        constexpr FVector4 kAudioAsset(1.00f, 0.78f, 0.30f, 1.0f);

        void DrawAudioAttenuation(IPrimitiveDrawInterface* PDI, const FVector3& Center, const FQuat& Rotation,
            const SAudioAttenuation& Attenuation, bool bSpatialized, bool bPlaying)
        {
            if (!bSpatialized || Attenuation.Model == EAudioAttenuationModel::None)
            {
                PDI->DrawSphere(Center, 0.3f, kAudioFlat, 12, 2.0f, true, 0.0f);
                return;
            }

            const FVector4 Inner = bPlaying ? kAudioActive : kAudioInner;
            const FVector4 Outer = bPlaying ? kAudioActive : kAudioOuter;

            PDI->DrawSphere(Center, Attenuation.MinDistance, Inner, 24, 1.5f, true, 0.0f);
            PDI->DrawSphere(Center, Attenuation.MaxDistance, Outer, 32, 2.0f, true, 0.0f);

            if (Attenuation.ConeOuterAngle >= 360.0f)
            {
                return;
            }

            // Cone angles are full widths, while DrawCone takes the half angle from the axis.
            const FVector3 Forward = Rotation * FViewVolume::ForwardAxis;
            PDI->DrawCone(Center, Forward, Math::Radians(Attenuation.ConeOuterAngle * 0.5f), Attenuation.MaxDistance, Outer, 20, 3, 1.5f, true, 0.0f);

            if (Attenuation.ConeInnerAngle < Attenuation.ConeOuterAngle)
            {
                PDI->DrawCone(Center, Forward, Math::Radians(Attenuation.ConeInnerAngle * 0.5f), Attenuation.MaxDistance, Inner, 20, 3, 1.5f, true, 0.0f);
            }
        }

        void DrawAudioAttenuationHandles(FComponentVisualizerContext& Context, SAudioAttenuationSettings& Settings,
            const FVector3& Center, const FQuat& Rotation, bool bSpatialized, FName EditLabel)
        {
            if (!bSpatialized)
            {
                return;
            }

            const SAudioAttenuation& Resolved = Settings.Resolve();
            if (Resolved.Model == EAudioAttenuationModel::None)
            {
                return;
            }

            const FVector3 Axis = ScreenRightAt(Context.View, Center);

            // Editing a shared asset through one emitter would silently retune every other user of it.
            if (Settings.AttenuationSettings != nullptr && !Settings.bOverrideAttenuation)
            {
                const FString AssetName = Settings.AttenuationSettings->GetName().ToString();
                Context.Label(Center + Axis * Resolved.MaxDistance, kAudioAsset, "%s", AssetName.c_str());
                return;
            }

            SAudioAttenuation& Editable = Settings.Overrides;

            const FVisualizerHandleResult MinResult = Context.AxisHandle(0, Center + Axis * Editable.MinDistance, Axis,
                ScalarStyle("Drag to set the full-gain distance."));

            if (MinResult.bChanged)
            {
                Context.NameEdit(EditLabel);
                Editable.MinDistance = Math::Clamp(Editable.MinDistance + MinResult.ScalarDelta, 0.0f, Editable.MaxDistance);
                Context.MarkDirty();
            }

            const FVisualizerHandleResult MaxResult = Context.AxisHandle(1, Center + Axis * Editable.MaxDistance, Axis,
                ScalarStyle("Drag to set the distance the falloff reaches silence."));

            if (MaxResult.bChanged)
            {
                Context.NameEdit(EditLabel);
                Editable.MaxDistance = Math::Max(Editable.MaxDistance + MaxResult.ScalarDelta, Editable.MinDistance);
                Context.MarkDirty();
            }

            if (MinResult.bHovered || MinResult.bActive)
            {
                Context.Measurement(Center, Center + Axis * Editable.MinDistance, kAudioInner, "min %.2f m", Editable.MinDistance);
            }
            else if (MaxResult.bHovered || MaxResult.bActive)
            {
                Context.Measurement(Center, Center + Axis * Editable.MaxDistance, kAudioOuter, "max %.2f m", Editable.MaxDistance);
            }

            if (Editable.ConeOuterAngle >= 360.0f)
            {
                return;
            }

            const FVector3 Forward = Rotation * FViewVolume::ForwardAxis;

            FVector3 Side = Math::Cross(Forward, Context.View.DirectionToCamera(Center));
            if (Math::LengthSquared(Side) < 1e-8f)
            {
                Side = Math::Cross(Forward, Context.View.CameraUp);
            }
            Side = Math::Normalize(Side);

            const FVector3 PlaneNormal = Math::Normalize(Math::Cross(Forward, Side));

            auto ConeHandle = [&](uint32 ID, float& FullAngle, float SideSign, const char* Tooltip)
            {
                const float Half = Math::Radians(FullAngle * 0.5f);
                const FVector3 Rim = Center
                                   + Forward * (Editable.MaxDistance * Math::Cos(Half))
                                   + Side * (SideSign * Editable.MaxDistance * Math::Sin(Half));

                const FVisualizerHandleResult Result = Context.PlaneHandle(ID, Rim, PlaneNormal, ScalarStyle(Tooltip));
                if (Result.bChanged)
                {
                    const FVector3 Local = Result.Position - Center;
                    const float Across = Math::Abs(Math::Dot(Local, Side));
                    const float Along = Math::Dot(Local, Forward);

                    Context.NameEdit(EditLabel);
                    FullAngle = Math::Clamp(Math::Degrees(std::atan2(Across, Along)) * 2.0f, 1.0f, 360.0f);
                    Context.MarkDirty();
                }

                return Result;
            };

            const FVisualizerHandleResult Outer = ConeHandle(2, Editable.ConeOuterAngle, 1.0f, "Drag to set the outer cone angle.");
            const FVisualizerHandleResult Inner = ConeHandle(3, Editable.ConeInnerAngle, -1.0f, "Drag to set the inner cone angle.");

            if (Editable.ConeInnerAngle > Editable.ConeOuterAngle)
            {
                Editable.ConeInnerAngle = Editable.ConeOuterAngle;
            }

            if (Outer.bHovered || Outer.bActive)
            {
                Context.Label(Center + Forward * (Editable.MaxDistance * 0.55f), kAudioOuter, "outer %.0f deg", Editable.ConeOuterAngle);
            }
            else if (Inner.bHovered || Inner.bActive)
            {
                Context.Label(Center + Forward * (Editable.MaxDistance * 0.55f), kAudioInner, "inner %.0f deg", Editable.ConeInnerAngle);
            }
        }

        void LabelAudioEmitter(FComponentVisualizerContext& Context, const FVector3& Center, const char* Name,
            EAudioBus Bus, float Volume, bool bSpatialized, bool bPlaying)
        {
            const FVector4 Color = bPlaying ? kAudioActive : kAudioFlat;
            const FVector3 Anchor = Center + Context.View.CameraUp * (Context.View.WorldPerPixelAt(Center) * 30.0f);

            Context.Label(Anchor, Color, "%s   %s   x%.2f%s", Name, ToString(Bus), Volume, bSpatialized ? "" : "   2D");
        }

        // Two rings joined by vertical spokes, optionally tapered, shared by the cylinder visualizers.
        void DrawWireCylinder(IPrimitiveDrawInterface* PDI, const FVector3& Center, const FQuat& Rot,
            float TopRadius, float BottomRadius, float HalfHeight, const FVector4& Color, float Thickness)
        {
            const FVector3 Up     = Rot * FVector3(0.0f, 1.0f, 0.0f);
            const FVector3 Right  = Rot * FVector3(1.0f, 0.0f, 0.0f);
            const FVector3 Fwd    = Rot * FVector3(0.0f, 0.0f, 1.0f);
            const FVector3 Top    = Center + Up * HalfHeight;
            const FVector3 Bottom = Center - Up * HalfHeight;

            constexpr int kSegments = 24;
            FVector3 PrevTop, PrevBottom;
            for (int i = 0; i <= kSegments; ++i)
            {
                const float A  = (float(i) / float(kSegments)) * Math::TwoPi<float>();
                const FVector3 Dir = Right * Math::Cos(A) + Fwd * Math::Sin(A);
                const FVector3 T = Top    + Dir * TopRadius;
                const FVector3 B = Bottom + Dir * BottomRadius;
                if (i > 0)
                {
                    PDI->DrawLine(PrevTop,    T, Color, Thickness, true, 0.0f);
                    PDI->DrawLine(PrevBottom, B, Color, Thickness, true, 0.0f);
                    if ((i % 6) == 0)
                    {
                        PDI->DrawLine(T, B, Color, Thickness, true, 0.0f);
                    }
                }
                PrevTop = T;
                PrevBottom = B;
            }
        }
    }

    CStruct* CComponentVisualizer_PointLight::GetSupportedComponentType() const
    {
        return SPointLightComponent::StaticStruct();
    }

    void CComponentVisualizer_PointLight::Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const SPointLightComponent& PointLight = Registry.Get<SPointLightComponent>(Entity);
        const STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);
        
        PDI->DrawSphere(Transform.GetWorldLocationCached(), PointLight.Attenuation, 
            FVector4(PointLight.LightColor, 1.0f), 32, 1.0f, true, 0.0f);
    }

    void CComponentVisualizer_PointLight::DrawVisualization(FComponentVisualizerContext& Context)
    {
        SPointLightComponent& Light = Context.Get<SPointLightComponent>();
        const STransformComponent& Transform = Context.Get<STransformComponent>();

        const FVector3 Center = Transform.GetWorldLocationCached();
        const FVector4 Color(Light.LightColor, 1.0f);
        const FVector3 Axis = ScreenRightAt(Context.View, Center);

        const FVisualizerHandleResult Result = Context.AxisHandle(0, Center + Axis * Light.Attenuation, Axis,
            ScalarStyle("Drag to set the light radius."));

        if (Result.bChanged)
        {
            Context.NameEdit("Set Light Radius");
            Light.Attenuation = Math::Max(Light.Attenuation + Result.ScalarDelta, 0.01f);
            Context.MarkDirty();
        }

        if (Result.bHovered || Result.bActive)
        {
            Context.Measurement(Center, Center + Axis * Light.Attenuation, Color, "%.2f m", Light.Attenuation);
        }
        else
        {
            Context.Label(Center + Context.View.CameraUp * (Context.View.WorldPerPixelAt(Center) * 24.0f), Color,
                "%.1f m   %.0f lm", Light.Attenuation, Light.Intensity);
        }
    }


    CStruct* CComponentVisualizer_SpotLight::GetSupportedComponentType() const
    {
        return SSpotLightComponent::StaticStruct();
    }

    void CComponentVisualizer_SpotLight::Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const SSpotLightComponent& SpotLight    = Registry.Get<SSpotLightComponent>(Entity);
        const STransformComponent& Transform    = Registry.Get<STransformComponent>(Entity);
        FVector3 Forward                       = Transform.GetWorldRotationCached() * FViewVolume::ForwardAxis;

        // The cone opens along the transform forward, which is the direction it lights.
        PDI->DrawCone(Transform.GetWorldLocationCached(), Forward, Math::Radians(SpotLight.OuterConeAngle), SpotLight.Attenuation, FVector4(SpotLight.LightColor, 1.0f));
        PDI->DrawCone(Transform.GetWorldLocationCached(), Forward, Math::Radians(SpotLight.InnerConeAngle), SpotLight.Attenuation, FVector4(SpotLight.LightColor, 1.0f));
    }

    void CComponentVisualizer_SpotLight::DrawVisualization(FComponentVisualizerContext& Context)
    {
        SSpotLightComponent& Light = Context.Get<SSpotLightComponent>();
        const STransformComponent& Transform = Context.Get<STransformComponent>();

        const FVector3 Apex = Transform.GetWorldLocationCached();
        const FVector3 Forward = Transform.GetWorldRotationCached() * FViewVolume::ForwardAxis;
        const FVector4 Color(Light.LightColor, 1.0f);

        FVector3 Side = Math::Cross(Forward, Context.View.DirectionToCamera(Apex));
        if (Math::LengthSquared(Side) < 1e-8f)
        {
            Side = Math::Cross(Forward, Context.View.CameraUp);
        }
        Side = Math::Normalize(Side);

        const FVector3 PlaneNormal = Math::Normalize(Math::Cross(Forward, Side));

        auto ConeHandle = [&](uint32 ID, float& AngleDegrees, float SideSign, const char* Tooltip)
        {
            const float Radians = Math::Radians(AngleDegrees);
            const FVector3 Rim = Apex
                               + Forward * (Light.Attenuation * Math::Cos(Radians))
                               + Side * (SideSign * Light.Attenuation * Math::Sin(Radians));

            const FVisualizerHandleResult Result = Context.PlaneHandle(ID, Rim, PlaneNormal, ScalarStyle(Tooltip));
            if (Result.bChanged)
            {
                const FVector3 Local = Result.Position - Apex;
                const float Along = Math::Dot(Local, Forward);
                const float Across = Math::Abs(Math::Dot(Local, Side));

                Context.NameEdit("Set Cone Angle");
                AngleDegrees = Math::Clamp(Math::Degrees(std::atan2(Across, Along)), 1.0f, 89.0f);
                Context.MarkDirty();
            }

            return Result;
        };

        const FVisualizerHandleResult Outer = ConeHandle(0, Light.OuterConeAngle, 1.0f, "Drag to set the outer cone angle.");
        const FVisualizerHandleResult Inner = ConeHandle(1, Light.InnerConeAngle, -1.0f, "Drag to set the inner cone angle.");

        if (Light.InnerConeAngle > Light.OuterConeAngle)
        {
            Light.InnerConeAngle = Light.OuterConeAngle;
        }

        const FVisualizerHandleResult Range = Context.AxisHandle(2, Apex + Forward * Light.Attenuation, Forward,
            ScalarStyle("Drag to set the light range."));

        if (Range.bChanged)
        {
            Context.NameEdit("Set Light Range");
            Light.Attenuation = Math::Max(Light.Attenuation + Range.ScalarDelta, 0.01f);
            Context.MarkDirty();
        }

        if (Outer.bHovered || Outer.bActive)
        {
            Context.Label(Apex + Forward * (Light.Attenuation * 0.5f), Color, "outer %.1f deg", Light.OuterConeAngle);
        }
        else if (Inner.bHovered || Inner.bActive)
        {
            Context.Label(Apex + Forward * (Light.Attenuation * 0.5f), Color, "inner %.1f deg", Light.InnerConeAngle);
        }
        else if (Range.bHovered || Range.bActive)
        {
            Context.Measurement(Apex, Apex + Forward * Light.Attenuation, Color, "%.2f m", Light.Attenuation);
        }
    }


    CStruct* CComponentVisualizer_DirectionalLight::GetSupportedComponentType() const
    {
        return SDirectionalLightComponent::StaticStruct();
    }

    void CComponentVisualizer_DirectionalLight::Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const auto& Light       = Registry.Get<SDirectionalLightComponent>(Entity);
        const auto& Transform   = Registry.Get<STransformComponent>(Entity);
        
        PDI->DrawArrow(Transform.GetWorldLocationCached(), -Light.Direction, 1.5f, FColor::Yellow, 4.0f);
    }

    CStruct* CComponentVisualizer_SphereCollider::GetSupportedComponentType() const
    {
        return SSphereColliderComponent::StaticStruct();
    }

    void CComponentVisualizer_SphereCollider::Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const SSphereColliderComponent& Sphere = Registry.Get<SSphereColliderComponent>(Entity);
        const STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);

        const FVector3 Center = Transform.GetWorldLocationCached() + Transform.GetWorldRotationCached() * Sphere.TranslationOffset;
        PDI->DrawSphere(Center, Sphere.Radius * Transform.MaxScale(), ColliderColor(Sphere.bIsTrigger), 8, 3.5f, true, 0.0f);
    }

    void CComponentVisualizer_SphereCollider::DrawVisualization(FComponentVisualizerContext& Context)
    {
        SSphereColliderComponent& Sphere = Context.Get<SSphereColliderComponent>();
        const STransformComponent& Transform = Context.Get<STransformComponent>();

        const float Scale = Math::Max(Transform.MaxScale(), 1e-4f);
        const FVector3 Center = Transform.GetWorldLocationCached() + Transform.GetWorldRotationCached() * Sphere.TranslationOffset;
        const FVector3 Axis = ScreenRightAt(Context.View, Center);

        const FVisualizerHandleResult Result = Context.AxisHandle(0, Center + Axis * (Sphere.Radius * Scale), Axis,
            ScalarStyle("Drag to set the collider radius."));

        if (Result.bChanged)
        {
            Context.NameEdit("Resize Sphere Collider");
            Sphere.Radius = Math::Max(Sphere.Radius + Result.ScalarDelta / Scale, kMinColliderExtent);
            Context.MarkDirty();
        }

        if (Result.bHovered || Result.bActive)
        {
            Context.Measurement(Center, Center + Axis * (Sphere.Radius * Scale), ColliderColor(Sphere.bIsTrigger),
                "r %.3f m", Sphere.Radius);
        }
    }


    CStruct* CComponentVisualizer_BoxCollider::GetSupportedComponentType() const
    {
        return SBoxColliderComponent::StaticStruct();
    }

    void CComponentVisualizer_BoxCollider::Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const SBoxColliderComponent& Box = Registry.Get<SBoxColliderComponent>(Entity);
        const STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);

        const FVector3 Center  = Transform.GetWorldLocationCached() + Transform.GetWorldRotationCached() * Box.TranslationOffset;
        const FQuat    WorldRot = Transform.GetWorldRotationCached() * FQuat(Box.RotationOffset);
        PDI->DrawBox(Center, Box.HalfExtent * Transform.GetWorldScaleCached(), WorldRot, ColliderColor(Box.bIsTrigger), 3.5f, true, 0.0f);
    }

    void CComponentVisualizer_BoxCollider::DrawVisualization(FComponentVisualizerContext& Context)
    {
        SBoxColliderComponent& Box = Context.Get<SBoxColliderComponent>();
        const STransformComponent& Transform = Context.Get<STransformComponent>();

        const FQuat EntityRotation = Transform.GetWorldRotationCached();
        const FQuat LocalRotation = FQuat(Box.RotationOffset);
        const FVector3 Scale = Transform.GetWorldScaleCached();
        const FVector3 Center = Transform.GetWorldLocationCached() + EntityRotation * Box.TranslationOffset;

        FBoxFace Faces[6];
        BuildBoxFaces(Center, EntityRotation * LocalRotation, Box.HalfExtent * Scale, Faces);

        const FVisualizerHandleStyle Style = FaceStyle(Box.bIsTrigger);

        for (int32 FaceIndex = 0; FaceIndex < 6; ++FaceIndex)
        {
            const FBoxFace& Face = Faces[FaceIndex];
            const FVisualizerHandleResult Result =
                Context.FaceHandle((uint32)FaceIndex, Face.Center, Face.Normal, Face.HalfU, Face.HalfV, Style);

            if (!Result.bChanged)
            {
                continue;
            }

            Context.NameEdit("Resize Box Collider");

            const float AxisScale = Math::Max(Math::Abs(Scale[Face.Axis]), 1e-4f);
            const float Desired = Box.HalfExtent[Face.Axis] + (Result.ScalarDelta * 0.5f) / AxisScale;
            const float Clamped = Math::Max(Desired, kMinColliderExtent);
            const float HalfDelta = Clamped - Box.HalfExtent[Face.Axis];

            Box.HalfExtent[Face.Axis] = Clamped;

            // Shift the box by half of what the face moved, so the opposite face stays where it was.
            Box.TranslationOffset += (LocalRotation * (UnitAxis(Face.Axis) * Face.Sign)) * (HalfDelta * AxisScale);

            Context.MarkDirty();
        }

        const int32 SelectedFace = Context.GetSelectedSubElement();
        if (SelectedFace < 0 || SelectedFace >= 6)
        {
            return;
        }

        FBoxFace Current[6];
        BuildBoxFaces(Transform.GetWorldLocationCached() + EntityRotation * Box.TranslationOffset,
            EntityRotation * LocalRotation, Box.HalfExtent * Scale, Current);

        const FBoxFace& Face = Current[SelectedFace];
        const float WorldSize = Box.HalfExtent[Face.Axis] * Math::Abs(Scale[Face.Axis]) * 2.0f;

        Context.Measurement(Face.Center - Face.Normal * WorldSize, Face.Center,
            FVector4(1.0f, 0.85f, 0.35f, 1.0f), "%s  %.3f m", BoxFaceName(Face.Axis, Face.Sign), WorldSize);

        ImGui::PushID((int)Context.GetEntity().GetPacked());
        if (Context.BeginPanel("##BoxColliderFace", Face.Center))
        {
            ImGui::TextUnformatted(BoxFaceName(Face.Axis, Face.Sign));
            ImGui::SameLine();
            ImGui::TextDisabled("face");
            ImGui::Separator();

            float LocalSize = Box.HalfExtent[Face.Axis] * 2.0f;
            ImGui::SetNextItemWidth(120.0f);
            const bool bSizeEdited = ImGui::DragFloat("##Size", &LocalSize, 0.01f, kMinColliderExtent * 2.0f, 100000.0f, "%.3f m");
            if (ImGui::IsItemActivated())
            {
                Context.BeginEdit("Resize Box Collider");
            }
            if (bSizeEdited)
            {
                Box.HalfExtent[Face.Axis] = Math::Max(LocalSize * 0.5f, kMinColliderExtent);
                Context.MarkDirty();
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                Context.EndEdit();
            }

            bool bTrigger = Box.bIsTrigger;
            if (ImGui::Checkbox("Trigger", &bTrigger))
            {
                Context.BeginEdit("Toggle Collider Trigger");
                Box.bIsTrigger = bTrigger;
                Context.MarkDirty();
                Context.EndEdit();
            }

            if (ImGui::SmallButton("Center"))
            {
                Context.BeginEdit("Center Box Collider");
                Box.TranslationOffset = FVector3(0.0f);
                Context.MarkDirty();
                Context.EndEdit();
            }

            ImGui::SameLine();
            if (ImGui::SmallButton("Done"))
            {
                Context.ClearSubElementSelection();
            }

            Context.EndPanel();
        }
        ImGui::PopID();
    }


    CStruct* CComponentVisualizer_CapsuleCollider::GetSupportedComponentType() const
    {
        return SCapsuleColliderComponent::StaticStruct();
    }

    void CComponentVisualizer_CapsuleCollider::Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const SCapsuleColliderComponent& Capsule = Registry.Get<SCapsuleColliderComponent>(Entity);
        const STransformComponent& Transform     = Registry.Get<STransformComponent>(Entity);

        // DrawCapsule wants the two cylinder-axis endpoints (caps tangent there), Y-aligned in local space.
        const float Scale     = Transform.MaxScale();
        const FQuat WorldRot  = Transform.GetWorldRotationCached() * FQuat(Capsule.RotationOffset);
        const FVector3 Center = Transform.GetWorldLocationCached() + Transform.GetWorldRotationCached() * Capsule.TranslationOffset;
        const FVector3 Axis   = WorldRot * FVector3(0.0f, Capsule.HalfHeight * Scale, 0.0f);

        PDI->DrawCapsule(Center - Axis, Center + Axis, Capsule.Radius * Scale, ColliderColor(Capsule.bIsTrigger), 12, 3.5f, true, 0.0f);
    }

    void CComponentVisualizer_CapsuleCollider::DrawVisualization(FComponentVisualizerContext& Context)
    {
        SCapsuleColliderComponent& Capsule = Context.Get<SCapsuleColliderComponent>();
        const STransformComponent& Transform = Context.Get<STransformComponent>();

        const float Scale = Math::Max(Transform.MaxScale(), 1e-4f);
        const FQuat EntityRotation = Transform.GetWorldRotationCached();
        const FQuat LocalRotation = FQuat(Capsule.RotationOffset);
        const FQuat WorldRotation = EntityRotation * LocalRotation;

        const FVector3 Center = Transform.GetWorldLocationCached() + EntityRotation * Capsule.TranslationOffset;
        const FVector3 Up = WorldRotation * FVector3(0.0f, 1.0f, 0.0f);
        const FVector4 Color = ColliderColor(Capsule.bIsTrigger);

        const FVector3 RadiusAxis = ScreenRightAt(Context.View, Center);
        const FVisualizerHandleResult RadiusResult = Context.AxisHandle(0, Center + RadiusAxis * (Capsule.Radius * Scale),
            RadiusAxis, ScalarStyle("Drag to set the capsule radius."));

        if (RadiusResult.bChanged)
        {
            Context.NameEdit("Resize Capsule Collider");
            Capsule.Radius = Math::Max(Capsule.Radius + RadiusResult.ScalarDelta / Scale, kMinColliderExtent);
            Context.MarkDirty();
        }

        for (int32 Side = 0; Side < 2; ++Side)
        {
            const float Sign = (Side == 0) ? 1.0f : -1.0f;
            const FVector3 Cap = Center + Up * (Sign * Capsule.HalfHeight * Scale);

            const FVisualizerHandleResult Result = Context.AxisHandle((uint32)(1 + Side), Cap, Up * Sign,
                ScalarStyle("Drag to move this end; the other end stays put."));

            if (!Result.bChanged)
            {
                continue;
            }

            Context.NameEdit("Resize Capsule Collider");

            const float Desired = Capsule.HalfHeight + (Result.ScalarDelta * 0.5f) / Scale;
            const float Clamped = Math::Max(Desired, kMinColliderExtent);
            const float HalfDelta = Clamped - Capsule.HalfHeight;

            Capsule.HalfHeight = Clamped;
            Capsule.TranslationOffset += (LocalRotation * FVector3(0.0f, Sign, 0.0f)) * (HalfDelta * Scale);

            Context.MarkDirty();
        }

        if (RadiusResult.bHovered || RadiusResult.bActive)
        {
            Context.Measurement(Center, Center + RadiusAxis * (Capsule.Radius * Scale), Color, "r %.3f m", Capsule.Radius);
        }
        else
        {
            const float Total = (Capsule.HalfHeight + Capsule.Radius) * 2.0f * Scale;
            Context.Measurement(Center - Up * (Total * 0.5f), Center + Up * (Total * 0.5f), Color, "h %.3f m", Total);
        }
    }


    CStruct* CComponentVisualizer_CylinderCollider::GetSupportedComponentType() const
    {
        return SCylinderColliderComponent::StaticStruct();
    }

    void CComponentVisualizer_CylinderCollider::Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const SCylinderColliderComponent& Cyl = Registry.Get<SCylinderColliderComponent>(Entity);
        const STransformComponent& Transform  = Registry.Get<STransformComponent>(Entity);

        const float Scale     = Transform.MaxScale();
        const FQuat WorldRot  = Transform.GetWorldRotationCached() * FQuat(Cyl.RotationOffset);
        const FVector3 Center = Transform.GetWorldLocationCached() + Transform.GetWorldRotationCached() * Cyl.TranslationOffset;
        const float Radius    = Cyl.Radius * Scale;

        DrawWireCylinder(PDI, Center, WorldRot, Radius, Radius, Cyl.HalfHeight * Scale, ColliderColor(Cyl.bIsTrigger), 3.5f);
    }

    CStruct* CComponentVisualizer_CharacterPhysics::GetSupportedComponentType() const
    {
        return SCharacterPhysicsComponent::StaticStruct();
    }

    void CComponentVisualizer_CharacterPhysics::Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const SCharacterPhysicsComponent& Character = Registry.Get<SCharacterPhysicsComponent>(Entity);
        const STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);

        // Matches Box3D, where Start and End are the cylinder-axis endpoints and Radius scales by MaxScale.
        const FQuat Rotation = Transform.GetWorldRotationCached();
        const FVector3 Center = Transform.GetWorldLocationCached() + Rotation * Character.TranslationOffset;
        const FVector3 Axis = Rotation * FVector3(0.0f, Character.HalfHeight, 0.0f);
        const FVector3 Start = Center - Axis;
        const FVector3 End   = Center + Axis;

        PDI->DrawCapsule(Start, End, Character.Radius * Transform.MaxScale(), FColor::Blue, 12, 2.0f, true, 0.0f);
    }

    CStruct* CComponentVisualizer_RigidBody::GetSupportedComponentType() const
    {
        return SRigidBodyComponent::StaticStruct();
    }

    void CComponentVisualizer_RigidBody::Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const SRigidBodyComponent& Body      = Registry.Get<SRigidBodyComponent>(Entity);
        const STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);

        if (Math::Dot(Body.CenterOfMassOffset, Body.CenterOfMassOffset) <= 0.0f)
        {
            return;
        }

        const FVector3 WorldCOM = Transform.GetWorldLocationCached() + Transform.GetWorldRotationCached() * (Body.CenterOfMassOffset * Transform.GetWorldScaleCached());

        PDI->DrawSphere(WorldCOM, 0.08f, FVector4(1.0f, 0.0f, 1.0f, 1.0f), 12, 2.0f, false, 0.0f);
        PDI->DrawLine(Transform.GetWorldLocationCached(), WorldCOM, FVector4(1.0f, 0.0f, 1.0f, 1.0f), 2.0f, false, 0.0f);
    }

    CStruct* CComponentVisualizer_Camera::GetSupportedComponentType() const
    {
        return SCameraComponent::StaticStruct();
    }

    void CComponentVisualizer_Camera::Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const auto& Transform   = Registry.Get<STransformComponent>(Entity);
        const auto& Camera      = Registry.Get<SCameraComponent>(Entity);

        // The cached ViewVolume only refreshes at runtime, so rebuild the view-projection from the transform.
        constexpr float GizmoFar = 25.0f;
        const FVector3 Location = Transform.GetWorldLocationCached();
        const FQuat    Rotation = Transform.GetWorldRotationCached();
        const FVector3 Forward  = Rotation * FViewVolume::ForwardAxis;
        const FVector3 Up       = Rotation * FViewVolume::UpAxis;

        FViewVolume Volume(Camera.GetFOV(), Camera.GetAspectRatio(), Camera.GetViewVolume().GetNear(), GizmoFar);
        Volume.SetView(Location, Forward, Up);

        // Reverse-Z Vulkan NDC puts the near plane at z=1 and the far plane at z=0.
        PDI->DrawFrustum(Volume.GetViewProjectionMatrix(), 1.0f, 0.0f, FColor::White, 4.0f);
        PDI->DrawArrow(Location, Forward, 3.5f, FColor::Green, 4.0f);
    }

    CStruct* CComponentVisualizer_Decal::GetSupportedComponentType() const
    {
        return SDecalComponent::StaticStruct();
    }

    void CComponentVisualizer_Decal::Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const SDecalComponent& Decal         = Registry.Get<SDecalComponent>(Entity);
        const STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);

        const FVector3 Location  = Transform.GetWorldLocationCached();
        const FQuat    Rotation  = Transform.GetWorldRotationCached();
        const FVector3 HalfExtent = Decal.Size * 0.5f * Transform.GetWorldScaleCached();

        // Projection volume + the -Z axis the material projects along.
        const FVector4 BoxColor(1.0f, 0.2f, 0.8f, 1.0f);
        PDI->DrawBox(Location, HalfExtent, Rotation, BoxColor, 2.5f, true, 0.0f);

        const FVector3 ProjectDir = Rotation * FVector3(0.0f, 0.0f, -1.0f);
        PDI->DrawArrow(Location, ProjectDir, HalfExtent.z, FVector4(1.0f, 0.85f, 0.2f, 1.0f), 3.0f);
    }

    void CComponentVisualizer_Decal::DrawVisualization(FComponentVisualizerContext& Context)
    {
        SDecalComponent& Decal = Context.Get<SDecalComponent>();
        const STransformComponent& Transform = Context.Get<STransformComponent>();

        const FVector3 Scale = Transform.GetWorldScaleCached();
        const FVector3 Center = Transform.GetWorldLocationCached();
        const FQuat Rotation = Transform.GetWorldRotationCached();

        FBoxFace Faces[6];
        BuildBoxFaces(Center, Rotation, Decal.Size * 0.5f * Scale, Faces);

        FVisualizerHandleStyle Style;
        Style.Color = FVector4(1.0f, 0.35f, 0.85f, 1.0f);
        Style.Shape = EVisualizerHandleShape::Square;
        Style.PixelRadius = 4.5f;
        Style.GrabPixelRadius = 9.0f;
        Style.SurfaceOpacity = 0.07f;
        Style.Tooltip = "Drag to resize the projection volume.";

        for (int32 FaceIndex = 0; FaceIndex < 6; ++FaceIndex)
        {
            const FBoxFace& Face = Faces[FaceIndex];
            const FVisualizerHandleResult Result =
                Context.FaceHandle((uint32)FaceIndex, Face.Center, Face.Normal, Face.HalfU, Face.HalfV, Style);

            if (!Result.bChanged)
            {
                continue;
            }

            Context.NameEdit("Resize Decal");

            // A decal has no local offset, so both faces move and the volume stays centered on the entity.
            const float AxisScale = Math::Max(Math::Abs(Scale[Face.Axis]), 1e-4f);
            Decal.Size[Face.Axis] = Math::Max(Decal.Size[Face.Axis] + (Result.ScalarDelta * 2.0f) / AxisScale, kMinColliderExtent);

            Context.MarkDirty();
        }

        const int32 SelectedFace = Context.GetSelectedSubElement();
        if (SelectedFace >= 0 && SelectedFace < 6)
        {
            const FBoxFace& Face = Faces[SelectedFace];
            Context.Label(Face.Center, Style.Color, "%s  %.3f m", BoxFaceName(Face.Axis, Face.Sign), Decal.Size[Face.Axis]);
        }
    }


    CStruct* CComponentVisualizer_ReflectionProbe::GetSupportedComponentType() const
    {
        return SReflectionProbeComponent::StaticStruct();
    }

    void CComponentVisualizer_ReflectionProbe::Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const SReflectionProbeComponent& Probe = Registry.Get<SReflectionProbeComponent>(Entity);
        const STransformComponent& Transform   = Registry.Get<STransformComponent>(Entity);

        const FVector3 Location = Transform.GetWorldLocationCached();
        const FQuat    Rotation = Transform.GetWorldRotationCached();

        // Dimmed when switched off, so a disabled probe shows where it sits without reading as active.
        const float Alpha = Probe.bEnabled ? 1.0f : 0.35f;

        // The gap between the shells is the cross-fade band, which is what matters for overlapping probes.
        const float InnerScale = Math::Clamp(1.0f - Probe.BlendDistance, 0.0f, 1.0f);

        // Always probes cost six scene renders per turn, so one left on by accident is worth seeing.
        const bool bAlways = (Probe.UpdateMode == EReflectionProbeUpdateMode::Always);
        const FVector3 Hue = bAlways ? FVector3(1.00f, 0.45f, 0.25f) : FVector3(0.30f, 0.85f, 1.00f);

        const FVector4 OuterColor(Hue.x, Hue.y, Hue.z, Alpha);
        const FVector4 InnerColor(Hue.x, Hue.y, Hue.z, Alpha * 0.45f);

        if (Probe.Shape == EReflectionProbeShape::Sphere)
        {
            // Sphere mode ignores Y and Z, and extraction collapses them too, so the shader test matches.
            const float Radius = Math::Max(Probe.Extent.x, 0.001f) * Transform.MaxScale();
            PDI->DrawSphere(Location, Radius, OuterColor, 24, 2.0f, true, 0.0f);
            if (InnerScale > 0.01f)
            {
                PDI->DrawSphere(Location, Radius * InnerScale, InnerColor, 24, 1.0f, true, 0.0f);
            }
        }
        else
        {
            const FVector3 HalfExtent = Math::Max(Probe.Extent, FVector3(0.001f)) * Transform.GetWorldScaleCached();
            PDI->DrawBox(Location, HalfExtent, Rotation, OuterColor, 2.0f, true, 0.0f);
            if (InnerScale > 0.01f)
            {
                PDI->DrawBox(Location, HalfExtent * InnerScale, Rotation, InnerColor, 1.0f, true, 0.0f);
            }
        }

        // CaptureOffset can put the origin well away from the entity, so drawing it explains an offset probe.
        const FVector3 CaptureWorld = Location + Rotation * Probe.CaptureOffset;
        const FVector4 CaptureColor(1.00f, 0.80f, 0.25f, Alpha);

        // Scaled off the volume so the marker stays readable on both a 1 m prop probe and a 50 m room.
        const float MarkerRadius = Math::Max(Math::Max(Probe.Extent.x, Probe.Extent.y), Probe.Extent.z)
                                 * Transform.MaxScale() * 0.05f;
        PDI->DrawSphere(CaptureWorld, Math::Max(MarkerRadius, 0.05f), CaptureColor, 12, 2.0f, false, 0.0f);

        if (Math::LengthSquared(Probe.CaptureOffset) > 1e-6f)
        {
            PDI->DrawLine(Location, CaptureWorld, CaptureColor, 1.5f, false, 0.0f);
        }
    }

    CStruct* CComponentVisualizer_TaperedCapsuleCollider::GetSupportedComponentType() const
    {
        return STaperedCapsuleColliderComponent::StaticStruct();
    }

    void CComponentVisualizer_TaperedCapsuleCollider::Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const STaperedCapsuleColliderComponent& TC = Registry.Get<STaperedCapsuleColliderComponent>(Entity);
        const STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);

        const float Scale     = Transform.MaxScale();
        const FQuat WorldRot  = Transform.GetWorldRotationCached() * FQuat(TC.RotationOffset);
        const FVector3 Center = Transform.GetWorldLocationCached() + Transform.GetWorldRotationCached() * TC.TranslationOffset;
        const float TopR  = TC.TopRadius * Scale;
        const float BotR  = TC.BottomRadius * Scale;
        const float HalfH = TC.HalfHeight * Scale;
        const FVector4 Color = ColliderColor(TC.bIsTrigger);
        const FVector3 Up    = WorldRot * FVector3(0.0f, 1.0f, 0.0f);

        DrawWireCylinder(PDI, Center, WorldRot, TopR, BotR, HalfH, Color, 3.5f);
        PDI->DrawSphere(Center + Up * HalfH, TopR, Color, 10, 3.5f, true, 0.0f);
        PDI->DrawSphere(Center - Up * HalfH, BotR, Color, 10, 3.5f, true, 0.0f);
    }

    CStruct* CComponentVisualizer_TaperedCylinderCollider::GetSupportedComponentType() const
    {
        return STaperedCylinderColliderComponent::StaticStruct();
    }

    void CComponentVisualizer_TaperedCylinderCollider::Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const STaperedCylinderColliderComponent& TC = Registry.Get<STaperedCylinderColliderComponent>(Entity);
        const STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);

        const float Scale     = Transform.MaxScale();
        const FQuat WorldRot  = Transform.GetWorldRotationCached() * FQuat(TC.RotationOffset);
        const FVector3 Center = Transform.GetWorldLocationCached() + Transform.GetWorldRotationCached() * TC.TranslationOffset;

        DrawWireCylinder(PDI, Center, WorldRot, TC.TopRadius * Scale, TC.BottomRadius * Scale, TC.HalfHeight * Scale, ColliderColor(TC.bIsTrigger), 3.5f);
    }

    CStruct* CComponentVisualizer_PlaneCollider::GetSupportedComponentType() const
    {
        return SPlaneColliderComponent::StaticStruct();
    }

    void CComponentVisualizer_PlaneCollider::Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const SPlaneColliderComponent& Plane = Registry.Get<SPlaneColliderComponent>(Entity);
        const STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);

        const FVector3 Center = Transform.GetWorldLocationCached();
        const FQuat    Rot    = Transform.GetWorldRotationCached();
        const FVector3 Right  = Rot * FVector3(1.0f, 0.0f, 0.0f);
        const FVector3 Fwd    = Rot * FVector3(0.0f, 0.0f, 1.0f);
        const FVector3 Up     = Rot * FVector3(0.0f, 1.0f, 0.0f);
        const FVector4 Color  = ColliderColor(Plane.bIsTrigger);

        // Collision is effectively infinite, so a fixed patch, a cross and the +Y normal keep it readable.
        constexpr float kHalf = 2.5f;
        const FVector3 Corners[4] =
        {
            Center + Right * kHalf + Fwd * kHalf,
            Center - Right * kHalf + Fwd * kHalf,
            Center - Right * kHalf - Fwd * kHalf,
            Center + Right * kHalf - Fwd * kHalf,
        };
        for (int i = 0; i < 4; ++i)
        {
            PDI->DrawLine(Corners[i], Corners[(i + 1) % 4], Color, 3.0f, true, 0.0f);
        }
        PDI->DrawLine(Center - Right * kHalf, Center + Right * kHalf, Color, 1.5f, true, 0.0f);
        PDI->DrawLine(Center - Fwd * kHalf,   Center + Fwd * kHalf,   Color, 1.5f, true, 0.0f);
        PDI->DrawArrow(Center, Up, 1.5f, FVector4(0.2f, 1.0f, 1.0f, 1.0f), 3.0f, true, 0.0f, 0.2f);
    }

    CStruct* CComponentVisualizer_CompoundCollider::GetSupportedComponentType() const
    {
        return SCompoundColliderComponent::StaticStruct();
    }

    void CComponentVisualizer_CompoundCollider::Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const SCompoundColliderComponent& Comp = Registry.Get<SCompoundColliderComponent>(Entity);
        const STransformComponent& Transform   = Registry.Get<STransformComponent>(Entity);

        const FVector3 WorldLoc = Transform.GetWorldLocationCached();
        const FQuat    WorldRot = Transform.GetWorldRotationCached();
        const float    Scale    = Transform.MaxScale();
        const FVector4 Color    = ColliderColor(Comp.bIsTrigger);

        // Draw each child at its local offset/rotation so designers can see the merged shape they're building.
        for (const SCompoundSubShape& Sub : Comp.Shapes)
        {
            const FVector3 Center = WorldLoc + WorldRot * (Sub.Offset * Scale);
            const FQuat    Rot    = WorldRot * FQuat(Sub.Rotation);
            switch (Sub.Type)
            {
            case ECompoundShapeType::Box:
                PDI->DrawBox(Center, Sub.HalfExtent * Scale, Rot, Color, 3.0f, true, 0.0f);
                break;
            case ECompoundShapeType::Sphere:
                PDI->DrawSphere(Center, Sub.Radius * Scale, Color, 10, 3.0f, true, 0.0f);
                break;
            case ECompoundShapeType::Capsule:
            {
                const FVector3 Axis = Rot * FVector3(0.0f, Sub.HalfHeight * Scale, 0.0f);
                PDI->DrawCapsule(Center - Axis, Center + Axis, Sub.Radius * Scale, Color, 10, 3.0f, true, 0.0f);
                break;
            }
            case ECompoundShapeType::Cylinder:
                DrawWireCylinder(PDI, Center, Rot, Sub.Radius * Scale, Sub.Radius * Scale, Sub.HalfHeight * Scale, Color, 3.0f);
                break;
            }
        }
    }

    CStruct* CComponentVisualizer_Conveyor::GetSupportedComponentType() const
    {
        return SConveyorComponent::StaticStruct();
    }

    void CComponentVisualizer_Conveyor::Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const SConveyorComponent& Conveyor   = Registry.Get<SConveyorComponent>(Entity);
        const STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);

        // SurfaceVelocity is world space; draw it as an arrow whose length scales (clamped) with speed.
        const FVector3 Center = Transform.GetWorldLocationCached();
        const float Speed = Math::Length(Conveyor.SurfaceVelocity);
        if (Speed > 1.0e-3f)
        {
            const FVector3 Dir = Conveyor.SurfaceVelocity / Speed;
            const float Len = Math::Clamp(Speed * 0.25f, 0.5f, 5.0f);
            PDI->DrawArrow(Center, Dir, Len, FVector4(1.0f, 0.6f, 0.0f, 1.0f), 4.0f, true, 0.0f, 0.25f);
        }
    }

    CStruct* CComponentVisualizer_PhysicsConstraint::GetSupportedComponentType() const
    {
        return SPhysicsConstraintComponent::StaticStruct();
    }

    void CComponentVisualizer_PhysicsConstraint::Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const SPhysicsConstraintComponent& Con = Registry.Get<SPhysicsConstraintComponent>(Entity);
        const STransformComponent& Transform   = Registry.Get<STransformComponent>(Entity);

        const FVector3 WorldLoc = Transform.GetWorldLocationCached();
        const FQuat    WorldRot = Transform.GetWorldRotationCached();
        const FVector3 Pivot    = WorldLoc + WorldRot * Con.PivotOffset;

        // Pivot marker.
        PDI->DrawSphere(Pivot, 0.08f, FVector4(1.0f, 0.9f, 0.2f, 1.0f), 10, 3.0f, false, 0.0f);

        // Constraint axis (hinge rotation / slider direction / cone & twist axis), drawn both ways.
        const FVector4 AxisColor(0.2f, 0.8f, 1.0f, 1.0f);
        const FVector3 AxisN = Math::LengthSquared(Con.Axis) > 1.0e-6f
            ? Math::Normalize(WorldRot * Con.Axis) : (WorldRot * FVector3(0.0f, 1.0f, 0.0f));
        PDI->DrawArrow(Pivot,  AxisN, 0.6f, AxisColor, 3.0f, false, 0.0f, 0.2f);
        PDI->DrawArrow(Pivot, -AxisN, 0.6f, AxisColor, 3.0f, false, 0.0f, 0.2f);

        // Shows the allowed swing half-angle for a cone limit.
        if (Con.Type == EPhysicsConstraintType::Cone)
        {
            PDI->DrawCone(Pivot, AxisN, Math::Radians(Con.ConeHalfAngle), 0.6f, AxisColor, 16, 4, 2.0f, false, 0.0f);
        }

        // Line to the connected body (nothing drawn when anchored to the world).
        if (Con.TargetBody != 0xFFFFFFFFu)
        {
            const ECS::FEntity Target = static_cast<ECS::FEntity>(Con.TargetBody);
            const STransformComponent* TargetTransform =
                Registry.IsValid(Target) ? Registry.TryGet<STransformComponent>(Target) : nullptr;
            if (TargetTransform != nullptr)
            {
                const FVector3 TargetLoc = TargetTransform->GetWorldLocationCached();
                PDI->DrawLine(Pivot, TargetLoc, FVector4(1.0f, 0.4f, 0.4f, 1.0f), 2.0f, false, 0.0f);
            }
        }
    }

    CStruct* CComponentVisualizer_AIPerception::GetSupportedComponentType() const
    {
        return SPerceptionComponent::StaticStruct();
    }

    void CComponentVisualizer_AIPerception::Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const SPerceptionComponent& Perception = Registry.Get<SPerceptionComponent>(Entity);
        const STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);

        const FVector3 Location = Transform.GetWorldLocationCached();
        const FVector3 Eye      = Location + Perception.EyeOffset;
        const FVector3 Forward  = Transform.GetWorldRotationCached() * FViewVolume::ForwardAxis;

        if (Perception.bSightEnabled)
        {
            PDI->DrawCone(Eye, Forward, Math::Radians(Perception.SightFOVDegrees * 0.5f), Perception.SightRadius,
                FVector4(0.2f, 0.8f, 1.0f, 1.0f), 16, 4, 2.0f, true, 0.0f);
            PDI->DrawSphere(Eye, Perception.LoseSightRadius, FVector4(0.25f, 0.3f, 0.6f, 1.0f), 16, 1.5f, true, 0.0f);
        }
        if (Perception.bHearingEnabled)
        {
            PDI->DrawSphere(Location, Perception.HearingRadius, FVector4(1.0f, 0.85f, 0.2f, 1.0f), 16, 1.5f, true, 0.0f);
        }
    }

    CStruct* CComponentVisualizer_Spline::GetSupportedComponentType() const
    {
        return SSplineComponent::StaticStruct();
    }

    void CComponentVisualizer_Spline::Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const SSplineComponent&    Spline    = Registry.Get<SSplineComponent>(Entity);
        const STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);

        if (!Spline.bDrawDebug || Spline.Points.empty())
        {
            return;
        }

        const FMatrix4 LocalToWorld = Transform.GetWorldMatrix();
        auto ToWorld = [&](const FVector3& Local) { return FVector3(LocalToWorld * FVector4(Local, 1.0f)); };

        const FVector4 CurveColor(1.0f, 0.55f, 0.15f, 1.0f);
        const FVector4 PointColor(1.0f, 0.85f, 0.30f, 1.0f);
        const FVector4 ArriveColor(0.35f, 0.75f, 1.0f, 1.0f);
        const FVector4 LeaveColor(0.35f, 1.00f, 0.55f, 1.0f);

        // Sized from the curve's own extent so a 1 m spline and a 200 m road both read sensibly.
        float Extent = 0.0f;
        for (const SSplinePoint& Point : Spline.Points)
        {
            Extent = Math::Max(Extent, Math::Distance(Spline.Points[0].Location, Point.Location));
        }
        const float MarkerRadius = Math::Clamp(Extent * 0.02f, 0.05f, 0.5f);

        const int32 NumSegments = Spline.GetNumSegments();

        // Independent of SamplesPerSegment, since that knob sizes the GPU table, not the viewport.
        constexpr int32 kStepsPerSegment = 16;
        if (NumSegments > 0)
        {
            FVector3 Prev = ToWorld(Spline.EvaluatePosition(0.0f));
            const int32 TotalSteps = NumSegments * kStepsPerSegment;
            for (int32 Step = 1; Step <= TotalSteps; ++Step)
            {
                const float Key = (static_cast<float>(Step) / static_cast<float>(kStepsPerSegment));
                const FVector3 Curr = ToWorld(Spline.EvaluatePosition(Key));
                PDI->DrawLine(Prev, Curr, CurveColor, 2.5f, true, 0.0f);
                Prev = Curr;
            }
        }

        for (const SSplinePoint& Point : Spline.Points)
        {
            const FVector3 World = ToWorld(Point.Location);
            PDI->DrawSphere(World, MarkerRadius, PointColor, 10, 2.0f, false, 0.0f);

            // A Hermite tangent is three times the chord it produces, so draw the handle at a third length.
            if (Point.TangentMode == ESplineTangentMode::User)
            {
                const FVector3 Arrive = ToWorld(Point.Location - Point.ArriveTangent / 3.0f);
                const FVector3 Leave  = ToWorld(Point.Location + Point.LeaveTangent / 3.0f);

                PDI->DrawLine(World, Arrive, ArriveColor, 1.5f, false, 0.0f);
                PDI->DrawSphere(Arrive, MarkerRadius * 0.6f, ArriveColor, 8, 1.5f, false, 0.0f);

                PDI->DrawLine(World, Leave, LeaveColor, 1.5f, false, 0.0f);
                PDI->DrawSphere(Leave, MarkerRadius * 0.6f, LeaveColor, 8, 1.5f, false, 0.0f);
            }
        }
    }

    CStruct* CComponentVisualizer_AudioSource::GetSupportedComponentType() const
    {
        return SAudioSourceComponent::StaticStruct();
    }

    void CComponentVisualizer_AudioSource::Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const SAudioSourceComponent& Source = Registry.Get<SAudioSourceComponent>(Entity);
        const STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);

        DrawAudioAttenuation(PDI, Transform.GetWorldLocationCached(), Transform.GetWorldRotationCached(),
            Source.Attenuation.Resolve(), Source.bSpatialized, Source.bPlaying);
    }

    void CComponentVisualizer_AudioSource::DrawVisualization(FComponentVisualizerContext& Context)
    {
        SAudioSourceComponent& Source = Context.Get<SAudioSourceComponent>();
        const STransformComponent& Transform = Context.Get<STransformComponent>();

        const FVector3 Center = Transform.GetWorldLocationCached();

        DrawAudioAttenuationHandles(Context, Source.Attenuation, Center, Transform.GetWorldRotationCached(),
            Source.bSpatialized, "Edit Audio Attenuation");

        const FString SoundName = Source.Sound.IsValid() ? Source.Sound->GetName().ToString() : FString("No Sound");
        LabelAudioEmitter(Context, Center, SoundName.c_str(), Source.Bus, Source.Volume, Source.bSpatialized, Source.bPlaying);
    }

    CStruct* CComponentVisualizer_ProceduralAudio::GetSupportedComponentType() const
    {
        return SProceduralAudioComponent::StaticStruct();
    }

    void CComponentVisualizer_ProceduralAudio::Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const SProceduralAudioComponent& Source = Registry.Get<SProceduralAudioComponent>(Entity);
        const STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);

        DrawAudioAttenuation(PDI, Transform.GetWorldLocationCached(), Transform.GetWorldRotationCached(),
            Source.Attenuation.Resolve(), Source.bSpatialized, Source.bPlaying);
    }

    void CComponentVisualizer_ProceduralAudio::DrawVisualization(FComponentVisualizerContext& Context)
    {
        SProceduralAudioComponent& Source = Context.Get<SProceduralAudioComponent>();
        const STransformComponent& Transform = Context.Get<STransformComponent>();

        const FVector3 Center = Transform.GetWorldLocationCached();

        DrawAudioAttenuationHandles(Context, Source.Attenuation, Center, Transform.GetWorldRotationCached(),
            Source.bSpatialized, "Edit Audio Attenuation");

        const FFixedString Name = FormatAs<FFixedString>("Procedural {} Hz", Source.SampleRate);
        LabelAudioEmitter(Context, Center, Name.c_str(), Source.Bus, Source.Volume, Source.bSpatialized, Source.bPlaying);
    }

    CStruct* CComponentVisualizer_AudioListener::GetSupportedComponentType() const
    {
        return SAudioListenerComponent::StaticStruct();
    }

    void CComponentVisualizer_AudioListener::Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);

        const FVector3 Location = Transform.GetWorldLocationCached();
        const FQuat    Rotation = Transform.GetWorldRotationCached();
        const FVector3 Forward  = Rotation * FViewVolume::ForwardAxis;
        const FVector3 Right    = Rotation * FViewVolume::RightAxis;

        constexpr float EarOffset = 0.32f;

        PDI->DrawSphere(Location, 0.22f, kAudioFlat, 14, 1.5f, true, 0.0f);
        PDI->DrawArrow(Location, Forward, 1.1f, kAudioInner, 3.0f);
        PDI->DrawLine(Location - Right * EarOffset, Location + Right * EarOffset, kAudioOuter, 2.5f, true, 0.0f);
        PDI->DrawSphere(Location + Right * EarOffset, 0.09f, kAudioOuter, 10, 2.0f, true, 0.0f);
        PDI->DrawSphere(Location - Right * EarOffset, 0.09f, kAudioOuter, 10, 2.0f, true, 0.0f);
    }

    void CComponentVisualizer_AudioListener::DrawVisualization(FComponentVisualizerContext& Context)
    {
        const SAudioListenerComponent& Listener = Context.Get<SAudioListenerComponent>();
        const STransformComponent& Transform = Context.Get<STransformComponent>();

        const FVector3 Center = Transform.GetWorldLocationCached();
        const FVector3 Anchor = Center + Context.View.CameraUp * (Context.View.WorldPerPixelAt(Center) * 30.0f);

        // Two components on one slot means the second silently wins, which is worth flagging in place.
        int32 SharingSlot = 0;
        Context.GetRegistry().View<SAudioListenerComponent>().ForEach(
            [&](ECS::FEntity Other, const SAudioListenerComponent& OtherListener)
        {
            if (Other != Context.GetEntity() && OtherListener.ListenerIndex == Listener.ListenerIndex)
            {
                ++SharingSlot;
            }
        });

        if (SharingSlot > 0)
        {
            Context.Label(Anchor, FVector4(1.0f, 0.35f, 0.35f, 1.0f), "Listener %d   shared with %d other", Listener.ListenerIndex, SharingSlot);
            return;
        }

        Context.Label(Anchor, kAudioInner, "Listener %d%s", Listener.ListenerIndex, Listener.bApplyDoppler ? "   doppler" : "");
    }
}
