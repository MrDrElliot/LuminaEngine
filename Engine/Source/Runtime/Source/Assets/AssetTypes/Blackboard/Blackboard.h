#pragma once

#include "Core/LuminaMacros.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Containers/Array.h"
#include "Core/Math/Math.h"
#include "Scripting/ScriptDataStruct.h"
#include "Blackboard.generated.h"

namespace Lumina
{
    // Blackboard key value type; anim system uses the scalar-carried numerics, Vector/Object/Entity are
    // for AI. Append new types at the end: the value is what assets serialize.
    REFLECT()
    enum class EBlackboardKeyType : uint8
    {
        Float,
        Int,
        Bool,
        Enum,
        Vector,
        Object,
        Entity,
    };

    // Per-key behavior flags.
    REFLECT()
    enum class EBlackboardKeyFlags : uint8
    {
        None     = 0,

        // Read-only at runtime (constants/inputs); the editor preview disables editing it.
        ReadOnly = 1 << 0,

        // Hidden from key pickers; for internal/scratch keys not meant to be wired by hand.
        Hidden   = 1 << 1,
    };
    ENUM_CLASS_FLAGS(EBlackboardKeyFlags)

    /** Marker a struct derives from to offer itself as a blackboard's backing type.
     *
     *  Carries nothing. It exists so the editor can list the types meant to be used this way rather
     *  than every reflected struct in the build, and so declaring one is a deliberate act. Works the
     *  same from C#, where a script struct deriving from it registers as an ordinary CStruct.
     */
    REFLECT()
    struct RUNTIME_API SBlackboardDataBase
    {
        GENERATED_BODY()
    };

    // Schema-only declaration of one blackboard entry (name, type, default); live per-instance
    // values live in SBlackboardComponent, seeded from these defaults.
    REFLECT()
    struct FBlackboardKey
    {
        GENERATED_BODY()

        /** Identifier scripts / the animation graph reference this key by. */
        PROPERTY(Editable, Category = "Blackboard")
        FName Name;

        /** Value type held under this key. */
        PROPERTY(Editable, Category = "Blackboard")
        EBlackboardKeyType Type = EBlackboardKeyType::Float;

        /** Behavior flags (read-only, hidden from pickers, ...). */
        PROPERTY(Editable, Category = "Blackboard")
        EBlackboardKeyFlags Flags = EBlackboardKeyFlags::None;

        /** Default for Float keys (and the 0/1 source for Bool when unset). */
        PROPERTY(Editable, Category = "Blackboard")
        float DefaultFloat = 0.0f;

        /** Default for Int keys, and the value for Enum keys. */
        PROPERTY(Editable, Category = "Blackboard")
        int32 DefaultInt = 0;

        /** Default for Bool keys. */
        PROPERTY(Editable, Category = "Blackboard")
        bool DefaultBool = false;

        /** For Enum keys: registered name of the reflected CEnum (e.g. "EClipLoopMode"); DefaultInt holds the value. */
        PROPERTY(Editable, Category = "Blackboard")
        FName EnumType;

        /** Default for Vector keys. */
        PROPERTY(Editable, Category = "Blackboard")
        FVector3 DefaultVector = FVector3(0.0f);

        /** Default for Object keys. */
        PROPERTY(Editable, Category = "Blackboard")
        TObjectPtr<CObject> DefaultObject;

        // Entity keys have no authorable default: an entity id is only meaningful inside a live world,
        // so they always start unset and are written at runtime.

        /** Set when this key was generated from the backing struct rather than authored here. Such a
         *  key is rewritten on every sync, so editing it by hand would not survive. */
        PROPERTY()
        bool bDerived = false;
    };

    // True for the key types the animation VM reads as a float register. Vector/Object/Entity keys carry
    // no scalar, so they can't drive a graph parameter.
    constexpr bool IsScalarBlackboardKey(EBlackboardKeyType Type)
    {
        return Type == EBlackboardKeyType::Float
            || Type == EBlackboardKeyType::Int
            || Type == EBlackboardKeyType::Bool
            || Type == EBlackboardKeyType::Enum;
    }

    // Display name for a key type, for editor UI and compile diagnostics.
    constexpr const char* BlackboardKeyTypeLabel(EBlackboardKeyType Type)
    {
        switch (Type)
        {
        case EBlackboardKeyType::Float:  return "Float";
        case EBlackboardKeyType::Int:    return "Int";
        case EBlackboardKeyType::Bool:   return "Bool";
        case EBlackboardKeyType::Enum:   return "Enum";
        case EBlackboardKeyType::Vector: return "Vector";
        case EBlackboardKeyType::Object: return "Object";
        case EBlackboardKeyType::Entity: return "Entity";
        }
        return "Unknown";
    }

    // Reusable blackboard schema asset (typed keys + defaults), shared read-only. Live values are per-entity
    // in SBlackboardComponent, so entities sharing a schema keep independent values.
    REFLECT()
    class RUNTIME_API CBlackboard : public CObject
    {
        GENERATED_BODY()

    public:

        bool IsAsset() const override { return true; }

        int32 FindKeyIndex(const FName& Name) const;
        const FBlackboardKey* FindKey(const FName& Name) const;

        /** Number of keys declared by this schema. */
        FUNCTION(Script)
        int32 NumKeys() const { return (int32)Keys.size(); }

        /** True if this schema declares a key called Name. */
        FUNCTION(Script)
        bool HasKey(const FName& Name) const { return FindKeyIndex(Name) != INDEX_NONE; }

        /** Type declared for Name, or Fallback when the schema has no such key. */
        FUNCTION(Script)
        EBlackboardKeyType GetKeyType(const FName& Name, EBlackboardKeyType Fallback = EBlackboardKeyType::Float) const;

        /** Name of the key at Index, or None when out of range. */
        FUNCTION(Script)
        FName GetKeyName(int32 Index) const;

        /** Optional backing type. Every reflected property on it whose type maps to a key type becomes
         *  a derived key, so a schema can be declared as an ordinary struct in C++ or C# and the asset
         *  picks it up instead of the keys being retyped by hand in two places.
         *
         *  Stored as a name for the same reason CDataTable::RowStructName is: a CStruct is a reflection
         *  object built at module init, not a serializable asset, so a pointer to one cannot go in a
         *  package. Read it through GetBackingStruct(), which resolves and caches. */
        PROPERTY()
        FName BackingStructName;

        /** The resolved backing type, or null if unset or its module is not loaded. */
        NODISCARD CStruct* GetBackingStruct() const;

        /** Points the schema at a new backing type and syncs immediately. Keys authored by hand are
         *  kept; the previous type's derived keys are dropped. */
        void SetBackingStruct(CStruct* InStruct);

        /** Rebuilds the derived keys from the backing struct, preserving hand-authored ones. Returns
         *  true when Keys actually changed, so a caller can avoid dirtying the package for nothing. */
        bool SyncKeysFromBackingStruct();

        /** Key declarations that make up this blackboard's schema. Derived keys come first, in the
         *  backing struct's property order; hand-authored ones follow. */
        PROPERTY(Editable, Category = "Blackboard")
        TVector<FBlackboardKey> Keys;

    private:

        /** Resolution result for BackingStructName. Not serialized: reflection objects live at different
         *  addresses each run, and a script-declared type is re-minted on every reload. */
        FDataStructResolveCache BackingStructCache;
    };
}
