#pragma once

#include "ReflectedProperty.h"

namespace Lumina
{
    class FReflectedStructProperty : public FReflectedProperty
    {
    public:

        const char* GetTypeName() const override { return "Struct"; }
        EReflectedLink GetLink() const override { return EReflectedLink::Struct; }
    };
}
