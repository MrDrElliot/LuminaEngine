#pragma once

#include "ReflectedProperty.h"

namespace Lumina
{
    class FReflectedEnumProperty : public FReflectedProperty
    {
    public:

        const char* GetTypeName() const override { return "Enum"; }
        EReflectedLink GetLink() const override { return EReflectedLink::Enum; }
    };
}
