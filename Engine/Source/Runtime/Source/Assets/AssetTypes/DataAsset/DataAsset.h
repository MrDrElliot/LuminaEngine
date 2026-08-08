#pragma once

#include "Core/Object/Object.h"
#include "Core/Object/ObjectMacros.h"
#include "DataAsset.generated.h"

namespace Lumina
{
    /**
     * Base class for designer-authored data assets. Subclass it in C++, declare PROPERTY() fields, and
     * that is the whole type:
     *
     *     REFLECT()
     *     class CWeaponData : public CDataAsset
     *     {
     *         GENERATED_BODY()
     *     public:
     *         PROPERTY(Editable, Category = "Damage") float BaseDamage = 10.0f;
     *         PROPERTY(Editable) TObjectPtr<CStaticMesh> Mesh;
     *     };
     *
     * The content browser's Data Asset entry lists every class deriving from this one and mints the
     * one you pick, so a new type needs no factory, no editor tool and no registration. The property
     * grid and serialization both come from reflection, exactly as they do for any other asset, which
     * is why a subclass gets a working editor for free.
     */
    REFLECT()
    class RUNTIME_API CDataAsset : public CObject
    {
        GENERATED_BODY()

    public:

        bool IsAsset() const override { return true; }
    };
}
