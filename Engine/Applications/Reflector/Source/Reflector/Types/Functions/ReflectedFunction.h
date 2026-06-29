#pragma once
#include "EASTL/vector.h"
#include "Reflector/Types/StructReflectItem.h"
#include "Reflector/Utils/MetadataUtils.h"
#include <Reflector/Types/FieldInfo.h>

#include "EASTL/optional.h"

namespace Lumina
{
    class FReflectedFunction : public IStructReflectable
    {
    public:

        FReflectedFunction() = default;
        
        void GenerateMetadata(const eastl::string& InMetadata) override;

        void AddArgument(FFieldInfo&& Field) { Arguments.emplace_back(Field); }

        eastl::optional<FFieldInfo>     Return;
        eastl::vector<FFieldInfo>       Arguments;
        eastl::vector<FMetadataPair>    Metadata;
        eastl::string                   Name;
        eastl::string                   Outer;
        // True when an unsupported argument was dropped from Arguments during parsing (LRT1005). The C#
        // binder must skip such a function: its reflected arg list is shorter than the real signature, so
        // a generated call would pass too few args.
        bool                            bHasOmittedArgs = false;

        // True for a C++ virtual method. A FUNCTION(ScriptEvent) must be virtual: the Scriptable codegen
        // generates a native shim override + a reverse-dispatch managed thunk so a C# subclass can override it.
        bool                            bIsVirtual = false;

        //~ Free-function (SCRIPT_EXPORT) fields. A free function has no owning type: it binds to a named C#
        //  static class and is called by its fully-qualified name in the generated thunk.
        bool                            bFreeFunction = false;
        eastl::string                   QualifiedName; // C++ fully-qualified function name (the thunk call target)
        eastl::string                   CSharpTarget;  // target C# class, optionally namespaced ("Lumina.Native")
    };
}
