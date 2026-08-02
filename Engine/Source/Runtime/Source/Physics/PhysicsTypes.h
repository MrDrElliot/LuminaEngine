#pragma once

#include "Core/Object/ObjectMacros.h"
#include "PhysicsTypes.generated.h"

namespace Lumina
{
    REFLECT(BitMask)
    enum class RUNTIME_API ECollisionProfiles : uint16
    {
        Static      = BIT(0),
        Dynamic     = BIT(1),
        
        Channel0    = BIT(2),
        Channel1    = BIT(3),
        Channel2    = BIT(4),
        Channel3    = BIT(5),
        Channel4    = BIT(6),
        Channel5    = BIT(7),
        Channel6    = BIT(8),
        Channel7    = BIT(9),
        Channel8    = BIT(10),
        Channel9    = BIT(11),
        Channel10   = BIT(12),
        Channel11   = BIT(13),
        Channel12   = BIT(14),
        Channel13   = BIT(15),
    };
    
    ENUM_CLASS_FLAGS(ECollisionProfiles);
    
    REFLECT()
    struct FCollisionProfile
    {
        GENERATED_BODY()
        
        /** Collision layer this body belongs to, determines which layers can see it. */
        PROPERTY(Script, Editable)
        ECollisionProfiles Layer    = ECollisionProfiles::Dynamic;

        /** Bitmask of layers this body collides against. */
        PROPERTY(Script, Editable)
        ECollisionProfiles Mask     = ECollisionProfiles::Static | ECollisionProfiles::Dynamic;
        
        NODISCARD FORCEINLINE bool ShouldCollide(const FCollisionProfile& Other) const
        {
            return (Mask & Other.Layer) != (ECollisionProfiles)0 || (Other.Mask & Layer) != (ECollisionProfiles)0;
        }
    };
    
    REFLECT()
    enum class RUNTIME_API EMoveMode : uint8
    {
        Teleport,           // Hard set position (loses velocity)
        MoveKinematic,      // Move with velocity calculation (preserves physics)
        ActivateOnly        // Just wake up, don't move
    };
    
    REFLECT()
    enum class RUNTIME_API EBodyType : uint8
    {
        Static,
        Kinematic,
        Dynamic,
    };

    /** Joint type for SPhysicsConstraintComponent / World.Physics constraints. Order is the wire contract
        shared with the C# Physics facade and the native FConstraintDesc switch -- append only. */
    REFLECT()
    enum class RUNTIME_API EPhysicsConstraintType : uint8
    {
        Fixed,      // Weld: removes all 6 DOF (compound props, breakable welds).
        Point,      // Ball-and-socket: removes 3 translation DOF (ropes, chains).
        Distance,   // Keeps two points a fixed/ranged distance apart (rope length cap, stiff spring).
        Hinge,      // Single rotation axis with optional limits + motor (doors, wheels, levers).
        Slider,     // Single translation axis with optional limits + motor (pistons, drawers, lifts).
        Cone,       // Swing-limited ball-socket (twist axis stays within a cone).
    };
}
