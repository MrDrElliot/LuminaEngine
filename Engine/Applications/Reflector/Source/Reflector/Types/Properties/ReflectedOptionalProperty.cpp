#include "ReflectedOptionalProperty.h"

#include "Reflector/CodeGeneration/CodeWriter.h"
#include "Reflector/Types/ReflectedType.h"

namespace Lumina
{
    namespace
    {
        // Order must match FOptionalPropertyParams: HasValue, GetValue, SetValue, Reset.
        constexpr const char* kOptionalFatTailSuffixes[] =
        {
            "OptionalHasValue_WrapperImpl",
            "OptionalGetValue_WrapperImpl",
            "OptionalSetValue_WrapperImpl",
            "OptionalReset_WrapperImpl",
        };
    }

    void FReflectedOptionalProperty::AppendDefinition(Reflection::FCodeWriter& Writer) const
    {
        eastl::string CustomData;
        constexpr size_t kNumSuffixes = std::size(kOptionalFatTailSuffixes);
        for (size_t i = 0; i < kNumSuffixes; ++i)
        {
            if (i > 0)
            {
                CustomData += ", ";
            }
            CustomData += AccessorScope + Name + kOptionalFatTailSuffixes[i];
        }

        const eastl::string PropertyFlagStr = PropertyFlagsToString(PropertyFlags);
        AppendPropertyDef(Writer, PropertyFlagStr.c_str(), "Lumina::EPropertyTypeFlags::Optional", CustomData);
    }

    bool FReflectedOptionalProperty::HasAccessors()
    {
        return true;
    }

    bool FReflectedOptionalProperty::DeclareAccessors(Reflection::FCodeWriter& Writer, const eastl::string& FileID)
    {
        FReflectedProperty::DeclareAccessors(Writer, FileID);

        Writer.Macrof("static bool  %sOptionalHasValue_WrapperImpl(const void* Object);", Name.c_str());
        Writer.Macrof("static void* %sOptionalGetValue_WrapperImpl(void* Object);", Name.c_str());
        Writer.Macrof("static void  %sOptionalSetValue_WrapperImpl(void* Object, const void* InValue);", Name.c_str());
        Writer.Macrof("static void  %sOptionalReset_WrapperImpl(void* Object);", Name.c_str());

        return true;
    }

    bool FReflectedOptionalProperty::DefineAccessors(Reflection::FCodeWriter& Writer, Reflection::FReflectedType* ReflectedType)
    {
        FReflectedProperty::DefineAccessors(Writer, ReflectedType);

        const eastl::string& Q = AccessorDefinitionScope;
        const char* N = Name.c_str();
        const char* Raw = RawTypeName.c_str();      // The wrapper type, e.g. TOptional<T>.
        const char* Elem = ElementTypeName.c_str(); // The payload type T.

        // Object is the optional instance itself (&TOptional<T>), not the owning struct.
        // The caller resolves the member offset via GetValuePtr, so optionals compose.

        // HasValue
        Writer.Linef("bool %s%sOptionalHasValue_WrapperImpl(const void* Object)", Q.c_str(), N);
        Writer.BeginBlock();
        Writer.Linef("return ((const %s*)Object)->has_value();", Raw);
        Writer.EndBlock();
        Writer.Line();

        // GetValue (raw pointer to held T; only valid when HasValue is true).
        Writer.Linef("void* %s%sOptionalGetValue_WrapperImpl(void* Object)", Q.c_str(), N);
        Writer.BeginBlock();
        Writer.Linef("%s* Opt = (%s*)Object;", Raw, Raw);
        Writer.Line("return Opt->has_value() ? &Opt->value() : nullptr;");
        Writer.EndBlock();
        Writer.Line();

        // SetValue: copy-emplace when given a value, default-construct when null.
        Writer.Linef("void %s%sOptionalSetValue_WrapperImpl(void* Object, const void* InValue)", Q.c_str(), N);
        Writer.BeginBlock();
        Writer.Linef("%s* Opt = (%s*)Object;", Raw, Raw);
        Writer.Line("if (InValue)");
        Writer.BeginBlock();
        Writer.Linef("*Opt = *(const %s*)InValue;", Elem);
        Writer.EndBlock();
        Writer.Line("else");
        Writer.BeginBlock();
        Writer.Line("Opt->emplace();");
        Writer.EndBlock();
        Writer.EndBlock();
        Writer.Line();

        // Reset
        Writer.Linef("void %s%sOptionalReset_WrapperImpl(void* Object)", Q.c_str(), N);
        Writer.BeginBlock();
        Writer.Linef("((%s*)Object)->reset();", Raw);
        Writer.EndBlock();
        Writer.Line();

        return true;
    }

    bool FReflectedOptionalProperty::GenerateLuaBinding(Reflection::FCodeWriter& Writer)
    {
        return true;
    }
}
