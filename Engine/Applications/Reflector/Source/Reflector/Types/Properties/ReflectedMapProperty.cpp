#include "ReflectedMapProperty.h"

#include "Reflector/CodeGeneration/CodeWriter.h"
#include "Reflector/Types/ReflectedType.h"

namespace Lumina
{
    void FReflectedMapProperty::AppendDefinition(Reflection::FCodeWriter& Writer) const
    {
        // FMapPropertyParams carries a single GetOpsFn: the per-property forwarder that returns the shared
        // GetMapOps<Key,Value> table (a static member so both template types resolve in scope).
        const eastl::string CustomData = AccessorScope + Name + "MapOps_WrapperImpl";
        const eastl::string PropertyFlagStr = PropertyFlagsToString(PropertyFlags);
        AppendPropertyDef(Writer, PropertyFlagStr.c_str(), "Lumina::EPropertyTypeFlags::Map", CustomData);
    }

    bool FReflectedMapProperty::HasAccessors()
    {
        return true;
    }

    bool FReflectedMapProperty::DeclareAccessors(Reflection::FCodeWriter& Writer, const eastl::string& FileID)
    {
        FReflectedProperty::DeclareAccessors(Writer, FileID);

        Writer.Macrof("static void %sMapGetter_WrapperImpl(const void* Object, void* OutValue);", Name.c_str());
        Writer.Macrof("static const ::Lumina::FMapOps* %sMapOps_WrapperImpl();", Name.c_str());

        return true;
    }

    bool FReflectedMapProperty::DefineAccessors(Reflection::FCodeWriter& Writer, Reflection::FReflectedType* ReflectedType)
    {
        FReflectedProperty::DefineAccessors(Writer, ReflectedType);

        const eastl::string& Q = AccessorDefinitionScope;
        const char* N = Name.c_str();
        const char* Raw = RawTypeName.c_str();       // The container type, e.g. THashMap<K,V>.
        const char* Key = KeyTypeName.c_str();       // The key type K.
        const char* Val = ValueTypeName.c_str();     // The value type V.

        // Object is the container instance itself (&THashMap<K,V>); the caller resolves the member offset.

        // Getter (exposes the raw map pointer for debug / inspection).
        Writer.Linef("void %s%sMapGetter_WrapperImpl(const void* Object, void* OutValue)", Q.c_str(), N);
        Writer.BeginBlock();
        Writer.Linef("*(const %s**)OutValue = (const %s*)Object;", Raw, Raw);
        Writer.EndBlock();
        Writer.Line();

        // The whole operation set is the shared key/value ops table. Resolving it here (a member of the owning
        // type) lets the unqualified key/value names compile, unlike the global-scope params initializer.
        Writer.Linef("const ::Lumina::FMapOps* %s%sMapOps_WrapperImpl()", Q.c_str(), N);
        Writer.BeginBlock();
        Writer.Linef("return ::Lumina::GetMapOps<%s, %s>();", Key, Val);
        Writer.EndBlock();
        Writer.Line();

        return true;
    }

    bool FReflectedMapProperty::GenerateLuaBinding(Reflection::FCodeWriter& Writer)
    {
        return true;
    }
}
