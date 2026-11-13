#pragma once

#include "ReflectedProperty.h"

namespace Lumina
{
    // A TInstancedStruct<T> field, whose TypeName is the base struct T, or empty for a bare FInstancedStruct.
    class FReflectedInstancedStructProperty : public FReflectedProperty
    {
    public:

        const char* GetTypeName() const override { return "InstancedStruct"; }
        EReflectedLink GetLink() const override { return TypeName.empty() ? EReflectedLink::None : EReflectedLink::Struct; }
    };
}
