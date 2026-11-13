#pragma once

#include "ReflectedProperty.h"

namespace Lumina
{
    // A TSubStructOf<T> field, whose TypeName is the base struct T.
    class FReflectedSubStructProperty : public FReflectedProperty
    {
    public:

        const char* GetTypeName() const override { return "SubStruct"; }
        EReflectedLink GetLink() const override { return EReflectedLink::Struct; }
    };
}
