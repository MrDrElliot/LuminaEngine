#pragma once
#include <vector>
#include "Reflector/Types/StructReflectItem.h"
#include "Reflector/Utils/MetadataUtils.h"
#include <Reflector/Types/FieldInfo.h>

#include <optional>
#include <memory>
#include "Reflector/Types/Properties/ReflectedProperty.h"

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

        bool                            bIsStatic = false;
        bool                            bIsConst = false;

        //~ Native FFunction reflection. Populated only when every argument and the return can be both
        //  described as a property and stored in a frame; a function that cannot is left unreflected
        //  natively rather than failing the build, since reflecting it is additive.
        bool                                             bReflectNatively = false;

        /// Every property describing the frame, flat and in emission order: a container's inners come
        /// immediately before it, exactly as a type's own property table lays its members out.
        std::vector<std::unique_ptr<FReflectedProperty>>  ParamEntries;

        /// The frame's actual members, in declaration order, return last when there is one. Points into
        /// ParamEntries and excludes the inners, which are described by their owner rather than declared.
        std::vector<FReflectedProperty*>                 TopLevelParams;

        /// Storage type spellings parallel to TopLevelParams, which is what the frame struct declares.
        std::vector<std::string>                         ParameterStorageTypes;

        /// The declared pointer type an object parameter is cast back to, empty for anything else. The frame
        /// holds a TObjectPtr<CObject>, because that is the storage FObjectProperty describes.
        std::vector<std::string>                         ParameterObjectCastTypes;

        /// True for a parameter the real signature takes by reference, so the thunk hands the slot over.
        std::vector<bool>                                ParameterIsReference;

        /// True where the signature takes an rvalue reference, so the call moves out of the frame's slot.
        /// A moved-from value is still destructible, which is all the frame teardown needs of it.
        std::vector<bool>                                ParameterIsRvalue;

        /// Index into TopLevelParams of the return value, or -1 when the function returns void.
        int                                              ReturnIndex = -1;

        //~ Free-function (SCRIPT_EXPORT) fields. A free function has no owning type: it binds to a named C#
        //  static class and is called by its fully-qualified name in the generated thunk.
        bool                            bFreeFunction = false;
        std::string                   QualifiedName; // C++ fully-qualified function name (the thunk call target)
        std::string                   CSharpTarget;  // target C# class, optionally namespaced ("Lumina.Native")
    };
}
