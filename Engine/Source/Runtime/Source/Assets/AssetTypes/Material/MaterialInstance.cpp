#include "RuntimePCH.h"
#include "Memory/MemoryTracking.h"
#include "MaterialInstance.h"
#include "Material.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Core/Engine/Engine.h"
#include "Core/Object/Cast.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Renderer/RenderManager.h"
#include "World/Scene/RenderScene/MeshResolveCache.h"
#include "Log/Log.h"
#include "Containers/HashTable.h"
#include "Core/Threading/Thread.h"

namespace Lumina
{
    namespace
    {
        const char* ParameterKindName(EMaterialParameterType Type)
        {
            switch (Type)
            {
            case EMaterialParameterType::Scalar:  return "scalar";
            case EMaterialParameterType::Vector:  return "vector";
            case EMaterialParameterType::Texture: return "texture";
            }
            return "unknown";
        }

        // Once per name, since a script setting a missing parameter every frame would flood the log.
        void WarnMissingParameterOnce(const char* Kind, const FName& Name)
        {
            static FMutex          Mutex;
            static THashSet<FName> Reported;
            {
                FScopeLock Lock(Mutex);
                if (!Reported.insert(Name).second)
                {
                    return;
                }
            }
            LOG_WARN("Material instance: no parent {} parameter named '{}'.", Kind, Name);
        }

        // Writes an override into a block without uploading, for the full rebuild that uploads once at the end.
        void ApplyOverride(const FMaterialParameter& Param, const FMaterialParameterOverride& Override, FMaterialUniforms& Uniforms)
        {
            switch (Override.Type)
            {
            case EMaterialParameterType::Scalar:
                if (Param.Index < MAX_SCALARS)
                {
                    Uniforms.Scalars[Param.Index] = Override.Scalar;
                }
                break;

            case EMaterialParameterType::Vector:
                if (Param.Index < MAX_VECTORS)
                {
                    Uniforms.Vectors[Param.Index] = Override.Vector;
                }
                break;

            case EMaterialParameterType::Texture:
                if (Param.Index < MAX_TEXTURES && Override.Texture && Override.Texture->GetResourceID() >= 0)
                {
                    Uniforms.Textures[Param.Index] = (uint32)Override.Texture->GetResourceID();
                }
                break;
            }
        }
    }

    CMaterialInstance* CMaterialInstance::CreateDynamic(CMaterialInterface* Parent)
    {
        if (Parent == nullptr)
        {
            LOG_ERROR("CreateDynamic: no parent material.");
            return nullptr;
        }

        CMaterialInstance* Instance = NewObject<CMaterialInstance>(nullptr, "DynamicMaterialInstance");
        if (Instance == nullptr)
        {
            return nullptr;
        }

        // Parented before the slot is taken, so the slot starts from a resolved block rather than zeros.
        if (!Instance->SetParentMaterial(Parent))
        {
            Instance->ConditionalBeginDestroy();
            return nullptr;
        }

        Instance->AcquireOrUploadMaterialSlot();
        Instance->SetReadyForRender(true);
        return Instance;
    }

    void CMaterialInstance::PostLoad()
    {
        LUMINA_MEMORY_SCOPE("Materials");
        if (!Material)
        {
            return;
        }

        // Registered before the parent's PostLoad, so its PropagateToChildren reaches this level.
        Material->RegisterChild(this);

        // The loader's own guard, so a parent that already ran, or is stale for good, never runs twice.
        if (Material->HasAnyFlag(OF_NeedsPostLoad))
        {
            Material->ClearFlags(OF_NeedsPostLoad);
            Material->PostLoad();
        }
        else
        {
            RebuildUniformsFromOverrides();
        }

        AcquireOrUploadMaterialSlot();
        SetReadyForRender(true);

        // A loaded instance may name a permutation the root has never built, and nothing else asks for it.
        RequestStaticSwitchPermutation();

        // Surfaces that fell back to the default still record this as a dependency, so they wake here.
        FMeshResolveCache::InvalidateDependency(this);
    }

    void CMaterialInstance::OnDestroy()
    {
        CMaterialInterface::OnDestroy();

        // Resolves are keyed partly on this pointer, so they go before it can be recycled.
        FMeshResolveCache::InvalidateDependency(this);

        if (Material)
        {
            Material->UnregisterChild(this);
        }
        ReleaseMaterialSlot();
    }

    void CMaterialInstance::PostPropertyChange(FProperty* ChangedProperty)
    {
        Super::PostPropertyChange(ChangedProperty);

        if (ChangedProperty != nullptr && ChangedProperty->GetPropertyName() == FName("Material"))
        {
            AdoptEditedParent();
            return;
        }

        // The shading model override has to be re-stamped and uploaded to show.
        RefreshSubtree();
    }

    void CMaterialInstance::OnReferencesReplaced()
    {
        // A nulled override or parent stays baked in the block until it is rebuilt, and surfaces keep the old resolve.
        RefreshSubtree();
        FMeshResolveCache::InvalidateDependency(this);
    }

    CMaterial* CMaterialInstance::GetMaterial() const
    {
        // Bounded, so a cycle that slipped past SetParentMaterial cannot hang.
        CMaterialInterface* Parent = Material.Get();
        for (uint32 Depth = 0; Parent != nullptr && Depth < MaxChainDepth; ++Depth)
        {
            if (CMaterial* Root = Cast<CMaterial>(Parent))
            {
                return Root;
            }
            Parent = Parent->GetParentMaterial();
        }
        return nullptr;
    }

    bool CMaterialInstance::SetParentMaterial(CMaterialInterface* NewParent)
    {
        if (NewParent == Material.Get())
        {
            return true;
        }

        if (NewParent == this)
        {
            LOG_ERROR("Material instance '{}' cannot be its own parent.", GetName());
            return false;
        }

        uint32 Depth = 0;
        for (CMaterialInterface* Ancestor = NewParent; Ancestor != nullptr; Ancestor = Ancestor->GetParentMaterial())
        {
            if (Ancestor == this)
            {
                LOG_ERROR("Reparenting '{}' to '{}' would form a cycle.", GetName(), NewParent->GetName());
                return false;
            }
            if (++Depth >= MaxChainDepth)
            {
                LOG_ERROR("Reparenting '{}' to '{}' exceeds the {} level instance chain limit.", GetName(), NewParent->GetName(), MaxChainDepth);
                return false;
            }
        }

        if (CMaterialInterface* OldParent = Material.Get())
        {
            OldParent->UnregisterChild(this);
        }

        Material = NewParent;
        if (NewParent != nullptr)
        {
            NewParent->RegisterChild(this);
        }

        RefreshSubtree();

        // A new parent can mean a new root, so the shaders and blend mode a surface resolved are stale.
        FMeshResolveCache::InvalidateDependency(this);
        return true;
    }

    void CMaterialInstance::AdoptEditedParent()
    {
        CMaterialInterface* EditedParent = Material.Get();
        Material = nullptr;
        if (!SetParentMaterial(EditedParent) || Material == nullptr)
        {
            SetReadyForRender(false);
            return;
        }

        AcquireOrUploadMaterialSlot();
        SetReadyForRender(true);
        RequestStaticSwitchPermutation();
    }

    void CMaterialInstance::EnsureRegisteredWithParent()
    {
        if (Material)
        {
            Material->RegisterChild(this);
        }
    }

    bool CMaterialInstance::IsReadyForRender() const
    {
        return CMaterialInterface::IsReadyForRender() && Material != nullptr && Material->IsReadyForRender();
    }

    EMaterialShadingModel CMaterialInstance::GetShadingModel() const
    {
        // The parent, not the root, since an instance between them may override it too.
        if (bOverrideShadingModel)
        {
            return ShadingModelOverride;
        }
        return Material ? Material->GetShadingModel() : EMaterialShadingModel::Lit;
    }

    void CMaterialInstance::RebuildUniformsFromOverrides()
    {
        CMaterial* Root = GetMaterial();
        if (!Material || Root == nullptr)
        {
            return;
        }

        EnsureRegisteredWithParent();

        // The immediate parent's resolved block, which composes a chain only because propagation runs top down.
        MaterialUniforms = *Material->GetMaterialUniforms();

        // Overrides are never pruned here, so a recompile that drops a parameter cannot destroy its value.
        const uint32 OverriddenMask = GetOverriddenTextureMask();
        for (const FMaterialParameter& Param : Root->Parameters)
        {
            if (Param.Type == EMaterialParameterType::Texture && Param.Index < MAX_TEXTURES && (OverriddenMask & (1u << Param.Index)) == 0)
            {
                MaterialUniforms.Textures[Param.Index] = Material->GetResolvedTextureSlot(Param.Index);
            }
        }

        for (const FMaterialParameterOverride& Override : Overrides)
        {
            FMaterialParameter Param;
            if (Override.bEnabled && Root->GetParameterValue(Override.Type, Override.ParameterName, Param))
            {
                ApplyOverride(Param, Override, MaterialUniforms);
            }
        }

        // Stamped last so nothing above can undo an instance's shading model.
        MaterialUniforms.Flags &= ~(kMaterialShadingModelMask << kMaterialShadingModelShift);
        MaterialUniforms.Flags |= ((uint32)GetShadingModel() & kMaterialShadingModelMask) << kMaterialShadingModelShift;
    }

    void CMaterialInstance::RefreshFromParent()
    {
        RebuildUniformsFromOverrides();
        UploadMaterialUniforms();

        // The parent may have just recompiled, which drops every permutation and renumbers the bits.
        RequestStaticSwitchPermutation();
    }

    bool CMaterialInstance::InheritParameterValue(EMaterialParameterType Type, const FName& Name, uint16 Index)
    {
        if (!Material)
        {
            return false;
        }

        const bool bOverridden = Algo::AnyOf(Overrides, [&](const FMaterialParameterOverride& Override)
        {
            return Override.Type == Type && Override.bEnabled && Override.ParameterName == Name;
        });
        if (bOverridden)
        {
            return false;
        }

        const FMaterialUniforms& Inherited = *Material->GetMaterialUniforms();
        switch (Type)
        {
        case EMaterialParameterType::Scalar:
            if (Index < MAX_SCALARS)
            {
                WriteScalarSlot(Index, Inherited.Scalars[Index]);
                return true;
            }
            break;

        case EMaterialParameterType::Vector:
            if (Index < MAX_VECTORS)
            {
                WriteVectorSlot(Index, Inherited.Vectors[Index]);
                return true;
            }
            break;

        case EMaterialParameterType::Texture:
            if (Index < MAX_TEXTURES)
            {
                WriteTextureSlot(Index, Inherited.Textures[Index]);
                return true;
            }
            break;
        }
        return false;
    }

    template <typename TApply>
    bool CMaterialInstance::SetOverride(EMaterialParameterType Type, const FName& Name, TApply&& Apply)
    {
        if (!Material)
        {
            return false;
        }

        EnsureRegisteredWithParent();

        FMaterialParameter Param;
        if (!GetParameterValue(Type, Name, Param))
        {
            WarnMissingParameterOnce(ParameterKindName(Type), Name);
            return false;
        }

        FMaterialParameterOverride& Override = FindOrAddOverride(Name, Type);
        Override.bEnabled = true;
        Apply(Override, Param.Index);

        PropagateParameterToChildren(Type, Name, Param.Index);
        return true;
    }

    bool CMaterialInstance::SetScalarValue(const FName& Name, const float Value)
    {
        return SetOverride(EMaterialParameterType::Scalar, Name, [&](FMaterialParameterOverride& Override, uint16 Index)
        {
            Override.Scalar = Value;
            WriteScalarSlot(Index, Value);
        });
    }

    bool CMaterialInstance::SetVectorValue(const FName& Name, const FVector4& Value)
    {
        return SetOverride(EMaterialParameterType::Vector, Name, [&](FMaterialParameterOverride& Override, uint16 Index)
        {
            Override.Vector = Value;
            WriteVectorSlot(Index, Value);
        });
    }

    bool CMaterialInstance::SetTextureValue(const FName& Name, CTexture* TextureValue)
    {
        return SetOverride(EMaterialParameterType::Texture, Name, [&](FMaterialParameterOverride& Override, uint16 Index)
        {
            Override.Texture = TextureValue;

            // A texture not resident yet shows the parent's until RequestTexturesResolved rebuilds the block.
            const int32 ResourceID = TextureValue != nullptr ? TextureValue->GetResourceID() : -1;
            WriteTextureSlot(Index, ResourceID >= 0 ? (uint32)ResourceID : Material->GetResolvedTextureSlot(Index));
        });
    }

    bool CMaterialInstance::GetParameterValue(EMaterialParameterType Type, const FName& Name, FMaterialParameter& Param)
    {
        // Straight to the root, the only level that declares parameters.
        Param = {};
        CMaterial* Root = GetMaterial();
        return Root != nullptr && Root->GetParameterValue(Type, Name, Param);
    }

    const TVector<FMaterialParameter>& CMaterialInstance::GetMaterialParams() const
    {
        static const TVector<FMaterialParameter> Empty;
        const CMaterial* Root = GetMaterial();
        return Root != nullptr ? Root->Parameters : Empty;
    }

    FMaterialParameterOverride& CMaterialInstance::FindOrAddOverride(const FName& Name, EMaterialParameterType Type)
    {
        auto It = Algo::FindIf(Overrides, [&](const FMaterialParameterOverride& Override) { return Override.ParameterName == Name && Override.Type == Type; });
        if (It != Overrides.end())
        {
            return *It;
        }

        FMaterialParameterOverride& Added = Overrides.emplace_back();
        Added.ParameterName = Name;
        Added.Type = Type;
        return Added;
    }

    const FMaterialParameterOverride* CMaterialInstance::FindOverride(const FName& Name) const
    {
        auto It = Algo::FindIf(Overrides, [&Name](const FMaterialParameterOverride& Override) { return Override.ParameterName == Name; });
        return It != Overrides.end() ? &*It : nullptr;
    }

    bool CMaterialInstance::IsOverrideEnabled(const FName& Name) const
    {
        const FMaterialParameterOverride* Override = FindOverride(Name);
        return Override != nullptr && Override->bEnabled;
    }

    void CMaterialInstance::SetOverrideEnabled(const FName& Name, bool bEnabled)
    {
        if (!Material)
        {
            return;
        }

        // An existing override keeps its value, so re-enabling restores the user's edit rather than resetting it.
        auto It = Algo::FindIf(Overrides, [&Name](const FMaterialParameterOverride& Override) { return Override.ParameterName == Name; });
        if (It != Overrides.end())
        {
            if (It->bEnabled != bEnabled)
            {
                It->bEnabled = bEnabled;
                RefreshSubtree();
            }
            return;
        }

        if (!bEnabled)
        {
            return;
        }

        FMaterialParameter Param;
        const bool bFound = GetParameterValue(EMaterialParameterType::Scalar,  Name, Param)
                         || GetParameterValue(EMaterialParameterType::Vector,  Name, Param)
                         || GetParameterValue(EMaterialParameterType::Texture, Name, Param);
        if (!bFound)
        {
            LOG_ERROR("Cannot override unknown parameter '{}'", Name);
            return;
        }

        // Seeded from the immediate parent, so the override starts where the instance already renders.
        const FMaterialUniforms& Inherited = *Material->GetMaterialUniforms();
        FMaterialParameterOverride& Override = FindOrAddOverride(Name, Param.Type);
        Override.bEnabled = true;
        switch (Param.Type)
        {
        case EMaterialParameterType::Scalar:
            Override.Scalar = Param.Index < MAX_SCALARS ? Inherited.Scalars[Param.Index] : 0.0f;
            break;
        case EMaterialParameterType::Vector:
            Override.Vector = Param.Index < MAX_VECTORS ? Inherited.Vectors[Param.Index] : FVector4(0.0f);
            break;
        case EMaterialParameterType::Texture:
            Override.Texture = Material->GetTextureParameterTexture(Name, Param.Index);
            break;
        }

        RefreshSubtree();
    }

    void CMaterialInstance::RemoveOverride(const FName& Name)
    {
        auto NewEnd = Algo::RemoveIf(Overrides, [&Name](const FMaterialParameterOverride& Override) { return Override.ParameterName == Name; });
        if (NewEnd == Overrides.end())
        {
            return;
        }
        Overrides.erase(NewEnd, Overrides.end());

        // A texture restore means resolving the parent default this instance had been skipping.
        RefreshSubtree();
    }

    uint32 CMaterialInstance::GetOverriddenTextureMask() const
    {
        CMaterial* Root = GetMaterial();
        if (Root == nullptr)
        {
            return 0;
        }

        uint32 Mask = 0;
        for (const FMaterialParameterOverride& Override : Overrides)
        {
            FMaterialParameter Param;
            if (Override.Type == EMaterialParameterType::Texture && Override.bEnabled && Override.Texture != nullptr
                && Root->GetParameterValue(EMaterialParameterType::Texture, Override.ParameterName, Param) && Param.Index < MAX_TEXTURES)
            {
                Mask |= (1u << Param.Index);
            }
        }
        return Mask;
    }

    bool CMaterialInstance::IsTextureSlotOverridden(uint32 Index) const
    {
        return Index < MAX_TEXTURES && (GetOverriddenTextureMask() & (1u << Index)) != 0;
    }

    uint32 CMaterialInstance::GetResolvedTextureSlot(uint32 Index)
    {
        // This level's block is already resolved, so a child inherits from it without touching the root.
        return Index < MAX_TEXTURES ? MaterialUniforms.Textures[Index] : RHI::Textures::DefaultResourceID();
    }

    CTexture* CMaterialInstance::GetTextureParameterTexture(const FName& Name, uint32 Index)
    {
        for (const FMaterialParameterOverride& Override : Overrides)
        {
            if (Override.Type == EMaterialParameterType::Texture && Override.bEnabled && Override.ParameterName == Name && Override.Texture != nullptr)
            {
                return Override.Texture.Get();
            }
        }
        return Material ? Material->GetTextureParameterTexture(Name, Index) : nullptr;
    }

    void CMaterialInstance::RefreshInheritedTextureSlots()
    {
        CMaterial* Root = GetMaterial();
        if (!Material || Root == nullptr)
        {
            return;
        }

        // Narrower than RefreshFromParent, with no parameter rebuild and no synchronous resolve, so the async load completion can call it.
        const uint32 OverriddenMask = GetOverriddenTextureMask();
        const FMaterialUniforms& Inherited = *Material->GetMaterialUniforms();

        // The slot count comes from the root, which declares the table, and the values from the parent.
        uint32 FirstChanged = MAX_TEXTURES;
        uint32 LastChanged  = 0;
        const uint32 NumSlots = (uint32)Math::Min<size_t>(Root->Textures.size(), MAX_TEXTURES);
        for (uint32 i = 0; i < NumSlots; ++i)
        {
            if ((OverriddenMask & (1u << i)) == 0 && MaterialUniforms.Textures[i] != Inherited.Textures[i])
            {
                MaterialUniforms.Textures[i] = Inherited.Textures[i];
                FirstChanged = Math::Min(FirstChanged, i);
                LastChanged  = i;
            }
        }

        if (FirstChanged <= LastChanged)
        {
            UploadUniformField(TextureFieldOffset(FirstChanged), &MaterialUniforms.Textures[FirstChanged], (LastChanged - FirstChanged + 1) * (uint32)sizeof(uint32));
        }
    }

    bool CMaterialInstance::RefreshTextureBindings(const CTexture* ChangedTexture)
    {
        // Inherited textures come from the root, which is why roots refresh before instances.
        const bool bReferences = ChangedTexture == nullptr
            || Algo::AnyOf(Overrides, [ChangedTexture](const FMaterialParameterOverride& Override)
               {
                   return Override.Type == EMaterialParameterType::Texture && Override.Texture.Get() == ChangedTexture;
               })
            || (GetMaterial() != nullptr && GetMaterial()->ReferencesTexture(ChangedTexture));

        if (!bReferences)
        {
            return false;
        }

        RefreshSubtree();
        return true;
    }

    bool CMaterialInstance::RequestTexturesResolved()
    {
        if (!Material)
        {
            return true;
        }

        // The parent owns every slot not overridden here, and asking it first keeps the rebuild below non-blocking.
        if (!Material->RequestTexturesResolved())
        {
            return false;
        }

        // Loaded is not resident, and losing the race with the texture's PostLoad would bake the placeholder.
        for (const FMaterialParameterOverride& Override : Overrides)
        {
            FMaterialParameter Param;
            const bool bBinds = Override.Type == EMaterialParameterType::Texture && Override.bEnabled
                             && GetParameterValue(EMaterialParameterType::Texture, Override.ParameterName, Param);
            if (bBinds && Override.Texture != nullptr && Override.Texture->GetResourceID() < 0)
            {
                return false;
            }
        }

        CMaterial* Root = GetMaterial();
        if (Root == nullptr)
        {
            return true;
        }

        // Rebuilt when an inherited slot lags the parent, or an overridden slot still holds the placeholder.
        const uint32 OverriddenMask = GetOverriddenTextureMask();
        const FMaterialUniforms& Inherited = *Material->GetMaterialUniforms();
        const uint32 Placeholder = RHI::Textures::DefaultResourceID();
        const uint32 NumSlots = (uint32)Math::Min<size_t>(Root->Textures.size(), MAX_TEXTURES);

        bool bNeedsRebuild = false;
        for (uint32 i = 0; i < MAX_TEXTURES && !bNeedsRebuild; ++i)
        {
            const bool bOverridden = (OverriddenMask & (1u << i)) != 0;
            bNeedsRebuild = bOverridden ? MaterialUniforms.Textures[i] == Placeholder
                                        : (i < NumSlots && MaterialUniforms.Textures[i] != Inherited.Textures[i]);
        }

        if (bNeedsRebuild)
        {
            RebuildUniformsFromOverrides();
            UploadMaterialUniforms();
        }
        return true;
    }

    uint64 CMaterialInstance::GetStaticSwitchKey() const
    {
        CMaterial* Root = GetMaterial();
        if (Root == nullptr || Root->StaticSwitches.empty())
        {
            return 0;
        }

        // The common chain overrides no switch at all, which skips the key build.
        THashMap<FName, bool> Values;
        GatherStaticSwitchValues(Values);
        return Values.empty() ? Root->GetDefaultStaticSwitchKey() : Root->MakeStaticSwitchKey(Values);
    }

    void CMaterialInstance::GatherStaticSwitchValues(THashMap<FName, bool>& OutValues, uint32 Depth) const
    {
        if (Depth >= MaxChainDepth)
        {
            return;
        }

        if (const CMaterialInstance* ParentInstance = Cast<CMaterialInstance>(Material.Get()))
        {
            ParentInstance->GatherStaticSwitchValues(OutValues, Depth + 1);
        }
        for (const FMaterialStaticSwitchOverride& Override : StaticSwitchOverrides)
        {
            OutValues[Override.ParameterName] = Override.bValue;
        }
    }

    bool CMaterialInstance::HasStaticSwitchOverride(const FName& Name) const
    {
        return Algo::AnyOf(StaticSwitchOverrides, [&Name](const FMaterialStaticSwitchOverride& Override) { return Override.ParameterName == Name; });
    }

    bool CMaterialInstance::GetStaticSwitchValue(const FName& Name) const
    {
        auto It = Algo::FindIf(StaticSwitchOverrides, [&Name](const FMaterialStaticSwitchOverride& Override) { return Override.ParameterName == Name; });
        if (It != StaticSwitchOverrides.end())
        {
            return It->bValue;
        }

        if (const CMaterialInstance* ParentInstance = Cast<CMaterialInstance>(Material.Get()))
        {
            return ParentInstance->GetStaticSwitchValue(Name);
        }

        if (const CMaterial* Root = GetMaterial())
        {
            auto Switch = Algo::FindIf(Root->StaticSwitches, [&Name](const FMaterialStaticSwitch& S) { return S.ParameterName == Name; });
            return Switch != Root->StaticSwitches.end() && Switch->bDefaultValue;
        }
        return false;
    }

    bool CMaterialInstance::SetStaticSwitchValue(const FName& Name, bool bValue)
    {
        CMaterial* Root = GetMaterial();
        if (Root == nullptr || Root->FindStaticSwitchBit(Name) == Constants::kIndexNone)
        {
            WarnMissingParameterOnce("static switch", Name);
            return false;
        }

        auto It = Algo::FindIf(StaticSwitchOverrides, [&Name](const FMaterialStaticSwitchOverride& Override) { return Override.ParameterName == Name; });
        if (It != StaticSwitchOverrides.end())
        {
            if (It->bValue == bValue)
            {
                return true;
            }
            It->bValue = bValue;
        }
        else
        {
            FMaterialStaticSwitchOverride& Added = StaticSwitchOverrides.emplace_back();
            Added.ParameterName = Name;
            Added.bValue        = bValue;
        }

        OnStaticSwitchesChanged();
        return true;
    }

    void CMaterialInstance::RemoveStaticSwitchOverride(const FName& Name)
    {
        auto NewEnd = Algo::RemoveIf(StaticSwitchOverrides, [&Name](const FMaterialStaticSwitchOverride& Override) { return Override.ParameterName == Name; });
        if (NewEnd == StaticSwitchOverrides.end())
        {
            return;
        }
        StaticSwitchOverrides.erase(NewEnd, StaticSwitchOverrides.end());
        OnStaticSwitchesChanged();
    }

    void CMaterialInstance::OnStaticSwitchesChanged()
    {
        RequestStaticSwitchPermutation();

        // A switch change swaps the shader set every surface below here cached by handle.
        FMeshResolveCache::InvalidateDependency(this);
        PropagateStaticSwitchChange();
    }

    void CMaterialInstance::PropagateStaticSwitchChange(uint32 Depth)
    {
        if (Depth >= MaxChainDepth)
        {
            return;
        }

        for (CMaterialInterface* Child : SnapshotChildren())
        {
            CMaterialInstance* ChildInstance = Cast<CMaterialInstance>(Child);
            if (ChildInstance != nullptr && ChildInstance->GetParentMaterial() == this)
            {
                ChildInstance->RequestStaticSwitchPermutation();
                FMeshResolveCache::InvalidateDependency(ChildInstance);
                ChildInstance->PropagateStaticSwitchChange(Depth + 1);
            }
        }
    }

    void CMaterialInstance::RequestStaticSwitchPermutation()
    {
#if USING(WITH_EDITOR)
        CMaterial* Root = GetMaterial();
        if (Root != nullptr && !Root->StaticSwitches.empty())
        {
            CMaterial::RequestPermutation(Root, GetStaticSwitchKey());
        }
#endif
    }
}
