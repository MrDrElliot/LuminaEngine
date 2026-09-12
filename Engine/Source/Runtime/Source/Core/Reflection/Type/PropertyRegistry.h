#pragma once

#include "Core/Object/ObjectCore.h"
#include "Core/Object/PropertyArena.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class FProperty;
}

namespace Lumina
{
    /**
     * Everything the engine needs to know about one reflected property kind, in one place.
     *
     * A kind used to be spelled out once per concern: construction in ObjectCore, footprint and construction
     * again in ScriptStruct, and how many inner params the emitter writes after it. Adding a kind meant
     * finding every one of those switches. A kind now registers itself here and each walker asks the table.
     */
    struct FPropertyKindOps
    {
        /**
         * Places the property in the owner's arena and attaches it.
         *
         * Params must be this kind's own params type wherever the kind reads one (a struct's StructFunc, an
         * object's ClassFunc). The arithmetic kinds read only the base, so a bare FPropertyParams is enough
         * for them, which is what lets a script schema build one without knowing the subtype.
         */
        FProperty* (*Construct)(const FPropertyOwner& Owner, const FPropertyParams* Params) = nullptr;

        /**
         * Reads the kind's trailing metadata table.
         *
         * Valid only for the params the Reflector emits, which is the only place a metadata array comes from.
         * A script-built params struct has no table and must not be passed here.
         */
        void (*GetMetadata)(const FPropertyParams* Params, uint16& OutNum, const FMetaDataPairParam*& OutArray) = nullptr;

        /** Value footprint when the kind has one that does not depend on a resolved type; 0 means ask the type. */
        uint32 Size = 0;
        uint32 Alignment = 0;

        /** Inner params the emitter writes after this one: an array element, a map's key and value, an enum's
         *  underlying integer, an optional's payload. */
        uint8 NumInnerParams = 0;

        /** An arithmetic kind, which is what a script schema can lay out from its footprint alone. */
        bool bArithmetic = false;

        /** True when the kind's params add nothing but the metadata tail, so Construct reads only the base.
         *  That is what lets a caller with no kind-specific data build one from a bare FPropertyParams. */
        bool bBaseParamsOnly = false;

        NODISCARD bool IsValid() const { return Construct != nullptr; }

        /** Fills Size/Alignment from the table when the kind has a fixed footprint. */
        NODISCARD bool GetFixedLayout(uint32& OutSize, uint32& OutAlignment) const
        {
            if (Size == 0)
            {
                return false;
            }
            OutSize = Size;
            OutAlignment = Alignment;
            return true;
        }
    };

    /** Never null: an unregistered kind answers with an entry whose IsValid() is false. */
    RUNTIME_API const FPropertyKindOps& GetPropertyKindOps(EPropertyTypeFlags Kind);
}
