#pragma once
#include <vector>
#include "Reflector/Types/StructReflectItem.h"
#include "Reflector/Utils/MetadataUtils.h"
#include <Reflector/Types/FieldInfo.h>

#include <optional>

namespace Lumina
{
    class FReflectedFunction : public IStructReflectable
    {
    public:

        FReflectedFunction() = default;
        
        void GenerateMetadata(const std::string& InMetadata) override;

        void AddArgument(FFieldInfo&& Field) { Arguments.emplace_back(Field); }

        std::optional<FFieldInfo>     Return;
        std::vector<FFieldInfo>       Arguments;
        std::vector<FMetadataPair>    Metadata;
        std::string                   Name;
        std::string                   Outer;
        // True when an unsupported argument was dropped from Arguments during parsing (LRT1005). The C#
        // binder must skip such a function: its reflected arg list is shorter than the real signature, so
        // a generated call would pass too few args.
        bool                            bHasOmittedArgs = false;

        // True for a C++ virtual method. Only a virtual is overridable from C#: the Scriptable codegen
        // generates a native shim override + a reverse-dispatch managed thunk so a C# subclass can override it.
        bool                            bIsVirtual = false;

        //~ Free-function (SCRIPT_EXPORT) fields. A free function has no owning type: it binds to a named C#
        //  static class and is called by its fully-qualified name in the generated thunk.
        bool                            bFreeFunction = false;
        std::string                   QualifiedName; // C++ fully-qualified function name (the thunk call target)
        std::string                   CSharpTarget;  // target C# class, optionally namespaced ("Lumina.Native")
    };
}
