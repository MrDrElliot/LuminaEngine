#include "EditorPCH.h"
#include "ComponentVisualizer.h"
#include "Core/Math/Color.h"
#include "Renderer/PrimitiveDrawInterface.h"
#include "Tools/Import/ImportHelpers.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/DecalComponent.h"
#include "world/entity/components/lightcomponent.h"
#include "world/entity/components/physicscomponent.h"
#include "World/Entity/Components/PerceptionComponent.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina
{
    static CComponentVisualizerRegistry* Singleton = nullptr;
    
    
    CComponentVisualizerRegistry& CComponentVisualizerRegistry::Get()
    {
        static std::once_flag Flag;
        std::call_once(Flag, []()
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

        // Wireframe cylinder, optionally tapered (different top vs bottom radius): two rings joined by a few
        // vertical spokes. Reused by the cylinder / tapered-cylinder / compound visualizers.
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

    void CComponentVisualizer_PointLight::Draw(IPrimitiveDrawInterface* PDI, entt::registry& Registry, entt::entity Entity)
    {
        const SPointLightComponent& PointLight = Registry.get<SPointLightComponent>(Entity);
        const STransformComponent& Transform = Registry.get<STransformComponent>(Entity);
        
        PDI->DrawSphere(Transform.GetWorldLocationCached(), PointLight.Attenuation, 
            FVector4(PointLight.LightColor, 1.0f), 32, 1.0f, true, 0.0f);
    }

    CStruct* CComponentVisualizer_SpotLight::GetSupportedComponentType() const
    {
        return SSpotLightComponent::StaticStruct();
    }

    void CComponentVisualizer_SpotLight::Draw(IPrimitiveDrawInterface* PDI, entt::registry& Registry, entt::entity Entity)
    {
        const SSpotLightComponent& SpotLight    = Registry.get<SSpotLightComponent>(Entity);
        const STransformComponent& Transform    = Registry.get<STransformComponent>(Entity);
        FVector3 Forward                       = Transform.GetWorldRotationCached() * FViewVolume::ForwardAxis;

        // Cone opens along the spot's aim (transform forward) -- the direction it lights, matching FLight::Direction = -aim (to-light).
        PDI->DrawCone(Transform.GetWorldLocationCached(), Forward, Math::Radians(SpotLight.OuterConeAngle), SpotLight.Attenuation, FVector4(SpotLight.LightColor, 1.0f));
        PDI->DrawCone(Transform.GetWorldLocationCached(), Forward, Math::Radians(SpotLight.InnerConeAngle), SpotLight.Attenuation, FVector4(SpotLight.LightColor, 1.0f));
    }

    CStruct* CComponentVisualizer_DirectionalLight::GetSupportedComponentType() const
    {
        return SDirectionalLightComponent::StaticStruct();
    }

    void CComponentVisualizer_DirectionalLight::Draw(IPrimitiveDrawInterface* PDI, entt::registry& Registry, entt::entity Entity)
    {
        const auto& Light       = Registry.get<SDirectionalLightComponent>(Entity);
        const auto& Transform   = Registry.get<STransformComponent>(Entity);
        
        PDI->DrawArrow(Transform.GetWorldLocationCached(), -Light.Direction, 1.5f, FColor::Yellow, 4.0f);
    }

    CStruct* CComponentVisualizer_SphereCollider::GetSupportedComponentType() const
    {
        return SSphereColliderComponent::StaticStruct();
    }

    void CComponentVisualizer_SphereCollider::Draw(IPrimitiveDrawInterface* PDI, entt::registry& Registry, entt::entity Entity)
    {
        const SSphereColliderComponent& Sphere = Registry.get<SSphereColliderComponent>(Entity);
        const STransformComponent& Transform = Registry.get<STransformComponent>(Entity);

        const FVector3 Center = Transform.GetWorldLocationCached() + Transform.GetWorldRotationCached() * Sphere.TranslationOffset;
        PDI->DrawSphere(Center, Sphere.Radius * Transform.MaxScale(), ColliderColor(Sphere.bIsTrigger), 8, 3.5f, true, 0.0f);
    }

    CStruct* CComponentVisualizer_BoxCollider::GetSupportedComponentType() const
    {
        return SBoxColliderComponent::StaticStruct();
    }

    void CComponentVisualizer_BoxCollider::Draw(IPrimitiveDrawInterface* PDI, entt::registry& Registry, entt::entity Entity)
    {
        const SBoxColliderComponent& Box = Registry.get<SBoxColliderComponent>(Entity);
        const STransformComponent& Transform = Registry.get<STransformComponent>(Entity);

        const FVector3 Center  = Transform.GetWorldLocationCached() + Transform.GetWorldRotationCached() * Box.TranslationOffset;
        const FQuat    WorldRot = Transform.GetWorldRotationCached() * FQuat(Box.RotationOffset);
        PDI->DrawBox(Center, Box.HalfExtent * Transform.GetWorldScaleCached(), WorldRot, ColliderColor(Box.bIsTrigger), 3.5f, true, 0.0f);
    }

    CStruct* CComponentVisualizer_CapsuleCollider::GetSupportedComponentType() const
    {
        return SCapsuleColliderComponent::StaticStruct();
    }

    void CComponentVisualizer_CapsuleCollider::Draw(IPrimitiveDrawInterface* PDI, entt::registry& Registry, entt::entity Entity)
    {
        const SCapsuleColliderComponent& Capsule = Registry.get<SCapsuleColliderComponent>(Entity);
        const STransformComponent& Transform     = Registry.get<STransformComponent>(Entity);

        // DrawCapsule wants the two cylinder-axis endpoints (caps tangent there), Y-aligned in local space.
        const float Scale     = Transform.MaxScale();
        const FQuat WorldRot  = Transform.GetWorldRotationCached() * FQuat(Capsule.RotationOffset);
        const FVector3 Center = Transform.GetWorldLocationCached() + Transform.GetWorldRotationCached() * Capsule.TranslationOffset;
        const FVector3 Axis   = WorldRot * FVector3(0.0f, Capsule.HalfHeight * Scale, 0.0f);

        PDI->DrawCapsule(Center - Axis, Center + Axis, Capsule.Radius * Scale, ColliderColor(Capsule.bIsTrigger), 12, 3.5f, true, 0.0f);
    }

    CStruct* CComponentVisualizer_CylinderCollider::GetSupportedComponentType() const
    {
        return SCylinderColliderComponent::StaticStruct();
    }

    void CComponentVisualizer_CylinderCollider::Draw(IPrimitiveDrawInterface* PDI, entt::registry& Registry, entt::entity Entity)
    {
        const SCylinderColliderComponent& Cyl = Registry.get<SCylinderColliderComponent>(Entity);
        const STransformComponent& Transform  = Registry.get<STransformComponent>(Entity);

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

    void CComponentVisualizer_CharacterPhysics::Draw(IPrimitiveDrawInterface* PDI, entt::registry& Registry, entt::entity Entity)
    {
        const SCharacterPhysicsComponent& Character = Registry.get<SCharacterPhysicsComponent>(Entity);
        const STransformComponent& Transform = Registry.get<STransformComponent>(Entity);

        // Match Jolt: Start/End are cylinder-axis endpoints; Radius scales by MaxScale.
        const FVector3 Location = Transform.GetWorldLocationCached();
        const FVector3 Axis = Transform.GetWorldRotationCached() * FVector3(0.0f, Character.HalfHeight, 0.0f);
        const FVector3 Start = Location - Axis;
        const FVector3 End   = Location + Axis;

        PDI->DrawCapsule(Start, End, Character.Radius * Transform.MaxScale(), FColor::Blue, 12, 2.0f, true, 0.0f);
    }

    CStruct* CComponentVisualizer_RigidBody::GetSupportedComponentType() const
    {
        return SRigidBodyComponent::StaticStruct();
    }

    void CComponentVisualizer_RigidBody::Draw(IPrimitiveDrawInterface* PDI, entt::registry& Registry, entt::entity Entity)
    {
        const SRigidBodyComponent& Body      = Registry.get<SRigidBodyComponent>(Entity);
        const STransformComponent& Transform = Registry.get<STransformComponent>(Entity);

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

    void CComponentVisualizer_Camera::Draw(IPrimitiveDrawInterface* PDI, entt::registry& Registry, entt::entity Entity)
    {
        const auto& Transform   = Registry.get<STransformComponent>(Entity);
        const auto& Camera      = Registry.get<SCameraComponent>(Entity);

        // Cached ViewVolume only refreshes at runtime via SCameraSystem, so rebuild the view-projection from the live transform.
        // The real far plane is effectively infinite; clamp only the gizmo's far for display (never affects the camera).
        constexpr float GizmoFar = 25.0f;
        const FVector3 Location = Transform.GetWorldLocationCached();
        const FQuat    Rotation = Transform.GetWorldRotationCached();
        const FVector3 Forward  = Rotation * FViewVolume::ForwardAxis;
        const FVector3 Up       = Rotation * FViewVolume::UpAxis;

        FViewVolume Volume(Camera.GetFOV(), Camera.GetAspectRatio(), Camera.GetViewVolume().GetNear(), GizmoFar);
        Volume.SetView(Location, Forward, Up);

        // Reverse-Z Vulkan NDC: near plane is z=1, far plane is z=0.
        PDI->DrawFrustum(Volume.GetViewProjectionMatrix(), 1.0f, 0.0f, FColor::White, 4.0f);
        PDI->DrawArrow(Location, Forward, 3.5f, FColor::Green, 4.0f);
    }

    CStruct* CComponentVisualizer_Decal::GetSupportedComponentType() const
    {
        return SDecalComponent::StaticStruct();
    }

    void CComponentVisualizer_Decal::Draw(IPrimitiveDrawInterface* PDI, entt::registry& Registry, entt::entity Entity)
    {
        const SDecalComponent& Decal         = Registry.get<SDecalComponent>(Entity);
        const STransformComponent& Transform = Registry.get<STransformComponent>(Entity);

        const FVector3 Location  = Transform.GetWorldLocationCached();
        const FQuat    Rotation  = Transform.GetWorldRotationCached();
        const FVector3 HalfExtent = Decal.Size * 0.5f * Transform.GetWorldScaleCached();

        // Projection volume + the -Z axis the material projects along.
        const FVector4 BoxColor(1.0f, 0.2f, 0.8f, 1.0f);
        PDI->DrawBox(Location, HalfExtent, Rotation, BoxColor, 2.5f, true, 0.0f);

        const FVector3 ProjectDir = Rotation * FVector3(0.0f, 0.0f, -1.0f);
        PDI->DrawArrow(Location, ProjectDir, HalfExtent.z, FVector4(1.0f, 0.85f, 0.2f, 1.0f), 3.0f);
    }

    CStruct* CComponentVisualizer_TaperedCapsuleCollider::GetSupportedComponentType() const
    {
        return STaperedCapsuleColliderComponent::StaticStruct();
    }

    void CComponentVisualizer_TaperedCapsuleCollider::Draw(IPrimitiveDrawInterface* PDI, entt::registry& Registry, entt::entity Entity)
    {
        const STaperedCapsuleColliderComponent& TC = Registry.get<STaperedCapsuleColliderComponent>(Entity);
        const STransformComponent& Transform = Registry.get<STransformComponent>(Entity);

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

    void CComponentVisualizer_TaperedCylinderCollider::Draw(IPrimitiveDrawInterface* PDI, entt::registry& Registry, entt::entity Entity)
    {
        const STaperedCylinderColliderComponent& TC = Registry.get<STaperedCylinderColliderComponent>(Entity);
        const STransformComponent& Transform = Registry.get<STransformComponent>(Entity);

        const float Scale     = Transform.MaxScale();
        const FQuat WorldRot  = Transform.GetWorldRotationCached() * FQuat(TC.RotationOffset);
        const FVector3 Center = Transform.GetWorldLocationCached() + Transform.GetWorldRotationCached() * TC.TranslationOffset;

        DrawWireCylinder(PDI, Center, WorldRot, TC.TopRadius * Scale, TC.BottomRadius * Scale, TC.HalfHeight * Scale, ColliderColor(TC.bIsTrigger), 3.5f);
    }

    CStruct* CComponentVisualizer_PlaneCollider::GetSupportedComponentType() const
    {
        return SPlaneColliderComponent::StaticStruct();
    }

    void CComponentVisualizer_PlaneCollider::Draw(IPrimitiveDrawInterface* PDI, entt::registry& Registry, entt::entity Entity)
    {
        const SPlaneColliderComponent& Plane = Registry.get<SPlaneColliderComponent>(Entity);
        const STransformComponent& Transform = Registry.get<STransformComponent>(Entity);

        const FVector3 Center = Transform.GetWorldLocationCached();
        const FQuat    Rot    = Transform.GetWorldRotationCached();
        const FVector3 Right  = Rot * FVector3(1.0f, 0.0f, 0.0f);
        const FVector3 Fwd    = Rot * FVector3(0.0f, 0.0f, 1.0f);
        const FVector3 Up     = Rot * FVector3(0.0f, 1.0f, 0.0f);
        const FVector4 Color  = ColliderColor(Plane.bIsTrigger);

        // Collision is effectively infinite; draw a fixed patch + cross so it's readable, plus the +Y surface
        // normal (the solid half-space is below the plane).
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

    void CComponentVisualizer_CompoundCollider::Draw(IPrimitiveDrawInterface* PDI, entt::registry& Registry, entt::entity Entity)
    {
        const SCompoundColliderComponent& Comp = Registry.get<SCompoundColliderComponent>(Entity);
        const STransformComponent& Transform   = Registry.get<STransformComponent>(Entity);

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

    void CComponentVisualizer_Conveyor::Draw(IPrimitiveDrawInterface* PDI, entt::registry& Registry, entt::entity Entity)
    {
        const SConveyorComponent& Conveyor   = Registry.get<SConveyorComponent>(Entity);
        const STransformComponent& Transform = Registry.get<STransformComponent>(Entity);

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

    void CComponentVisualizer_PhysicsConstraint::Draw(IPrimitiveDrawInterface* PDI, entt::registry& Registry, entt::entity Entity)
    {
        const SPhysicsConstraintComponent& Con = Registry.get<SPhysicsConstraintComponent>(Entity);
        const STransformComponent& Transform   = Registry.get<STransformComponent>(Entity);

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

        // Cone limit: show the allowed swing half-angle.
        if (Con.Type == EPhysicsConstraintType::Cone)
        {
            PDI->DrawCone(Pivot, AxisN, Math::Radians(Con.ConeHalfAngle), 0.6f, AxisColor, 16, 4, 2.0f, false, 0.0f);
        }

        // Line to the connected body (nothing drawn when anchored to the world).
        if (Con.TargetBody != 0xFFFFFFFFu)
        {
            const entt::entity Target = static_cast<entt::entity>(Con.TargetBody);
            if (Registry.valid(Target) && Registry.any_of<STransformComponent>(Target))
            {
                const FVector3 TargetLoc = Registry.get<STransformComponent>(Target).GetWorldLocationCached();
                PDI->DrawLine(Pivot, TargetLoc, FVector4(1.0f, 0.4f, 0.4f, 1.0f), 2.0f, false, 0.0f);
            }
        }
    }

    CStruct* CComponentVisualizer_AIPerception::GetSupportedComponentType() const
    {
        return SPerceptionComponent::StaticStruct();
    }

    void CComponentVisualizer_AIPerception::Draw(IPrimitiveDrawInterface* PDI, entt::registry& Registry, entt::entity Entity)
    {
        const SPerceptionComponent& Perception = Registry.get<SPerceptionComponent>(Entity);
        const STransformComponent& Transform = Registry.get<STransformComponent>(Entity);

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
}
