#include "ReflectedMapProperty.h"

#include "Reflector/CodeGeneration/CodeWriter.h"
#include "Reflector/Types/ReflectedType.h"

namespace Lumina
{
    void FReflectedMapProperty::AppendDefinition(Reflection::FCodeWriter& Writer) const
    {
        // The per-property forwarder returns the shared GetMapOpsFor table, a static member for scope.
        const std::string CustomData = AccessorScope + Name + "MapOps_WrapperImpl";
        const std::string PropertyFlagStr = PropertyFlagsToString(PropertyFlags);
        AppendPropertyDef(Writer, PropertyFlagStr.c_str(), "Lumina::EPropertyTypeFlags::Map", CustomData);
    }

    bool FReflectedMapProperty::HasAccessors()
    {
        return true;
    }

    bool FReflectedMapProperty::DeclareAccessors(Reflection::FCodeWriter& Writer, const std::string& FileID)
    {
        FReflectedProperty::DeclareAccessors(Writer, FileID);

        Writer.Macrof("static void %sMapGetter_WrapperImpl(const void* Object, void* OutValue);", Name.c_str());
        Writer.Macrof("static const ::Lumina::FMapOps* %sMapOps_WrapperImpl();", Name.c_str());

        return true;
    }

    bool FReflectedMapProperty::DefineAccessors(Reflection::FCodeWriter& Writer, Reflection::FReflectedType* ReflectedType)
    {
        FReflectedProperty::DefineAccessors(Writer, ReflectedType);

        const std::string& Q = AccessorDefinitionScope;
        const char* N = Name.c_str();
        const char* Raw = RawTypeName.c_str();       // The container type, e.g. THashMap<K,V>.

        // Object is the container instance itself (&THashMap<K,V>); the caller resolves the member offset.

        // Getter (exposes the raw map pointer for debug / inspection).
        Writer.Linef("void %s%sMapGetter_WrapperImpl(const void* Object, void* OutValue)", Q.c_str(), N);
        Writer.BeginBlock();
        Writer.Linef("*(const %s**)OutValue = (const %s*)Object;", Raw, Raw);
        Writer.EndBlock();
        Writer.Line();

        // Keyed on the container, not the key and value, because a fixed map must be grown as its own type.
        Writer.Linef("const ::Lumina::FMapOps* %s%sMapOps_WrapperImpl()", Q.c_str(), N);
        Writer.BeginBlock();
        Writer.Linef("return ::Lumina::GetMapOpsFor<%s>();", Raw);
        Writer.EndBlock();
        Writer.Line();

        return true;
    }

    bool FReflectedMapProperty::GenerateLuaBinding(Reflection::FCodeWriter& Writer)
    {
        return true;
    }
}
