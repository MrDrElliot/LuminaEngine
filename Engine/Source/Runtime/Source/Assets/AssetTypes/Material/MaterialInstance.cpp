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
        return Material.Get();
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

    static void ApplyOverride(CMaterial* Material, const FMaterialParameterOverride& Override, FMaterialUniforms& Uniforms)
    {
        // Through the parent's name->parameter map rather than a scan of its parameter list: this runs once
        // per override, and a material carries up to 72 parameters.
        FMaterialParameter Param;
        if (!Material->GetParameterValue(Override.Type, Override.ParameterName, Param))
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
            Material->RegisterInstance(this);
        }
    }

    void CMaterialInstance::RebuildUniformsFromOverrides()
    {
        if (!Material)
        {
            return;
        }

        // The choke point for every override change (RefreshFromParent, PostPropertyChange, enabling or
        // disabling a parameter), and an override flipping on or off changes which texture a slot samples
        // -- so it changes what this instance owes the streamer.

        EnsureRegisteredWithParent();

        MaterialUniforms = Material->MaterialUniforms;

        // Overrides are never pruned here: a recompile that drops a parameter must not destroy its value.

        // Slots are demanded from the parent one at a time, so a parameter this instance overrides never
        // asks the parent to resolve its default. Runs BEFORE the override loop, which has the last word.
        const uint32 OverriddenMask = GetOverriddenTextureMask();
        for (const FMaterialParameter& Param : Material->Parameters)
        {
            if (Param.Type != EMaterialParameterType::Texture || Param.Index >= MAX_TEXTURES)
            {
                continue;
            }

            if ((OverriddenMask & (1u << Param.Index)) != 0)
            {
                continue;   // ApplyOverride supplies this slot; the parent default is dead weight
            }

            MaterialUniforms.Textures[Param.Index] = Material->ResolveTextureSlot(Param.Index);
        }

        for (const FMaterialParameterOverride& Override : Overrides)
        {
            // Disabled overrides keep their stored value but are not applied; the parent value shows through.
            if (Override.bEnabled)
            {
                ApplyOverride(Material, Override, MaterialUniforms);
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
    }

    void CMaterialInstance::PostPropertyChange(FProperty* ChangedProperty)
    {
        Super::PostPropertyChange(ChangedProperty);
        
        RebuildUniformsFromOverrides();
        UpdateMaterialUniforms();
    }

    uint32 CMaterialInstance::GetOverriddenTextureMask() const
    {
        if (!Material)
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
            if (Material->GetParameterValue(EMaterialParameterType::Texture, Override.ParameterName, Param)
                && Param.Index < MAX_TEXTURES)
            {
                Mask |= (1u << Param.Index);
            }
        }

        return Mask;
    }

    bool CMaterialInstance::IsTextureSlotOverridden(uint32 Index) const
    {
        // A slot no parameter names is a plainly bound texture on the parent, and there is nothing an
        // instance could override it with -- the mask has no bit for it either way.
        return Index < MAX_TEXTURES && (GetOverriddenTextureMask() & (1u << Index)) != 0;
    }

    void CMaterialInstance::RefreshInheritedTextureSlots()
    {
        if (!Material)
        {
            return;
        }

        const uint32 OverriddenMask = GetOverriddenTextureMask();
        
        uint32 FirstChanged = MAX_TEXTURES;
        uint32 LastChanged  = 0;

        const uint32 NumSlots = (uint32)Math::Min<size_t>(Material->Textures.size(), MAX_TEXTURES);
        for (uint32 i = 0; i < NumSlots; ++i)
        {
            if ((OverriddenMask & (1u << i)) != 0)
            {
                continue;
            }

            if (MaterialUniforms.Textures[i] != Material->MaterialUniforms.Textures[i])
            {
                MaterialUniforms.Textures[i] = Material->MaterialUniforms.Textures[i];
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
        if (!Material->GetParameterValue(EMaterialParameterType::Scalar, Name, Param))
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
        if (!Material->GetParameterValue(EMaterialParameterType::Vector, Name, Param))
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
        if (!Material->GetParameterValue(EMaterialParameterType::Texture, Name, Param))
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
                : Material->MaterialUniforms.Textures[Param.Index];

            UploadUniformField(TextureFieldOffset(Param.Index), &MaterialUniforms.Textures[Param.Index], sizeof(uint32));
        }

        return true;
    }

    const TVector<FMaterialParameter>& CMaterialInstance::GetMaterialParams() const
    {
        static const TVector<FMaterialParameter> Empty;
        return Material ? Material->Parameters : Empty;
    }

    bool CMaterialInstance::GetParameterValue(EMaterialParameterType Type, const FName& Name, FMaterialParameter& Param)
    {
        Param = {};

        // The parent owns the parameter list and indexes it by name; an instance only diverges in VALUES,
        // so there is nothing here a linear scan of a private copy would answer differently.
        return Material != nullptr && Material->GetParameterValue(Type, Name, Param);
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
                RebuildUniformsFromOverrides();
                UpdateMaterialUniforms();
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

        FMaterialParameterOverride& Override = FindOrAddOverride(Overrides, Name, Param.Type);
        Override.bEnabled = true;
        switch (Param.Type)
        {
        case EMaterialParameterType::Scalar:
            Override.Scalar = (Param.Index < MAX_SCALARS) ? Material->MaterialUniforms.Scalars[Param.Index] : 0.0f;
            break;
        case EMaterialParameterType::Vector:
            Override.Vector = (Param.Index < MAX_VECTORS) ? Material->MaterialUniforms.Vectors[Param.Index] : FVector4(0.0f);
            break;
        case EMaterialParameterType::Texture:
            if (Param.Index < (uint32)Material->Textures.size())
            {
                Material->ResolveTextureSlot(Param.Index);
                Override.Texture = Material->ResolvedTextures[Param.Index];
            }
            else
            {
                Override.Texture = nullptr;
            }
            break;
        }

        RebuildUniformsFromOverrides();
        UpdateMaterialUniforms();
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
        RebuildUniformsFromOverrides();
        UpdateMaterialUniforms();
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

        // An instance inherits every texture it does not override, so a change to one the PARENT binds
        // reaches it too -- which is why the driver refreshes masters before instances.
        if (!bReferences && Material != nullptr && Material->ReferencesTexture(ChangedTexture))
        {
            bReferences = true;
        }

        if (!bReferences)
        {
            return false;
        }

        RebuildUniformsFromOverrides();
        UpdateMaterialUniforms();
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

            // A retained override for a parameter the parent dropped binds nothing, so it cannot gate this.
            FMaterialParameter Param;
            if (!Material->GetParameterValue(EMaterialParameterType::Texture, Override.ParameterName, Param))
            {
                continue;
            }

            if (Override.Texture != nullptr && Override.Texture->GetResourceID() < 0)
            {
                return false;
            }
        }
        
        const uint32 OverriddenMask = GetOverriddenTextureMask();

        bool bNeedsRebuild = false;

        const uint32 NumSlots = (uint32)Math::Min<size_t>(Material->Textures.size(), MAX_TEXTURES);
        for (uint32 i = 0; i < NumSlots && !bNeedsRebuild; ++i)
        {
            if ((OverriddenMask & (1u << i)) == 0)
            {
                bNeedsRebuild = MaterialUniforms.Textures[i] != Material->MaterialUniforms.Textures[i];
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

        // Register before parent's PostLoad so NotifyInstancesParentChanged refreshes our cached params.
        Material->RegisterInstance(this);

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
            Material->UnregisterInstance(this);
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
