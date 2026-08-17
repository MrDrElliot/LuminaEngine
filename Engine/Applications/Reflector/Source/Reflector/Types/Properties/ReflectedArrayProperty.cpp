#include "ReflectedArrayProperty.h"

#include "Reflector/CodeGeneration/CodeWriter.h"
#include "Reflector/Types/ReflectedType.h"

namespace Lumina
{
    void FReflectedArrayProperty::AppendDefinition(Reflection::FCodeWriter& Writer) const
    {
        // FArrayPropertyParams carries a single GetOpsFn: the per-property forwarder that returns the shared
        // GetVectorOpsFor<Container> table (a static member so the container type resolves in scope).
        const eastl::string CustomData = AccessorScope + Name + "ArrayOps_WrapperImpl";
        const eastl::string PropertyFlagStr = PropertyFlagsToString(PropertyFlags);
        AppendPropertyDef(Writer, PropertyFlagStr.c_str(), "Lumina::EPropertyTypeFlags::Vector", CustomData);
    }

    bool FReflectedArrayProperty::HasAccessors()
    {
        return true;
    }

    bool FReflectedArrayProperty::DeclareAccessors(Reflection::FCodeWriter& Writer, const eastl::string& FileID)
    {
        FReflectedProperty::DeclareAccessors(Writer, FileID);

        Writer.Macrof("static void %sArrayGetter_WrapperImpl(const void* Object, void* OutValue);", Name.c_str());
        Writer.Macrof("static const ::Lumina::FVectorOps* %sArrayOps_WrapperImpl();", Name.c_str());

        return true;
    }

    bool FReflectedArrayProperty::DefineAccessors(Reflection::FCodeWriter& Writer, Reflection::FReflectedType* ReflectedType)
    {
        FReflectedProperty::DefineAccessors(Writer, ReflectedType);

        const eastl::string& Q = AccessorDefinitionScope;
        const char* N = Name.c_str();
        const char* Raw = RawTypeName.c_str();      // The container type, e.g. TVector<T> or TFixedVector<T, N>.

        // Object is the container instance itself (&TVector<T>), not the owning struct.
        // The caller resolves the member offset via GetValuePtr, so arrays compose.

        // Getter (exposes the raw vector pointer for debug / inspection).
        Writer.Linef("void %s%sArrayGetter_WrapperImpl(const void* Object, void* OutValue)", Q.c_str(), N);
        Writer.BeginBlock();
        Writer.Linef("*(const %s**)OutValue = (const %s*)Object;", Raw, Raw);
        Writer.EndBlock();
        Writer.Line();

        // The whole operation set is the container's ops table. Resolving it here (a member of the owning
        // type) lets the unqualified container name compile, unlike the global-scope params initializer. It is
        // keyed on the container, not the element: a fixed container must be grown as its own type.
        Writer.Linef("const ::Lumina::FVectorOps* %s%sArrayOps_WrapperImpl()", Q.c_str(), N);
        Writer.BeginBlock();
        Writer.Linef("return ::Lumina::GetVectorOpsFor<%s>();", Raw);
        Writer.EndBlock();
        Writer.Line();

        return true;
    }

    bool FReflectedArrayProperty::GenerateLuaBinding(Reflection::FCodeWriter& Writer)
    {
        return true;
    }
}
