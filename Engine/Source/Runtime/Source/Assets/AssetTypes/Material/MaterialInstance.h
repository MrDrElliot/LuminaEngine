#pragma once

#include "Renderer/ShaderHandle.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "MaterialInterface.h"
#include "Containers/HashTable.h"
#include "Renderer/MaterialTypes.h"
#include "MaterialInstance.generated.h"

namespace Lumina
{
    class CMaterial;
    class CTexture;
}

namespace Lumina
{
    // One parameter an instance diverges on. Only divergent parameters are stored.
    REFLECT()
    struct RUNTIME_API FMaterialParameterOverride
    {
        GENERATED_BODY()

        PROPERTY()
        FName ParameterName;

        PROPERTY()
        EMaterialParameterType Type = EMaterialParameterType::Scalar;

        // A disabled override keeps its value but shows the parent's instead.
        PROPERTY()
        bool bEnabled = true;

        PROPERTY()
        float Scalar = 0.0f;

        PROPERTY()
        FVector4 Vector = FVector4(0.0f);

        PROPERTY()
        TStrongObjectPtr<CTexture> Texture;
    };

    // A static switch an instance flips. Absence is the inherit state, so it needs no enabled flag.
    REFLECT()
    struct RUNTIME_API FMaterialStaticSwitchOverride
    {
        GENERATED_BODY()

        PROPERTY()
        FName ParameterName;

        PROPERTY()
        bool bValue = false;
    };

    // Overrides a parent's parameter values and static switches. Its shaders come from the root's permutation for its switches.
    REFLECT()
    class RUNTIME_API CMaterialInstance : public CMaterialInterface
    {
        GENERATED_BODY()
    public:

        // A transient instance parented to Parent, with its own GPU slot seeded from Parent's values.
        static CMaterialInstance* CreateDynamic(CMaterialInterface* Parent);

        // A dynamic instance has no package, and must stay out of the registry and the save path.
        bool IsAsset() const override { return GetPackage() != nullptr; }
        void PostLoad() override;
        void OnDestroy() override;
        void PostPropertyChange(FProperty* ChangedProperty) override;
        void OnReferencesReplaced() override;

        CMaterialInterface* GetParentMaterial() const override { return Material.Get(); }
        CMaterial* GetMaterial() const override;
        bool SetScalarValue(const FName& Name, const float Value) override;
        bool SetVectorValue(const FName& Name, const FVector4& Value) override;
        bool SetTextureValue(const FName& Name, CTexture* TextureValue) override;
        bool GetParameterValue(EMaterialParameterType Type, const FName& Name, FMaterialParameter& Param) override;
        EMaterialShadingModel GetShadingModel() const override;
        uint64 GetStaticSwitchKey() const override;
        bool IsReadyForRender() const override;
        void RefreshFromParent() override;
        void RefreshInheritedTextureSlots() override;
        bool InheritParameterValue(EMaterialParameterType Type, const FName& Name, uint16 Index) override;
        uint32 GetResolvedTextureSlot(uint32 Index) override;
        CTexture* GetTextureParameterTexture(const FName& Name, uint32 Index) override;
        bool RefreshTextureBindings(const CTexture* ChangedTexture) override;
        bool RequestTexturesResolved() override;

        // Reparents with a cycle and depth guard and rebuilds this subtree, false when rejected.
        bool SetParentMaterial(CMaterialInterface* NewParent);

        // Idempotent, and needed beyond PostLoad because an instance built at runtime never registers there.
        void EnsureRegisteredWithParent();

        // Resets the block to the parent's and re-applies every enabled override.
        void RebuildUniformsFromOverrides();

        // The root's parameter table, which every level shares since an instance only overrides values.
        const TVector<FMaterialParameter>& GetMaterialParams() const;

        const FMaterialParameterOverride* FindOverride(const FName& Name) const;

        // Whether an override exists for Name and is enabled.
        bool IsOverrideEnabled(const FName& Name) const;

        // Enabling seeds the override from the parent's current value, and disabling keeps it for later.
        void SetOverrideEnabled(const FName& Name, bool bEnabled);

        // Drops the override along with its stored value.
        void RemoveOverride(const FName& Name);

        // Whether an enabled texture override supplies slot Index. A slot the parent binds with no parameter can only be inherited.
        bool IsTextureSlotOverridden(uint32 Index) const;

        // Bit i set when an enabled texture override supplies slot i. Hoist it out of loops over slots.
        NODISCARD uint32 GetOverriddenTextureMask() const;

        // Flips a named switch onto another permutation, false when the root declares no such switch.
        bool SetStaticSwitchValue(const FName& Name, bool bValue);

        // This level's override, else the nearest ancestor's, else the root's authored default.
        NODISCARD bool GetStaticSwitchValue(const FName& Name) const;

        NODISCARD bool HasStaticSwitchOverride(const FName& Name) const;

        // Drops the override so this level inherits the switch again.
        void RemoveStaticSwitchOverride(const FName& Name);

        // Switch values overridden anywhere up this chain, written root first so a nearer level wins.
        void GatherStaticSwitchValues(THashMap<FName, bool>& OutValues, uint32 Depth = 0) const;

        // The immediate parent, a base material or another instance. Assign through SetParentMaterial.
        PROPERTY(ReadOnly, Category = "Material")
        TStrongObjectPtr<CMaterialInterface> Material;

        PROPERTY(Editable, Category = "Material|Shading")
        bool bOverrideShadingModel = false;

        PROPERTY(Editable, Category = "Material|Shading")
        EMaterialShadingModel ShadingModelOverride = EMaterialShadingModel::Lit;

        PROPERTY()
        TVector<FMaterialParameterOverride> Overrides;

        // Switches this level diverges on, inherited down the chain like a parameter override.
        PROPERTY()
        TVector<FMaterialStaticSwitchOverride> StaticSwitchOverrides;

    private:

        // Stores Apply's value as an enabled override of Name, writes it into this block and pushes it down the subtree.
        template <typename TApply>
        bool SetOverride(EMaterialParameterType Type, const FName& Name, TApply&& Apply);

        FMaterialParameterOverride& FindOrAddOverride(const FName& Name, EMaterialParameterType Type);

        // The details panel writes the parent field directly, so the registration that SetParentMaterial does is redone.
        void AdoptEditedParent();

        // Requests the new permutation and invalidates this level's resolves, then does the same down the subtree.
        void OnStaticSwitchesChanged();
        void PropagateStaticSwitchChange(uint32 Depth = 0);

        // Editor only. Asks the root to build the permutation this level selects, if it has not.
        void RequestStaticSwitchPermutation();
    };
}
