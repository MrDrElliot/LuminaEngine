#pragma once

#include "Core/Object/ObjectMacros.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "DestructibleComponent.generated.h"

namespace Lumina
{
    class CStaticMesh;
    class CGeometryCollection;

    // Marks an entity breakable (alongside a StaticMeshComponent); World.Fracture/FractureAt shatters it
    // at runtime into physics-driven chunks that inherit its motion plus an outward launch impulse.
    REFLECT(Component, Category = "Destruction")
    struct RUNTIME_API SDestructibleComponent
    {
        GENERATED_BODY()

        // Pre-baked fracture pieces; the entity shatters into these convex chunks. Null = on-the-fly
        // convex Voronoi fracture from the source mesh bounds.
        PROPERTY(Editable, Category = "Destruction")
        TObjectPtr<CGeometryCollection> Collection;

        /** Fallback only (no Collection, no Voronoi): mesh used for each grid chunk. Null = reuse the source mesh. */
        PROPERTY(Editable, Category = "Destruction")
        TObjectPtr<CStaticMesh> FragmentMesh;

        /** Approximate number of fragments to break into. Rounded up to the nearest cubic grid. */
        PROPERTY(Script, Editable, ClampMin = 2, ClampMax = 512, Category = "Destruction")
        int32 FragmentCount = 8;

        /** Default outward launch speed (m/s) for fragments when no explicit strength is passed to Fracture. */
        PROPERTY(Script, Editable, ClampMin = 0.0f, Category = "Destruction")
        float ExplosionStrength = 6.0f;

        /** Seconds each fragment survives before it is automatically cleaned up (0 = never). */
        PROPERTY(Script, Editable, ClampMin = 0.0f, Category = "Destruction")
        float FragmentLifetime = 8.0f;

        /** Maximum random spin (rad/s) imparted to fragments for a more chaotic break. */
        PROPERTY(Script, Editable, ClampMin = 0.0f, Category = "Destruction")
        float SpinStrength = 6.0f;

        /** When true, the original entity is removed once it fractures. */
        PROPERTY(Script, Editable, Category = "Destruction")
        bool bDestroyOriginal = true;

        /** Set once the entity has fractured so repeated Fracture calls are ignored. Not serialized. */
        bool bFractured = false;
    };

    /** Lightweight tag placed on spawned fragments, pointing back at the destructible they broke from. */
    REFLECT(Component, HideInComponentList, Category = "Destruction")
    struct RUNTIME_API SFragmentComponent
    {
        GENERATED_BODY()

        /** Entity id of the destructible this fragment originated from. */
        uint32 Source = entt::null;
    };
}
