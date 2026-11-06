#pragma once

#include "Containers/HashTable.h"
#include "Containers/Name.h"
#include "Containers/Span.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Memory/SmartPtr.h"
#include "Platform/GenericPlatform.h"

namespace Lumina::Scripting
{
    struct FManagedTypeDefinition;
}

namespace Lumina
{
    class CObject;
    class CScriptEnum;
    class CScriptStruct;
    class CStruct;
    struct FComponentOps;

    namespace Scripting
    {
        // Frees a rebuilt script type's name for its replacement, numbered by its internal index since it stays alive until reinstancing ends.
        void RenameSupersededType(CObject& Type);
    }

    // Every [Reflect] C# struct, minted once as a CScriptStruct under its name and the native struct it extends, if any.
    class RUNTIME_API FScriptStructRegistry
    {
    public:

        static FScriptStructRegistry& Get();

        FScriptStructRegistry();
        ~FScriptStructRegistry();
        FScriptStructRegistry(const FScriptStructRegistry&) = delete;
        FScriptStructRegistry& operator=(const FScriptStructRegistry&) = delete;
        FScriptStructRegistry(FScriptStructRegistry&&) = delete;
        FScriptStructRegistry& operator=(FScriptStructRegistry&&) = delete;

        // An unchanged struct keeps its object across a reload; a reshaped or removed one is superseded and kept for migration.
        void Refresh(TSpan<const Scripting::FManagedTypeDefinition> Definitions);

        /** Drops every minted type. Called when scripts unload with nothing to replace them. */
        void Clear();

        /** The minted type for a stable script type name, or null when no live type claims it. */
        NODISCARD CScriptStruct* Find(const FName& ScriptTypeName) const;

        // Bumped whenever a struct is superseded, so a holder caching a resolved pointer knows to resolve it again.
        NODISCARD uint64 GetGeneration() const { return Generation; }

    private:

        // Renamed aside rather than destroyed, since values of a superseded type migrate by field name from it.
        void Supersede(TStrongObjectPtr<CScriptStruct>&& Struct);

        // Makes a [Reflect] C# struct implementing IComponent an ECS component, or moves its live pools to a new layout.
        void RegisterComponent(const FName& Name, CScriptStruct& Struct);

        // Minted types by name. The registry is their owner.
        THashMap<FName, TStrongObjectPtr<CScriptStruct>> Types;

        // Superseded types, kept so a value still laid out by one can be read and destroyed.
        TVector<TStrongObjectPtr<CScriptStruct>> Superseded;

        // One per component type name for the life of the process, since its ECS type id outlives every reshape.
        TVector<TUniquePtr<FComponentOps>> ComponentOps;

        uint64 Generation = 0;
    };

    // Every [Reflect] C# enum, minted once as a CScriptEnum under its name. Its members are replaced in place on a reload.
    class RUNTIME_API FScriptEnumRegistry
    {
    public:

        static FScriptEnumRegistry& Get();

        void Refresh(TSpan<const Scripting::FManagedTypeDefinition> Definitions);

    private:

        THashMap<FName, TStrongObjectPtr<CScriptEnum>> Enums;
    };

    // The struct a stored name means, native or C#, which share one name space since a C# type never takes a native name.
    NODISCARD RUNTIME_API CStruct* ResolveStructByName(const FName& Name);

    // The name an asset stores for this type, which a superseded C# struct keeps although its object was renamed aside.
    NODISCARD RUNTIME_API FName StructIdentity(const CStruct* Struct);

    // A resolved struct name, resolved again when the name changes or a reload supersedes a C# struct.
    struct FStructResolveCache
    {
        NODISCARD CStruct* Resolve(const FName& Name) const
        {
            const uint64 Now = FScriptStructRegistry::Get().GetGeneration();
            if (CachedName != Name || CachedGeneration != Now)
            {
                CachedName = Name;
                CachedGeneration = Now;
                Cached = Name.IsNone() ? nullptr : ResolveStructByName(Name);
            }
            return Cached;
        }

        void Set(CStruct* Struct, const FName& Name)
        {
            Cached = Struct;
            CachedName = Name;
            CachedGeneration = FScriptStructRegistry::Get().GetGeneration();
        }

    private:

        mutable CStruct* Cached = nullptr;
        mutable FName    CachedName;
        mutable uint64   CachedGeneration = ~0ull;
    };
}
