#pragma once

#include "ReflectedProperty.h"

namespace Lumina
{
    class FReflectedObjectProperty : public FReflectedProperty
    {
    public:

        const char* GetTypeName() const override { return "Object"; }
        EReflectedLink GetLink() const override { return EReflectedLink::Class; }

        // The pointee may be an intrinsic class such as CObject, which the database does not hold.
        bool RequiresReflectedType() const override { return false; }
    };
}
