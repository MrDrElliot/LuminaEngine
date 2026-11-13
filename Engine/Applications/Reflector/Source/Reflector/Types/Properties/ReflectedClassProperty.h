#pragma once

#include "ReflectedProperty.h"

namespace Lumina
{
    // A TSubclassOf<T> field, whose TypeName is the base class T.
    class FReflectedClassProperty : public FReflectedProperty
    {
    public:

        const char* GetTypeName() const override { return "Class"; }
        EReflectedLink GetLink() const override { return EReflectedLink::Class; }
    };
}
