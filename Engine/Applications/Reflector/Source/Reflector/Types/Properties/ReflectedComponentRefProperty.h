#pragma once

#include "ReflectedProperty.h"

namespace Lumina
{
    // A TComponentRef<T> field, whose TypeName is the component T.
    class FReflectedComponentRefProperty : public FReflectedProperty
    {
    public:

        const char* GetTypeName() const override { return "ComponentRef"; }
        EReflectedLink GetLink() const override { return EReflectedLink::Struct; }
    };
}
