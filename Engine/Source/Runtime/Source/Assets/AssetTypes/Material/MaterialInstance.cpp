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

    static void ApplyOverride(const FMaterialParameterOverride& Override, const TVector<FMaterialParameter>& Params, FMaterialUniforms& Uniforms)
    {
        for (const FMaterialParameter& Param : Params)
        {
            if (Param.ParameterName != Override.ParameterName || Param.Type != Override.Type)
            {
                continue;
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
            return;
        }
    }

    void CMaterialInstance::RebuildUniformsFromOverrides()
    {
        if (!Material)
        {
            return;
        }

        Parameters = Material->Parameters;
        MaterialUniforms = Material->MaterialUniforms;

        // Drop overrides whose parent parameter is gone/retyped, otherwise dead entries persist forever.
        Overrides.erase(eastl::remove_if(Overrides.begin(), Overrides.end(),
            [this](const FMaterialParameterOverride& O)
            {
                FMaterialParameter Probe;
                return !Material->GetParameterValue(O.Type, O.ParameterName, Probe);
            }), Overrides.end());

        // Texture slots are demanded from the parent one at a time rather than inherited with the block
        // above, which is where the memory saving actually happens: a parameter this instance overrides
        // never asks the parent to resolve its default, so a master reached only through instances that
        // override everything loads none of its textures at all.
        //
        // Runs BEFORE the override loop so ApplyOverride still has the last word on every slot.
        for (const FMaterialParameter& Param : Parameters)
        {
            if (Param.Type != EMaterialParameterType::Texture || Param.Index >= MAX_TEXTURES)
            {
                continue;
            }

            const FMaterialParameterOverride* Override = FindOverride(Param.ParameterName);
            const bool bOverridden = Override != nullptr && Override->bEnabled && Override->Texture != nullptr;
            if (bOverridden)
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
                ApplyOverride(Override, Parameters, MaterialUniforms);
            }
        }
    }

    void CMaterialInstance::RefreshFromParent()
    {
        RebuildUniformsFromOverrides();
        UpdateMaterialUniforms();
    }

    bool CMaterialInstance::IsTextureSlotOverridden(uint32 Index) const
    {
        for (const FMaterialParameter& Param : Parameters)
        {
            if (Param.Type != EMaterialParameterType::Texture || Param.Index != Index)
            {
                continue;
            }

            const FMaterialParameterOverride* Override = FindOverride(Param.ParameterName);
            return Override != nullptr && Override->bEnabled && Override->Texture != nullptr;
        }

        // No parameter names this slot, so it is a plainly bound texture on the parent and there is
        // nothing an instance could override it with.
        return false;
    }

    void CMaterialInstance::RefreshInheritedTextureSlots()
    {
        if (!Material)
        {
            return;
        }

        bool bChanged = false;

        const uint32 NumSlots = (uint32)Math::Min<size_t>(Material->Textures.size(), MAX_TEXTURES);
        for (uint32 i = 0; i < NumSlots; ++i)
        {
            if (IsTextureSlotOverridden(i))
            {
                continue;
            }

            if (MaterialUniforms.Textures[i] != Material->MaterialUniforms.Textures[i])
            {
                MaterialUniforms.Textures[i] = Material->MaterialUniforms.Textures[i];
                bChanged = true;
            }
        }

        if (bChanged)
        {
            UpdateMaterialUniforms();
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

        FMaterialParameter Param;
        if (!Material->GetParameterValue(EMaterialParameterType::Scalar, Name, Param))
        {
            LOG_ERROR("Failed to find parent scalar parameter '{}'", Name);
            return false;
        }

        FMaterialParameterOverride& Override = FindOrAddOverride(Overrides, Name, EMaterialParameterType::Scalar);
        Override.Scalar = Value;
        Override.bEnabled = true;

        RebuildUniformsFromOverrides();
        UpdateMaterialUniforms();

        return true;
    }

    bool CMaterialInstance::SetVectorValue(const FName& Name, const FVector4& Value)
    {
        if (!Material)
        {
            return false;
        }

        FMaterialParameter Param;
        if (!Material->GetParameterValue(EMaterialParameterType::Vector, Name, Param))
        {
            LOG_ERROR("Failed to find parent vector parameter '{}'", Name);
            return false;
        }

        FMaterialParameterOverride& Override = FindOrAddOverride(Overrides, Name, EMaterialParameterType::Vector);
        Override.Vector = Value;
        Override.bEnabled = true;

        RebuildUniformsFromOverrides();
        UpdateMaterialUniforms();

        return true;
    }

    bool CMaterialInstance::SetTextureValue(const FName& Name, CTexture* TextureValue)
    {
        if (!Material)
        {
            return false;
        }

        FMaterialParameter Param;
        if (!Material->GetParameterValue(EMaterialParameterType::Texture, Name, Param))
        {
            LOG_ERROR("Failed to find parent texture parameter '{}'", Name);
            return false;
        }

        FMaterialParameterOverride& Override = FindOrAddOverride(Overrides, Name, EMaterialParameterType::Texture);
        Override.Texture = TextureValue;
        Override.bEnabled = true;

        RebuildUniformsFromOverrides();
        UpdateMaterialUniforms();

        return true;
    }

    bool CMaterialInstance::GetParameterValue(EMaterialParameterType Type, const FName& Name, FMaterialParameter& Param)
    {
        Param = {};
        auto* Itr = eastl::find_if(Parameters.begin(), Parameters.end(), [Type, Name](const FMaterialParameter& Param)
        {
           return Param.ParameterName == Name && Param.Type == Type;
        });

        if (Itr != Parameters.end())
        {
            Param = *Itr;
            return true;
        }

        return false;
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
                O.bEnabled = bEnabled;
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
            // Seeding an override from the parent's default is a deliberate demand for that slot -- the
            // user is about to edit the value, so it has to be a real texture rather than a soft path.
            // Editor action, so the synchronous resolve is fine here.
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
        Overrides.erase(eastl::remove_if(Overrides.begin(), Overrides.end(), [Name](const FMaterialParameterOverride& O)
        {
            return O.ParameterName == Name;
        }), Overrides.end());

        RebuildUniformsFromOverrides();

        UpdateMaterialUniforms();
    }

    void CMaterialInstance::UpdateMaterialUniforms()
    {
        if (MaterialIndex != -1)
        {
            GRenderManager->GetMaterialManager().UpdateMaterialUniforms(&MaterialUniforms, (uint32)MaterialIndex);
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
        // reaches it too. RebuildUniformsFromOverrides copies the parent's block wholesale, which is why
        // the driver has to refresh masters before instances.
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

        // The parent owns every slot this instance does not override, and its own request handles the
        // async kick + one-request-per-material throttle. Asking it first is also what makes the rebuild
        // below non-blocking: by the time it reports true, ResolveTextureSlot is a pointer read.
        if (!Material->RequestTexturesResolved())
        {
            return false;
        }

        // An override's texture is a hard ref, so it is loaded -- but "loaded" is not "GPU-resident".
        // RHI::Textures::Create assigns the heap slot in the texture's own PostLoad, and material texture
        // refs sit outside the load graph's Hard/Owned BFS, so nothing orders that ahead of this
        // instance's PostLoad. Losing that race is what bakes the placeholder in.
        for (const FMaterialParameterOverride& Override : Overrides)
        {
            if (Override.Type != EMaterialParameterType::Texture || !Override.bEnabled)
            {
                continue;
            }
            if (Override.Texture != nullptr && Override.Texture->GetResourceID() < 0)
            {
                return false;
            }
        }

        // Everything resolvable now. If the block was baked while something was still missing it is
        // holding the placeholder, and this is the only thing that rewrites it -- previously the resolve
        // gate skipped instances outright on the premise that their block was "already whole", which is
        // true only when the load race was won.
        //
        // Driven off the parent's SLOT COUNT rather than off Parameters. A plain Texture Sample node
        // binds a slot in CMaterial::Textures with no FMaterialParameter behind it, so a Parameters-driven
        // scan is blind to exactly the slots an instance can only inherit -- which is what left instances
        // magenta until their master was recompiled.
        bool bNeedsRebuild = false;

        const uint32 NumSlots = (uint32)Math::Min<size_t>(Material->Textures.size(), MAX_TEXTURES);
        for (uint32 i = 0; i < NumSlots && !bNeedsRebuild; ++i)
        {
            // Inherited slots: the parent's block is the source of truth and it is rewritten
            // asynchronously as loads land, so any divergence means this copy is stale.
            if (!IsTextureSlotOverridden(i))
            {
                bNeedsRebuild = MaterialUniforms.Textures[i] != Material->MaterialUniforms.Textures[i];
            }
        }

        // Overridden slots have no parent value to compare against; they are wrong exactly when they are
        // still holding the placeholder the override was baked with before its texture went resident.
        const uint32 Placeholder = RHI::Textures::DefaultResourceID();
        for (const FMaterialParameter& Param : Parameters)
        {
            if (bNeedsRebuild)
            {
                break;
            }
            if (Param.Type != EMaterialParameterType::Texture || Param.Index >= MAX_TEXTURES)
            {
                continue;
            }
            bNeedsRebuild = MaterialUniforms.Textures[Param.Index] == Placeholder
                         && IsTextureSlotOverridden(Param.Index);
        }

        if (bNeedsRebuild)
        {
            RebuildUniformsFromOverrides();
            UpdateMaterialUniforms();
        }

        return true;
    }

    const FShaderEntry* CMaterialInstance::GetVertexShader() const
    {
        return Material ? Material->GetVertexShader() : nullptr;
    }

    const FShaderEntry* CMaterialInstance::GetPixelShader() const
    {
        return Material ? Material->GetPixelShader() : nullptr;
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
        return Material ? Material->IsUnlit() : false;
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

        if (GetMaterialIndex() == -1)
        {
            GRenderManager->GetMaterialManager().AddMaterial(this);
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

        // Resolves are keyed partly on this pointer; drop them before it can be recycled.
        FMeshResolveCache::InvalidateDependency(this);

        if (Material)
        {
            Material->UnregisterInstance(this);
        }

        if (GetMaterialIndex() != -1)
        {
            GRenderManager->GetMaterialManager().RemoveMaterial(this);
        }
    }
}
