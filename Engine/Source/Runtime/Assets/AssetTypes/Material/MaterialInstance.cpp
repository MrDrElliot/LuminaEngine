#include "pch.h"
#include "MaterialInstance.h"
#include "Material.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Core/Engine/Engine.h"
#include "Renderer/RenderManager.h"


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
            Override.Texture = (Param.Index < Material->Textures.size()) ? Material->Textures[Param.Index] : nullptr;
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
    }

    void CMaterialInstance::OnDestroy()
    {
        CMaterialInterface::OnDestroy();

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
