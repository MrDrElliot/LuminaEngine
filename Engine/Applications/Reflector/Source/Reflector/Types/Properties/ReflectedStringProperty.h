#pragma once

#include "ReflectedProperty.h"

namespace Lumina
{
    class FReflectedStringProperty : public FReflectedProperty
    {
    public:

        const char* GetTypeName() const override { return "FString"; }
    };

    class FReflectedNameProperty : public FReflectedProperty
    {
    public:

        const char* GetTypeName() const override { return "FName"; }
    };
}
