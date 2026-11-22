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
        uint32_t                      LineNumber = 0;
        // An argument was dropped (LRT1005), so the C# binder must skip this function or its call would pass too few args.
        bool                            bHasOmittedArgs = false;

        // Only a virtual is overridable from C#, through a generated native shim and managed thunk.
        bool                            bIsVirtual = false;

        // Declared = 0, so a C# override has no native body to fall back on.
        bool                            bIsPureVirtual = false;

        bool                            bIsStatic = false;
        bool                            bIsConst = false;

        //~ Native FFunction reflection, left off when any argument or return cannot be described and stored in a frame.
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
        /// holds a TStrongObjectPtr<CObject>, because that is the storage FObjectProperty describes.
        std::vector<std::string>                         ParameterObjectCastTypes;

        /// True where the signature takes an rvalue reference, so the call moves out of the frame's slot.
        /// A moved-from value is still destructible, which is all the frame teardown needs of it.
        std::vector<bool>                                ParameterIsRvalue;

        /// Index into TopLevelParams of the return value, or -1 when the function returns void.
        int                                              ReturnIndex = -1;

        //~ Free-function (SCRIPT_EXPORT) fields, bound to a named C# static class since there is no owning type.
        bool                            bFreeFunction = false;
        std::string                   QualifiedName; // C++ fully-qualified function name (the thunk call target)
        std::string                   CSharpTarget;  // target C# class, optionally namespaced ("Lumina.Native")
    };
}
