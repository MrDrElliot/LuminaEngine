#pragma once

#include "Assets/AssetTypes/Blackboard/Blackboard.h"
#include "Containers/Array.h"
#include "Containers/Name.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Math/Math.h"
#include "World/Entity/EntityHandle.h"
#include "BlackboardComponent.generated.h"

namespace Lumina
{
    // Per-instance runtime value for one blackboard key. Which field is live is
    // determined by the matching FBlackboardKey::Type on the schema.
    struct FBlackboardValue
    {
        float                Scalar = 0.0f;
        FVector3             Vector = FVector3(0.0f);
        TObjectPtr<CObject>  Object;
        FEntity              Entity = NullEntity;
    };

    // Per-entity blackboard: references a CBlackboard schema, owns its value store (seeded from schema
    // defaults). Entities sharing a schema keep independent values. Read/written via the Set*/Get* API.
    //
    // Accessors are type-loose on purpose: the schema decides which field of FBlackboardValue a key uses,
    // so reading a Vector key with GetFloat reads a scalar that was never written (0). Match the getter to
    // the key's declared type, or branch on GetKeyType. Writing a key the schema doesn't declare is
    // allowed and creates a scratch value that lives as long as the component.
    REFLECT(Component, Category = "AI")
    struct SBlackboardComponent
    {
        GENERATED_BODY()

        /** Schema this instance is initialized from. */
        PROPERTY(Script, Editable, Category = "Blackboard")
        TObjectPtr<CBlackboard> Blackboard;

        FUNCTION(Script)
        RUNTIME_API void SetFloat(const FName& Key, float Value);

        FUNCTION(Script)
        RUNTIME_API float GetFloat(const FName& Key, float Default = 0.0f) const;

        FUNCTION(Script)
        RUNTIME_API void SetInt(const FName& Key, int32 Value);

        FUNCTION(Script)
        RUNTIME_API int32 GetInt(const FName& Key, int32 Default = 0) const;

        FUNCTION(Script)
        RUNTIME_API void SetBool(const FName& Key, bool bValue);

        FUNCTION(Script)
        RUNTIME_API bool GetBool(const FName& Key, bool Default = false) const;

        FUNCTION(Script)
        RUNTIME_API void SetVector(const FName& Key, const FVector3& Value);

        FUNCTION(Script)
        RUNTIME_API FVector3 GetVector(const FName& Key, const FVector3& Default = FVector3(0.0f)) const;

        // Named *ObjectValue because the Win32 wingdi.h GetObject macro rewrites a plain GetObject.
        FUNCTION(Script)
        RUNTIME_API void SetObjectValue(const FName& Key, CObject* Value);

        FUNCTION(Script)
        RUNTIME_API CObject* GetObjectValue(const FName& Key) const;

        // Spelled entt::entity (not the FEntity alias) so the Reflector's script binder recognizes it and
        // marshals it as the C# Entity handle.

        /** Entity keys hold a live handle; an unset key reads back as the null entity. */
        FUNCTION(Script)
        RUNTIME_API void SetEntity(const FName& Key, entt::entity Value);

        FUNCTION(Script)
        RUNTIME_API entt::entity GetEntity(const FName& Key) const;

        /** True if the schema declares Key (scratch values written by Set* don't count). */
        FUNCTION(Script)
        RUNTIME_API bool HasKey(const FName& Key) const;

        /** Type the schema declares for Key, or Fallback when there's no schema or no such key. */
        FUNCTION(Script)
        RUNTIME_API EBlackboardKeyType GetKeyType(const FName& Key, EBlackboardKeyType Fallback = EBlackboardKeyType::Float) const;

        /** Restores Key to its schema default. Returns false if the schema doesn't declare it. */
        FUNCTION(Script)
        RUNTIME_API bool ClearValue(const FName& Key);

        /** Restores every key to its schema default (drops scratch values written for undeclared keys). */
        FUNCTION(Script)
        RUNTIME_API void ResetToDefaults();

        // (Re)seeds the value store from the schema defaults when first used or when the referenced
        // Blackboard asset changes, and picks up keys added to the schema since. Safe to call every
        // frame; the const accessors call it themselves, so reads see defaults before any write.
        RUNTIME_API void EnsureInitialized() const;

    private:

        // Live per-instance values, keyed by name. Mutable so a const getter can seed defaults on first read.
        mutable THashMap<FName, FBlackboardValue> Values;

        // Schema the Values store was seeded from; re-seed when it changes.
        mutable const CBlackboard* SeededSchema = nullptr;
    };
}
