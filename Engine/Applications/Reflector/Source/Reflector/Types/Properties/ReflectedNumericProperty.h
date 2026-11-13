#pragma once

#include "ReflectedProperty.h"

namespace Lumina
{
#define DEFINE_REFLECTED_NUMERIC_PROPERTY(ClassName, TypeNameStr)     class ClassName final : public FReflectedProperty     {     public:         const char* GetTypeName() const override { return TypeNameStr; }     };

    DEFINE_REFLECTED_NUMERIC_PROPERTY(FReflectedUInt8Property,  "UInt8")
    DEFINE_REFLECTED_NUMERIC_PROPERTY(FReflectedUInt16Property, "UInt16")
    DEFINE_REFLECTED_NUMERIC_PROPERTY(FReflectedUInt32Property, "UInt32")
    DEFINE_REFLECTED_NUMERIC_PROPERTY(FReflectedUInt64Property, "UInt64")
    DEFINE_REFLECTED_NUMERIC_PROPERTY(FReflectedInt8Property,   "Int8")
    DEFINE_REFLECTED_NUMERIC_PROPERTY(FReflectedInt16Property,  "Int16")
    DEFINE_REFLECTED_NUMERIC_PROPERTY(FReflectedInt32Property,  "Int32")
    DEFINE_REFLECTED_NUMERIC_PROPERTY(FReflectedInt64Property,  "Int64")
    DEFINE_REFLECTED_NUMERIC_PROPERTY(FReflectedEntityProperty, "Entity")
    DEFINE_REFLECTED_NUMERIC_PROPERTY(FReflectedFloatProperty,  "Float")
    DEFINE_REFLECTED_NUMERIC_PROPERTY(FReflectedDoubleProperty, "Double")
    DEFINE_REFLECTED_NUMERIC_PROPERTY(FReflectedBoolProperty,   "Bool")

#undef DEFINE_REFLECTED_NUMERIC_PROPERTY
}
