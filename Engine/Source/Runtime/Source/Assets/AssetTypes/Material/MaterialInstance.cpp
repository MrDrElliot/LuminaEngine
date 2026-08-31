#include "RuntimePCH.h"
#include "Memory/MemoryTracking.h"
#include "MaterialInstance.h"
#include "Material.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Core/Engine/Engine.h"
#include "Core/Object/Cast.h"
#include "Renderer/RenderManager.h"
#include "World/Scene/RenderScene/MeshResolveCache.h"
#include "Log/Log.h"
#include "Containers/HashTable.h"
#include "Core/Threading/Thread.h"

namespace Lumina
{
    namespace
    {
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
    }

    CMaterialInstance::CMaterialInstance()
    {
        Memory::Memzero(&MaterialUniforms, sizeof(FMaterialUniforms));
    }

    CMaterial* CMaterialInstance::GetMaterial() const
    {
        // Bounded rather than unbounded, so a cycle that slipped past SetParentMaterial cannot hang.
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
                LOG_ERROR("Reparenting '{}' to '{}' exceeds the {} level instance chain limit.",
                    GetName(), NewParent->GetName(), MaxChainDepth);
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

        // A new parent can mean a new ROOT, so the shaders and blend mode a surface resolved are stale.
        FMeshResolveCache::InvalidateDependency(this);
        return true;
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

        // Before the slot is taken, so AddMaterial writes an already-resolved block rather than zeroes.
        if (!Instance->SetParentMaterial(Parent))
        {
            Instance->ConditionalBeginDestroy();
            return nullptr;
        }

        if (FRenderManager* RenderManager = TryRender())
        {
            RenderManager->GetMaterialManager().AddMaterial(Instance);
        }

        Instance->SetReadyForRender(true);
        return Instance;
    }

    static void ApplyOverride(CMaterial* Root, const FMaterialParameterOverride& Override, FMaterialUniforms& Uniforms)
    {
        // Through the root's name map rather than a scan, since a material carries up to 72 parameters.
        FMaterialParameter Param;
        if (!Root->GetParameterValue(Override.Type, Override.ParameterName, Param))
        {
            return;
        }

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

    void CMaterialInstance::EnsureRegisteredWithParent()
    {
        if (Material)
        {
            Material->RegisterChild(this);
        }
    }

    void CMaterialInstance::RebuildUniformsFromOverrides()
    {
        CMaterial* Root = GetMaterial();
        if (!Material || Root == nullptr)
        {
            return;
        }

        // An override flipping changes which texture a slot samples, so it changes what is owed the streamer.

        EnsureRegisteredWithParent();

        // The IMMEDIATE parent's resolved block, which composes a chain only because propagation is top-down.
        MaterialUniforms = *Material->GetMaterialUniforms();

        // Overrides are never pruned here, so a recompile that drops a parameter cannot destroy its value.

        // Runs BEFORE the override loop, which has the last word, so an override never resolves the parent default.
        const uint32 OverriddenMask = GetOverriddenTextureMask();
        for (const FMaterialParameter& Param : Root->Parameters)
        {
            if (Param.Type != EMaterialParameterType::Texture || Param.Index >= MAX_TEXTURES)
            {
                continue;
            }

            if ((OverriddenMask & (1u << Param.Index)) != 0)
            {
                continue;   // ApplyOverride supplies this slot; the inherited value is dead weight
            }

            MaterialUniforms.Textures[Param.Index] = Material->GetResolvedTextureSlot(Param.Index);
        }

        for (const FMaterialParameterOverride& Override : Overrides)
        {
            // Disabled overrides keep their stored value but are not applied; the parent value shows through.
            if (Override.bEnabled)
            {
                ApplyOverride(Root, Override, MaterialUniforms);
            }
        }

        // Re-stamped last so an instance override lands and nothing above can undo it.
        const uint32 ModelBits =
            ((uint32)GetShadingModel() & kMaterialShadingModelMask) << kMaterialShadingModelShift;

        MaterialUniforms.Flags &= ~(kMaterialShadingModelMask << kMaterialShadingModelShift);
        MaterialUniforms.Flags |= ModelBits;
    }

    void CMaterialInstance::RefreshFromParent()
    {
        RebuildUniformsFromOverrides();
        UpdateMaterialUniforms();

        // The parent may have just recompiled, which drops every permutation and renumbers the bits.
        RequestStaticSwitchPermutation();

        // No invalidate, since a resolve stamps the slot INDEX and a value change never moves it.
    }

    void CMaterialInstance::GatherStaticSwitchValues(THashMap<FName, bool>& OutValues, uint32 Depth) const
    {
        if (Depth >= MaxChainDepth)
        {
            return;
        }

        // Parent first, so this level's overrides land on top of everything it inherits.
        if (const CMaterialInstance* ParentInstance = Cast<CMaterialInstance>(Material.Get()))
        {
            ParentInstance->GatherStaticSwitchValues(OutValues, Depth + 1);
        }

        for (const FMaterialStaticSwitchOverride& Override : StaticSwitchOverrides)
        {
            OutValues[Override.ParameterName] = Override.bValue;
        }
    }

    uint64 CMaterialInstance::GetStaticSwitchKey() const
    {
        CMaterial* Root = GetMaterial();
        if (Root == nullptr || Root->StaticSwitches.empty())
        {
            return 0;
        }

        // Cheap out before the map allocation for the common chain that overrides no switch at all.
        THashMap<FName, bool> Values;
        GatherStaticSwitchValues(Values);
        if (Values.empty())
        {
            return Root->GetDefaultStaticSwitchKey();
        }

        return Root->MakeStaticSwitchKey(Values);
    }

    bool CMaterialInstance::HasStaticSwitchOverride(const FName& Name) const
    {
        return Algo::AnyOf(StaticSwitchOverrides,
            [&Name](const FMaterialStaticSwitchOverride& O) { return O.ParameterName == Name; });
    }

    bool CMaterialInstance::GetStaticSwitchValue(const FName& Name) const
    {
        for (const FMaterialStaticSwitchOverride& Override : StaticSwitchOverrides)
        {
            if (Override.ParameterName == Name)
            {
                return Override.bValue;
            }
        }

        if (const CMaterialInstance* ParentInstance = Cast<CMaterialInstance>(Material.Get()))
        {
            return ParentInstance->GetStaticSwitchValue(Name);
        }

        if (CMaterial* Root = GetMaterial())
        {
            for (const FMaterialStaticSwitch& Switch : Root->StaticSwitches)
            {
                if (Switch.ParameterName == Name)
                {
                    return Switch.bDefaultValue;
                }
            }
        }

        return false;
    }

    bool CMaterialInstance::SetStaticSwitchValue(const FName& Name, bool bValue)
    {
        CMaterial* Root = GetMaterial();
        if (Root == nullptr || Root->FindStaticSwitchBit(Name) == INDEX_NONE)
        {
            WarnMissingParameterOnce("static switch", Name);
            return false;
        }

        for (FMaterialStaticSwitchOverride& Override : StaticSwitchOverrides)
        {
            if (Override.ParameterName != Name)
            {
                continue;
            }
            if (Override.bValue == bValue)
            {
                return true;
            }
            Override.bValue = bValue;
            OnStaticSwitchesChanged();
            return true;
        }

        FMaterialStaticSwitchOverride Override;
        Override.ParameterName = Name;
        Override.bValue        = bValue;
        StaticSwitchOverrides.push_back(Override);
        OnStaticSwitchesChanged();
        return true;
    }

    void CMaterialInstance::RemoveStaticSwitchOverride(const FName& Name)
    {
        auto NewEnd = Algo::RemoveIf(StaticSwitchOverrides, [Name](const FMaterialStaticSwitchOverride& O)
        {
            return O.ParameterName == Name;
        });

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

        // A switch change swaps the shader set every resolved surface below here caches by handle.
        FMeshResolveCache::InvalidateDependency(this);
        PropagateStaticSwitchChange();
    }

    void CMaterialInstance::PropagateStaticSwitchChange(uint32 Depth)
    {
        if (Depth >= MaxChainDepth)
        {
            return;
        }

        // Snapshotted rather than held, the same way every other propagation down this chain works.
        TVector<CMaterialInterface*> Snapshot;
        {
            FScopeLock Lock(ChildrenMutex);
            Snapshot = Children;
        }

        for (CMaterialInterface* Child : Snapshot)
        {
            CMaterialInstance* ChildInstance = Cast<CMaterialInstance>(Child);
            if (ChildInstance == nullptr || ChildInstance->GetParentMaterial() != this)
            {
                continue;
            }

            ChildInstance->RequestStaticSwitchPermutation();
            FMeshResolveCache::InvalidateDependency(ChildInstance);
            ChildInstance->PropagateStaticSwitchChange(Depth + 1);
        }
    }

    void CMaterialInstance::RequestStaticSwitchPermutation()
    {
#if USING(WITH_EDITOR)
        CMaterial* Root = GetMaterial();
        if (Root == nullptr || Root->StaticSwitches.empty())
        {
            return;
        }
        CMaterial::RequestPermutation(Root, GetStaticSwitchKey());
#endif
    }

    void CMaterialInstance::PostPropertyChange(FProperty* ChangedProperty)
    {
        Super::PostPropertyChange(ChangedProperty);

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
            if (Override.Type != EMaterialParameterType::Texture || !Override.bEnabled || Override.Texture == nullptr)
            {
                continue;
            }

            FMaterialParameter Param;
            if (Root->GetParameterValue(EMaterialParameterType::Texture, Override.ParameterName, Param)
                && Param.Index < MAX_TEXTURES)
            {
                Mask |= (1u << Param.Index);
            }
        }

        return Mask;
    }

    uint32 CMaterialInstance::GetResolvedTextureSlot(uint32 Index)
    {
        if (Index >= MAX_TEXTURES)
        {
            return RHI::Textures::DefaultResourceID();
        }

        // This level's block is already resolved, so a child inherits from it without touching the root.
        return MaterialUniforms.Textures[Index];
    }

    CTexture* CMaterialInstance::GetTextureParameterTexture(const FName& Name, uint32 Index)
    {
        for (const FMaterialParameterOverride& Override : Overrides)
        {
            if (Override.Type == EMaterialParameterType::Texture && Override.bEnabled
                && Override.ParameterName == Name && Override.Texture != nullptr)
            {
                return Override.Texture.Get();
            }
        }

        return Material ? Material->GetTextureParameterTexture(Name, Index) : nullptr;
    }

    bool CMaterialInstance::IsTextureSlotOverridden(uint32 Index) const
    {
        // A slot no parameter names is plainly bound on the parent, and the mask has no bit for it.
        return Index < MAX_TEXTURES && (GetOverriddenTextureMask() & (1u << Index)) != 0;
    }

    void CMaterialInstance::RefreshInheritedTextureSlots()
    {
        CMaterial* Root = GetMaterial();
        if (!Material || Root == nullptr)
        {
            return;
        }

        const uint32 OverriddenMask = GetOverriddenTextureMask();

        uint32 FirstChanged = MAX_TEXTURES;
        uint32 LastChanged  = 0;

        // Slot COUNT comes from the root, which declares the texture table; the VALUES come from the parent.
        const FMaterialUniforms& Inherited = *Material->GetMaterialUniforms();

        const uint32 NumSlots = (uint32)Math::Min<size_t>(Root->Textures.size(), MAX_TEXTURES);
        for (uint32 i = 0; i < NumSlots; ++i)
        {
            if ((OverriddenMask & (1u << i)) != 0)
            {
                continue;
            }

            if (MaterialUniforms.Textures[i] != Inherited.Textures[i])
            {
                MaterialUniforms.Textures[i] = Inherited.Textures[i];
                FirstChanged = Math::Min(FirstChanged, i);
                LastChanged  = i;
            }
        }

        if (FirstChanged <= LastChanged)
        {
            UploadUniformField(TextureFieldOffset(FirstChanged), &MaterialUniforms.Textures[FirstChanged],
                (LastChanged - FirstChanged + 1) * (uint32)sizeof(uint32));
        }
    }

    static FMaterialParameterOverride& FindOrAddOverride(TVector<FMaterialParameterOverride>& Overrides, const FName& Name, EMaterialParameterType Type)
    {
        for (FMaterialParameterOverride& O : Overrides)
        {
            if (O.ParameterName == Name && O.Type == Type)
            {
                return O;
            }
        }

        FMaterialParameterOverride New;
        New.ParameterName = Name;
        New.Type = Type;
        Overrides.push_back(New);
        return Overrides.back();
    }

    bool CMaterialInstance::SetScalarValue(const FName& Name, const float Value)
    {
        if (!Material)
        {
            return false;
        }

        EnsureRegisteredWithParent();

        FMaterialParameter Param;
        if (!GetParameterValue(EMaterialParameterType::Scalar, Name, Param))
        {
            WarnMissingParameterOnce("scalar", Name);
            return false;
        }

        FMaterialParameterOverride& Override = FindOrAddOverride(Overrides, Name, EMaterialParameterType::Scalar);
        Override.Scalar = Value;
        Override.bEnabled = true;

        if (Param.Index < MAX_SCALARS)
        {
            MaterialUniforms.Scalars[Param.Index] = Value;
            UploadUniformField(ScalarFieldOffset(Param.Index), &MaterialUniforms.Scalars[Param.Index], sizeof(float));
        }

        PropagateParameterToChildren(EMaterialParameterType::Scalar, Name, Param.Index);
        return true;
    }

    bool CMaterialInstance::SetVectorValue(const FName& Name, const FVector4& Value)
    {
        if (!Material)
        {
            return false;
        }

        EnsureRegisteredWithParent();

        FMaterialParameter Param;
        if (!GetParameterValue(EMaterialParameterType::Vector, Name, Param))
        {
            WarnMissingParameterOnce("vector", Name);
            return false;
        }

        FMaterialParameterOverride& Override = FindOrAddOverride(Overrides, Name, EMaterialParameterType::Vector);
        Override.Vector = Value;
        Override.bEnabled = true;

        // A targeted 16-byte write rather than a rebuild of the whole block, as in SetScalarValue.
        if (Param.Index < MAX_VECTORS)
        {
            MaterialUniforms.Vectors[Param.Index] = Value;
            UploadUniformField(VectorFieldOffset(Param.Index), &MaterialUniforms.Vectors[Param.Index], sizeof(FVector4));
        }

        PropagateParameterToChildren(EMaterialParameterType::Vector, Name, Param.Index);
        return true;
    }

    bool CMaterialInstance::SetTextureValue(const FName& Name, CTexture* TextureValue)
    {
        if (!Material)
        {
            return false;
        }

        EnsureRegisteredWithParent();

        FMaterialParameter Param;
        if (!GetParameterValue(EMaterialParameterType::Texture, Name, Param))
        {
            WarnMissingParameterOnce("texture", Name);
            return false;
        }

        FMaterialParameterOverride& Override = FindOrAddOverride(Overrides, Name, EMaterialParameterType::Texture);
        Override.Texture = TextureValue;
        Override.bEnabled = true;

        // Writes the uniform slot directly rather than rebuilding, so it has to mark dirty itself.

        if (Param.Index < MAX_TEXTURES)
        {
            const int32 ResourceID = (TextureValue != nullptr) ? TextureValue->GetResourceID() : -1;
            MaterialUniforms.Textures[Param.Index] = (ResourceID >= 0)
                ? (uint32)ResourceID
                : Material->GetResolvedTextureSlot(Param.Index);

            UploadUniformField(TextureFieldOffset(Param.Index), &MaterialUniforms.Textures[Param.Index], sizeof(uint32));
        }

        PropagateParameterToChildren(EMaterialParameterType::Texture, Name, Param.Index);
        return true;
    }

    const TVector<FMaterialParameter>& CMaterialInstance::GetMaterialParams() const
    {
        static const TVector<FMaterialParameter> Empty;
        CMaterial* Root = GetMaterial();
        return Root ? Root->Parameters : Empty;
    }

    bool CMaterialInstance::GetParameterValue(EMaterialParameterType Type, const FName& Name, FMaterialParameter& Param)
    {
        Param = {};

        // Straight to the ROOT, the only level that declares parameters; no level between could differ.
        CMaterial* Root = GetMaterial();
        return Root != nullptr && Root->GetParameterValue(Type, Name, Param);
    }

    const FMaterialParameterOverride* CMaterialInstance::FindOverride(const FName& Name) const
    {
        for (const FMaterialParameterOverride& O : Overrides)
        {
            if (O.ParameterName == Name)
            {
                return &O;
            }
        }
        return nullptr;
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

        // The stored value is retained, so re-enabling restores the user's edits rather than resetting.
        for (FMaterialParameterOverride& O : Overrides)
        {
            if (O.ParameterName == Name)
            {
                if (O.bEnabled == bEnabled)
                {
                    return;
                }

                O.bEnabled = bEnabled;

                // Disabling has to restore the parent's value, which for a texture means resolving its default.
                RefreshSubtree();
                return;
            }
        }

        // Disabling a parameter that was never overridden has nothing to do.
        if (!bEnabled)
        {
            return;
        }

        // Seeded with the parent's current value, and from here it persists across toggles.
        FMaterialParameter Param;
        const bool bFound =
            GetParameterValue(EMaterialParameterType::Scalar,  Name, Param) ||
            GetParameterValue(EMaterialParameterType::Vector,  Name, Param) ||
            GetParameterValue(EMaterialParameterType::Texture, Name, Param);
        if (!bFound)
        {
            LOG_ERROR("Cannot override unknown parameter '{}'", Name);
            return;
        }

        // From the IMMEDIATE parent, so this starts where the instance already renders, not at the root.
        const FMaterialUniforms& Inherited = *Material->GetMaterialUniforms();

        FMaterialParameterOverride& Override = FindOrAddOverride(Overrides, Name, Param.Type);
        Override.bEnabled = true;
        switch (Param.Type)
        {
        case EMaterialParameterType::Scalar:
            Override.Scalar = (Param.Index < MAX_SCALARS) ? Inherited.Scalars[Param.Index] : 0.0f;
            break;
        case EMaterialParameterType::Vector:
            Override.Vector = (Param.Index < MAX_VECTORS) ? Inherited.Vectors[Param.Index] : FVector4(0.0f);
            break;
        case EMaterialParameterType::Texture:
            Override.Texture = Material->GetTextureParameterTexture(Name, Param.Index);
            break;
        }

        RefreshSubtree();
    }

    void CMaterialInstance::RemoveOverride(const FName& Name)
    {
        auto NewEnd = Algo::RemoveIf(Overrides, [Name](const FMaterialParameterOverride& O)
        {
            return O.ParameterName == Name;
        });

        // Nothing matched, so nothing this could have changed.
        if (NewEnd == Overrides.end())
        {
            return;
        }

        Overrides.erase(NewEnd, Overrides.end());

        // A texture restore means resolving the parent default this instance had been skipping.
        RefreshSubtree();
    }

    void CMaterialInstance::UpdateMaterialUniforms()
    {
        // A slot is only ever handed out by the material manager, so holding one implies a renderer.
        if (MaterialIndex != -1)
        {
            Render().GetMaterialManager().UpdateMaterialUniforms(&MaterialUniforms, (uint32)MaterialIndex);
        }
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
            if (Index >= MAX_SCALARS)
            {
                return false;
            }
            MaterialUniforms.Scalars[Index] = Inherited.Scalars[Index];
            UploadUniformField(ScalarFieldOffset(Index), &MaterialUniforms.Scalars[Index], sizeof(float));
            return true;

        case EMaterialParameterType::Vector:
            if (Index >= MAX_VECTORS)
            {
                return false;
            }
            MaterialUniforms.Vectors[Index] = Inherited.Vectors[Index];
            UploadUniformField(VectorFieldOffset(Index), &MaterialUniforms.Vectors[Index], sizeof(FVector4));
            return true;

        case EMaterialParameterType::Texture:
            if (Index >= MAX_TEXTURES)
            {
                return false;
            }
            MaterialUniforms.Textures[Index] = Inherited.Textures[Index];
            UploadUniformField(TextureFieldOffset(Index), &MaterialUniforms.Textures[Index], sizeof(uint32));
            return true;
        }

        return false;
    }

    bool CMaterialInstance::RefreshTextureBindings(const CTexture* ChangedTexture)
    {
        bool bReferences = (ChangedTexture == nullptr);

        if (!bReferences)
        {
            for (const FMaterialParameterOverride& Override : Overrides)
            {
                if (Override.Type == EMaterialParameterType::Texture && Override.Texture.Get() == ChangedTexture)
                {
                    bReferences = true;
                    break;
                }
            }
        }

        // Inherited textures come from the root, which is why the driver refreshes masters before instances.
        if (!bReferences)
        {
            const CMaterial* Root = GetMaterial();
            bReferences = Root != nullptr && Root->ReferencesTexture(ChangedTexture);
        }

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

        // Asking the parent first, which owns every unoverridden slot, is what makes the rebuild non-blocking.
        if (!Material->RequestTexturesResolved())
        {
            return false;
        }

        // Loaded is not GPU-resident, and losing the race with the texture's PostLoad bakes the placeholder.
        for (const FMaterialParameterOverride& Override : Overrides)
        {
            if (Override.Type != EMaterialParameterType::Texture || !Override.bEnabled)
            {
                continue;
            }

            // A retained override for a parameter the root dropped binds nothing, so it cannot gate this.
            FMaterialParameter Param;
            if (!GetParameterValue(EMaterialParameterType::Texture, Override.ParameterName, Param))
            {
                continue;
            }

            if (Override.Texture != nullptr && Override.Texture->GetResourceID() < 0)
            {
                return false;
            }
        }

        CMaterial* Root = GetMaterial();
        if (Root == nullptr)
        {
            return true;
        }

        const uint32 OverriddenMask = GetOverriddenTextureMask();
        const FMaterialUniforms& Inherited = *Material->GetMaterialUniforms();

        bool bNeedsRebuild = false;

        const uint32 NumSlots = (uint32)Math::Min<size_t>(Root->Textures.size(), MAX_TEXTURES);
        for (uint32 i = 0; i < NumSlots && !bNeedsRebuild; ++i)
        {
            if ((OverriddenMask & (1u << i)) == 0)
            {
                bNeedsRebuild = MaterialUniforms.Textures[i] != Inherited.Textures[i];
            }
        }

        const uint32 Placeholder = RHI::Textures::DefaultResourceID();
        for (uint32 i = 0; i < MAX_TEXTURES && !bNeedsRebuild; ++i)
        {
            bNeedsRebuild = (OverriddenMask & (1u << i)) != 0
                         && MaterialUniforms.Textures[i] == Placeholder;
        }

        if (bNeedsRebuild)
        {
            RebuildUniformsFromOverrides();
            UpdateMaterialUniforms();
        }

        // Only reachable once the parent resolved, and queued since this gate runs on a worker fiber.

        return true;
    }

    FShaderH CMaterialInstance::GetVertexShader() const
    {
        CMaterial* Root = GetMaterial();
        return Root ? Root->GetStageForKey(EMaterialShaderStage::Vertex, GetStaticSwitchKey()) : FShaderH{};
    }

    FShaderH CMaterialInstance::GetPixelShader() const
    {
        CMaterial* Root = GetMaterial();
        return Root ? Root->GetStageForKey(EMaterialShaderStage::Pixel, GetStaticSwitchKey()) : FShaderH{};
    }

    EMaterialType CMaterialInstance::GetMaterialType() const
    {
        return Material ? Material->GetMaterialType() : EMaterialType::None;
    }

    bool CMaterialInstance::DoesCastShadows() const
    {
        return Material ? Material->DoesCastShadows() : false;
    }

    bool CMaterialInstance::IsTwoSided() const
    {
        return Material ? Material->IsTwoSided() : false;
    }

    bool CMaterialInstance::IsTranslucent()
    {
        return Material ? Material->IsTranslucent() : false;
    }

    bool CMaterialInstance::IsMasked()
    {
        return Material ? Material->IsMasked() : false;
    }

    bool CMaterialInstance::IsAdditive()
    {
        return Material ? Material->IsAdditive() : false;
    }

    bool CMaterialInstance::IsOpaque()
    {
        return Material ? Material->IsOpaque() : true;
    }

    bool CMaterialInstance::IsMomentResolved()
    {
        return Material ? Material->IsMomentResolved() : false;
    }

    bool CMaterialInstance::IsUnorderedBlend()
    {
        return Material ? Material->IsUnorderedBlend() : false;
    }

    bool CMaterialInstance::ReceivesDecals() const
    {
        return Material ? Material->ReceivesDecals() : true;
    }

    bool CMaterialInstance::WritesDepth() const
    {
        return Material ? Material->WritesDepth() : false;
    }

    bool CMaterialInstance::IsShadowOnly() const
    {
        return Material ? Material->IsShadowOnly() : false;
    }

    bool CMaterialInstance::IsUnlit()
    {
        // Through GetShadingModel so an override is honored and the two cannot disagree.
        return GetShadingModel() == EMaterialShadingModel::Unlit;
    }

    bool CMaterialInstance::DisableDepthTest()
    {
        return Material ? Material->DisableDepthTest() : false;
    }

    EBlendMode CMaterialInstance::GetBlendMode()
    {
        return Material ? Material->GetBlendMode() : EBlendMode::Opaque;
    }

    EMaterialShadingModel CMaterialInstance::GetShadingModel()
    {
        if (bOverrideShadingModel)
        {
            return ShadingModelOverride;
        }
        return Material ? Material->GetShadingModel() : EMaterialShadingModel::Lit;
    }

    float CMaterialInstance::GetOpacityMaskClipValue()
    {
        return Material ? Material->GetOpacityMaskClipValue() : 0.333f;
    }

    void CMaterialInstance::PostLoad()
    {
        LUMINA_MEMORY_SCOPE("Materials");
        if (!Material)
        {
            return;
        }

        // Register before the parent's PostLoad so its PropagateToChildren reaches this level.
        Material->RegisterChild(this);

        if (!Material->IsReadyForRender())
        {
            Material->PostLoad();
        }
        else
        {
            RebuildUniformsFromOverrides();
        }

        // Headless has no material table; the slot stays -1, which every consumer already handles.
        if (GetMaterialIndex() == -1)
        {
            if (FRenderManager* RenderManager = TryRender())
            {
                RenderManager->GetMaterialManager().AddMaterial(this);
            }
        }
        else
        {
            UpdateMaterialUniforms();
        }

        SetReadyForRender(true);

        // A loaded instance may name a permutation the master has never built, and nothing else asks.
        RequestStaticSwitchPermutation();

        // Surfaces that fell back to the default still record this as a dependency, so they wake here.
        FMeshResolveCache::InvalidateDependency(this);
    }

    void CMaterialInstance::OnDestroy()
    {
        CMaterialInterface::OnDestroy();

        // Before MaterialIndex can be recycled by the next material.

        // Resolves are keyed partly on this pointer; drop them before it can be recycled.
        FMeshResolveCache::InvalidateDependency(this);

        if (Material)
        {
            Material->UnregisterChild(this);
        }

        // An instance outliving the renderer must release quietly rather than assert.
        if (GetMaterialIndex() != -1)
        {
            if (FRenderManager* RenderManager = TryRender())
            {
                RHI::FRenderRelease Release;
                Release.MaterialSlot = GetMaterialIndex();
                RenderManager->GetReleaseQueue().Post(Release);
            }

            SetMaterialIndex(-1);
        }
    }
}
