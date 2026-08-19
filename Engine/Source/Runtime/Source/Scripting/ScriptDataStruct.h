#pragma once

#include "Containers/HashTable.h"
#include "Containers/Name.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CScriptStruct;
    class CStruct;

    /**
     * The C# data types published to the engine, minted as CScriptStructs whose super is the native
     * struct their marker named ([DataTableRow] -> FTableRowBase, and so on).
     *
     * One registry serves every feature that accepts a native base, because they all need the same three
     * things: discovery that does not depend on anything referencing the type, a real inheritance edge so
     * IsChildOf works, and an identity that outlives the minted object.
     */
    class RUNTIME_API FScriptDataStructRegistry
    {
    public:

        static FScriptDataStructRegistry& Get();

        /**
         * Rebuilds from the loaded script generation. Every type is re-minted rather than reused: a reload
         * can change a type's fields, and a layout is not something that can be patched in place.
         */
        void Refresh();

        /** Drops every minted type. Called when scripts unload with nothing to replace them. */
        void Clear();

        /** The minted type for a stable script type name, or null when no live type claims it. */
        NODISCARD CScriptStruct* Find(const FName& ScriptTypeName) const;

        /**
         * Bumped on every Refresh. A holder that caches a resolved pointer compares this against the value
         * it resolved at; anything older is from a generation whose objects no longer exist. This is what
         * makes a cached CStruct* safe without any of them having to be told about a reload.
         */
        NODISCARD uint64 GetGeneration() const { return Generation; }

    private:

        /** Minted types by stable script type name. Strong refs: the registry is their only owner. */
        THashMap<FName, TObjectPtr<CScriptStruct>> Types;

        uint64 Generation = 0;
    };

    /**
     * Resolves a stored struct name to the type it names, native or script-declared.
     *
     * Native first, then the script registry. The two share one flat name space (an asset stores a bare
     * name either way) and the registry refuses to mint over a native name, so the order is a statement
     * of precedence rather than a tie-break: a native type always wins, and a script type can never
     * change what an existing stored name means.
     */
    RUNTIME_API NODISCARD CStruct* ResolveDataStructByName(const FName& Name);

    /**
     * The name an asset stores to refer to this type later.
     *
     * A script type answers with its ScriptTypeName metadata, not its object name. The two happen to
     * agree today, and relying on that would make the minting code's choice of name a load-bearing
     * detail of the on-disk format; asking for the identity keeps the stored value stable no matter
     * how a minted object comes to be named.
     */
    RUNTIME_API NODISCARD FName DataStructIdentity(const CStruct* Struct);

    /**
     * Cache for a resolved struct name, so a holder re-resolves exactly when it must.
     *
     * A script type is re-minted on every reload, which invalidates every pointer to the previous one.
     * Comparing the registry generation is what catches that without the registry having to know who its
     * consumers are; comparing the name catches an edit.
     */
    struct FDataStructResolveCache
    {
        NODISCARD CStruct* Resolve(const FName& Name) const
        {
            const uint64 Now = FScriptDataStructRegistry::Get().GetGeneration();
            if (CachedName != Name || CachedGeneration != Now)
            {
                CachedName = Name;
                CachedGeneration = Now;
                Cached = Name.IsNone() ? nullptr : ResolveDataStructByName(Name);
            }
            return Cached;
        }

        void Set(CStruct* Struct, const FName& Name)
        {
            Cached = Struct;
            CachedName = Name;
            CachedGeneration = FScriptDataStructRegistry::Get().GetGeneration();
        }

    private:

        mutable CStruct* Cached = nullptr;
        mutable FName    CachedName;
        mutable uint64   CachedGeneration = ~0ull;
    };
}
