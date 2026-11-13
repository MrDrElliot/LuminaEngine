#pragma once

#include "ReflectedProperty.h"

namespace Lumina
{
    // A TSoftObjectPtr<T> or FSoftObjectPath field, saved as a path and GUID rather than a strong reference.
    class FReflectedSoftObjectProperty : public FReflectedProperty
    {
    public:

        const char* GetTypeName() const override { return "SoftObject"; }
        EReflectedLink GetLink() const override { return EReflectedLink::Class; }

        // A bare FSoftObjectPath names CObject, an intrinsic class the database does not hold.
        bool RequiresReflectedType() const override { return false; }
    };
}
