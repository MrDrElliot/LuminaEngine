#pragma once
#include "ReflectedProperty.h"
#include "Reflector/CodeGeneration/CodeWriter.h"


namespace Lumina
{
#define DEFINE_REFLECTED_NUMERIC_PROPERTY(ClassName, TypeFlag, TypeNameStr, LuaType) \
    class ClassName final : public FReflectedProperty \
    { \
    public: \
        void AppendDefinition(Reflection::FCodeWriter& Writer) const override \
        { \
            const std::string PropertyFlagStr = PropertyFlagsToString(PropertyFlags); \
            AppendPropertyDef(Writer, PropertyFlagStr.c_str(), #TypeFlag); \
        } \
        std::string_view GetLuaType() override { return LuaType; } \
        const char* GetTypeName() override { return TypeNameStr; } \
        const char* GetPropertyParamType() const override { return "FNumericPropertyParams"; } \
    };

    // Unsigned integer properties
    DEFINE_REFLECTED_NUMERIC_PROPERTY(FReflectedUInt8Property,  Lumina::EPropertyTypeFlags::UInt8,  "UInt8",  "number")
    DEFINE_REFLECTED_NUMERIC_PROPERTY(FReflectedUInt16Property, Lumina::EPropertyTypeFlags::UInt16, "UInt16", "number")
    DEFINE_REFLECTED_NUMERIC_PROPERTY(FReflectedUInt32Property, Lumina::EPropertyTypeFlags::UInt32, "UInt32", "number")
    DEFINE_REFLECTED_NUMERIC_PROPERTY(FReflectedUInt64Property, Lumina::EPropertyTypeFlags::UInt64, "UInt64", "number")

    // Signed integer properties
    DEFINE_REFLECTED_NUMERIC_PROPERTY(FReflectedInt8Property,   Lumina::EPropertyTypeFlags::Int8,   "Int8",   "number")
    DEFINE_REFLECTED_NUMERIC_PROPERTY(FReflectedInt16Property,  Lumina::EPropertyTypeFlags::Int16,  "Int16",  "number")
    DEFINE_REFLECTED_NUMERIC_PROPERTY(FReflectedInt32Property,  Lumina::EPropertyTypeFlags::Int32,  "Int32",  "number")
    DEFINE_REFLECTED_NUMERIC_PROPERTY(FReflectedInt64Property,  Lumina::EPropertyTypeFlags::Int64,  "Int64",  "number")

    // Floating point + bool properties
    DEFINE_REFLECTED_NUMERIC_PROPERTY(FReflectedFloatProperty,  Lumina::EPropertyTypeFlags::Float,  "Float",  "number")
    DEFINE_REFLECTED_NUMERIC_PROPERTY(FReflectedDoubleProperty, Lumina::EPropertyTypeFlags::Double, "Double", "number")
    DEFINE_REFLECTED_NUMERIC_PROPERTY(FReflectedBoolProperty,   Lumina::EPropertyTypeFlags::Bool,   "Bool",   "boolean")

    #undef DEFINE_REFLECTED_NUMERIC_PROPERTY
}
