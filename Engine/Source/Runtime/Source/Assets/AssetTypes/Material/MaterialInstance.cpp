#include "RuntimePCH.h"
#include "MaterialInstance.h"
#include "Material.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Core/Engine/Engine.h"
#include "Renderer/RenderManager.h"
#include "World/Scene/RenderScene/MeshResolveCache.h"
#include "Log/Log.h"


namespace Lumina
{
    CMaterialInstance::CMaterialInstance()
    {
        Memory::Memzero(&MaterialUniforms, sizeof(FMaterialUniforms));
    }

    CMaterial* CMaterialInstance::GetMaterial() const
    {
        // Bounded, not while(Parent): a cycle that slipped past SetParentMaterial must not hang the walk.
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

    /** Byte offsets of a single parameter's field inside the uniform block, for the targeted uploads the
        setters do. The block is a POD aggregate, so offsetof is exact and the array elements contiguous. */
    static constexpr uint32 ScalarFieldOffset(uint32 Index)
    {
        return (uint32)(offsetof(FMaterialUniforms, Scalars) + Index * sizeof(float));
    }

    static constexpr uint32 VectorFieldOffset(uint32 Index)
    {
        return (uint32)(offsetof(FMaterialUniforms, Vectors) + Index * sizeof(FVector4));
    }

    static constexpr uint32 TextureFieldOffset(uint32 Index)
    {
        return (uint32)(offsetof(FMaterialUniforms, Textures) + Index * sizeof(uint32));
    }

    static void ApplyOverride(CMaterial* Root, const FMaterialParameterOverride& Override, FMaterialUniforms& Uniforms)
    {
        // Through the root's name->parameter map, not a scan: a material carries up to 72 parameters.
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

        // The choke point for every override change (RefreshFromParent, PostPropertyChange, enabling or
        // disabling a parameter), and an override flipping on or off changes which texture a slot samples
        // -- so it changes what this instance owes the streamer.

        EnsureRegisteredWithParent();

        // The IMMEDIATE parent's resolved block, which composes a chain only because propagation is top-down.
        MaterialUniforms = *Material->GetMaterialUniforms();

        // Overrides are never pruned here: a recompile that drops a parameter must not destroy its value.

        // Slots are demanded from the parent one at a time, so a parameter this instance overrides never
        // asks the parent to resolve its default. Runs BEFORE the override loop, which has the last word.
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

        // The flags came from the parent wholesale, shading model included. Re-stamp the model field so an
        // instance override lands -- last, so nothing above can undo it.
        const uint32 ModelBits =
            ((uint32)GetShadingModel() & kMaterialShadingModelMask) << kMaterialShadingModelShift;

        MaterialUniforms.Flags &= ~(kMaterialShadingModelMask << kMaterialShadingModelShift);
        MaterialUniforms.Flags |= ModelBits;
    }

    void CMaterialInstance::RefreshFromParent()
    {
        RebuildUniformsFromOverrides();
        UpdateMaterialUniforms();

        // No InvalidateDependency: a resolve stamps the GPU slot INDEX, which a value change never moves.
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
        // A slot no parameter names is a plainly bound texture on the parent, and there is nothing an
        // instance could override it with -- the mask has no bit for it either way.
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

        // Unconditionally, not just when a slot moved: this can be the first call after the parent finished
        // resolving, where the inherited IDs already matched but the streamer has never seen the mapping.
        //
        // Marked, not published: the parent calls this from its async texture-load completion (hence "not on
        // the game thread" in the header), and publishing walks the parent's ResolvedTextures -- the array
        // that completion is writing. The streamer drains the queue on the game thread instead.
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
            LOG_ERROR("Failed to find parent scalar parameter '{}'", Name);
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

        PropagateToChildren();
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
            LOG_ERROR("Failed to find parent vector parameter '{}'", Name);
            return false;
        }

        FMaterialParameterOverride& Override = FindOrAddOverride(Overrides, Name, EMaterialParameterType::Vector);
        Override.Vector = Value;
        Override.bEnabled = true;

        // See SetScalarValue: a targeted 16-byte write, not a rebuild of the whole block.
        if (Param.Index < MAX_VECTORS)
        {
            MaterialUniforms.Vectors[Param.Index] = Value;
            UploadUniformField(VectorFieldOffset(Param.Index), &MaterialUniforms.Vectors[Param.Index], sizeof(FVector4));
        }

        PropagateToChildren();
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
            LOG_ERROR("Failed to find parent texture parameter '{}'", Name);
            return false;
        }

        FMaterialParameterOverride& Override = FindOrAddOverride(Overrides, Name, EMaterialParameterType::Texture);
        Override.Texture = TextureValue;
        Override.bEnabled = true;

        // Writes the uniform slot directly rather than going through RebuildUniformsFromOverrides, so it
        // has to mark dirty itself.

        if (Param.Index < MAX_TEXTURES)
        {
            const int32 ResourceID = (TextureValue != nullptr) ? TextureValue->GetResourceID() : -1;
            MaterialUniforms.Textures[Param.Index] = (ResourceID >= 0)
                ? (uint32)ResourceID
                : Material->GetResolvedTextureSlot(Param.Index);

            UploadUniformField(TextureFieldOffset(Param.Index), &MaterialUniforms.Textures[Param.Index], sizeof(uint32));
        }

        PropagateToChildren();
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

        // An existing entry just flips its flag; the stored value is retained either way, so re-enabling
        // restores the user's edits rather than resetting to the parent.
        for (FMaterialParameterOverride& O : Overrides)
        {
            if (O.ParameterName == Name)
            {
                if (O.bEnabled == bEnabled)
                {
                    return;
                }

                O.bEnabled = bEnabled;

                // A full rebuild, unlike the setters: disabling has to restore the parent's value, which
                // for a texture means resolving the parent default this instance had been skipping.
                RefreshSubtree();
                return;
            }
        }

        // Disabling a parameter that was never overridden: nothing to do.
        if (!bEnabled)
        {
            return;
        }

        // First enable: create the entry seeded with the parent's current value so it starts where the
        // parameter already is. From here the value persists across toggles.
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
        auto NewEnd = eastl::remove_if(Overrides.begin(), Overrides.end(), [Name](const FMaterialParameterOverride& O)
        {
            return O.ParameterName == Name;
        });

        // Nothing matched, so nothing this could have changed.
        if (NewEnd == Overrides.end())
        {
            return;
        }

        Overrides.erase(NewEnd, Overrides.end());

        // Dropping an override restores the parent's value, which for a texture means resolving the parent
        // default this instance had been skipping -- a rebuild, not a targeted write.
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

    void CMaterialInstance::UploadUniformField(uint32 ByteOffset, const void* Data, uint32 ByteSize)
    {
        if (MaterialIndex != -1)
        {
            Render().GetMaterialManager().UpdateMaterialUniformRange((uint32)MaterialIndex, ByteOffset, Data, ByteSize);
        }
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

        // The parent owns every slot this instance does not override and handles the async kick. Asking it
        // first is what makes the rebuild below non-blocking.
        if (!Material->RequestTexturesResolved())
        {
            return false;
        }

        // An override's texture is a hard ref, so it is loaded -- but loaded is not GPU-resident, and the
        // heap slot is assigned in the texture's own PostLoad. Losing that race bakes the placeholder in.
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

        // Only reachable once the parent has fully resolved (the early-out above), so the parent slots this
        // instance inherits are settled and the collected set will not be stale a frame later. Queued rather
        // than published -- this gate runs on a worker fiber inside Extract.

        return true;
    }

    FShaderH CMaterialInstance::GetVertexShader() const
    {
        return Material ? Material->GetVertexShader() : FShaderH{};
    }

    FShaderH CMaterialInstance::GetPixelShader() const
    {
        return Material ? Material->GetPixelShader() : FShaderH{};
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

    bool CMaterialInstance::IsUnlit()
    {
        // Through GetShadingModel so an override is honoured here too; querying the parent directly
        // would let the two disagree about the same instance.
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

        // Surfaces that fell back to the default material while this was compiling still record it as a
        // dependency, so they are woken here even though they never resolved against it.
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

        // Deferred for the same reason as CMaterial::OnDestroy -- see the note there.
        // TryRender on a teardown path: an instance outliving the renderer must release quietly, not assert.
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
