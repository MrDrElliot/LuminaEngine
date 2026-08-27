#pragma once

#include "World/ECS/Registry.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectMacros.h"
#include "ComponentVisualizer.generated.h"

namespace Lumina
{
    class IPrimitiveDrawInterface;
    class CComponentVisualizer;

    REFLECT()
    class EDITOR_API CComponentVisualizerRegistry : public CObject
    {
        GENERATED_BODY()
    public:
        
        static CComponentVisualizerRegistry& Get();
        
        void RegisterComponentVisualizer(CComponentVisualizer* Visualizer);
        
        CComponentVisualizer* GetComponentVisualizer(CStruct* Component);
        
        const THashMap<CStruct*, CComponentVisualizer*>& GetVisualizers() const { return Visualizers; }
        
    private:
        
        THashMap<CStruct*, CComponentVisualizer*> Visualizers;
    };
    
    REFLECT()
    class EDITOR_API CComponentVisualizer : public CObject
    {
        GENERATED_BODY()
    public:
        
        void PostCreateCDO() override;
        
        virtual CStruct* GetSupportedComponentType() const { return nullptr; }
        
        virtual void Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity) { }
    };
    
    REFLECT()
    class EDITOR_API CComponentVisualizer_PointLight : public CComponentVisualizer
    {
        GENERATED_BODY()
    public:
        
        CStruct* GetSupportedComponentType() const override;
        
        void Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity) override;
        
    };
    
    REFLECT()
    class EDITOR_API CComponentVisualizer_SpotLight : public CComponentVisualizer
    {
        GENERATED_BODY()
    public:
        
        CStruct* GetSupportedComponentType() const override;
        
        void Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity) override;
    };
    
    REFLECT()
    class EDITOR_API CComponentVisualizer_DirectionalLight : public CComponentVisualizer
    {
        GENERATED_BODY()
    public:
        
        CStruct* GetSupportedComponentType() const override;
        
        void Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity) override;
    };
    
    REFLECT()
    class EDITOR_API CComponentVisualizer_SphereCollider : public CComponentVisualizer
    {
        GENERATED_BODY()
    public:
        
        CStruct* GetSupportedComponentType() const override;
        
        void Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity) override;
        
    };
    
    REFLECT()
    class EDITOR_API CComponentVisualizer_BoxCollider : public CComponentVisualizer
    {
        GENERATED_BODY()
    public:
        
        CStruct* GetSupportedComponentType() const override;
        
        void Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity) override;
        
    };
    
    REFLECT()
    class EDITOR_API CComponentVisualizer_CapsuleCollider : public CComponentVisualizer
    {
        GENERATED_BODY()
    public:

        CStruct* GetSupportedComponentType() const override;

        void Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity) override;
    };

    REFLECT()
    class EDITOR_API CComponentVisualizer_CylinderCollider : public CComponentVisualizer
    {
        GENERATED_BODY()
    public:

        CStruct* GetSupportedComponentType() const override;

        void Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity) override;
    };

    REFLECT()
    class EDITOR_API CComponentVisualizer_CharacterPhysics : public CComponentVisualizer
    {
        GENERATED_BODY()
    public:
        
        CStruct* GetSupportedComponentType() const override;
        
        void Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity) override;
        
    };
    
    REFLECT()
    class EDITOR_API CComponentVisualizer_RigidBody : public CComponentVisualizer
    {
        GENERATED_BODY()
    public:

        CStruct* GetSupportedComponentType() const override;

        void Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity) override;

    };

    REFLECT()
    class EDITOR_API CComponentVisualizer_Camera : public CComponentVisualizer
    {
        GENERATED_BODY()
    public:

        CStruct* GetSupportedComponentType() const override;

        void Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity) override;

    };

    REFLECT()
    class EDITOR_API CComponentVisualizer_Decal : public CComponentVisualizer
    {
        GENERATED_BODY()
    public:

        CStruct* GetSupportedComponentType() const override;

        void Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity) override;
    };

    REFLECT()
    class EDITOR_API CComponentVisualizer_ReflectionProbe : public CComponentVisualizer
    {
        GENERATED_BODY()
    public:

        CStruct* GetSupportedComponentType() const override;

        void Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity) override;
    };

    REFLECT()
    class EDITOR_API CComponentVisualizer_TaperedCapsuleCollider : public CComponentVisualizer
    {
        GENERATED_BODY()
    public:
        CStruct* GetSupportedComponentType() const override;
        void Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity) override;
    };

    REFLECT()
    class EDITOR_API CComponentVisualizer_TaperedCylinderCollider : public CComponentVisualizer
    {
        GENERATED_BODY()
    public:
        CStruct* GetSupportedComponentType() const override;
        void Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity) override;
    };

    REFLECT()
    class EDITOR_API CComponentVisualizer_PlaneCollider : public CComponentVisualizer
    {
        GENERATED_BODY()
    public:
        CStruct* GetSupportedComponentType() const override;
        void Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity) override;
    };

    REFLECT()
    class EDITOR_API CComponentVisualizer_CompoundCollider : public CComponentVisualizer
    {
        GENERATED_BODY()
    public:
        CStruct* GetSupportedComponentType() const override;
        void Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity) override;
    };

    REFLECT()
    class EDITOR_API CComponentVisualizer_Conveyor : public CComponentVisualizer
    {
        GENERATED_BODY()
    public:
        CStruct* GetSupportedComponentType() const override;
        void Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity) override;
    };

    REFLECT()
    class EDITOR_API CComponentVisualizer_PhysicsConstraint : public CComponentVisualizer
    {
        GENERATED_BODY()
    public:
        CStruct* GetSupportedComponentType() const override;
        void Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity) override;
    };

    REFLECT()
    class EDITOR_API CComponentVisualizer_AIPerception : public CComponentVisualizer
    {
        GENERATED_BODY()
    public:
        CStruct* GetSupportedComponentType() const override;
        void Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity) override;
    };

    REFLECT()
    class EDITOR_API CComponentVisualizer_Spline : public CComponentVisualizer
    {
        GENERATED_BODY()
    public:
        CStruct* GetSupportedComponentType() const override;
        void Draw(IPrimitiveDrawInterface* PDI, ECS::FRegistry& Registry, ECS::FEntity Entity) override;
    };
}
