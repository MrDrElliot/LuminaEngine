#pragma once
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/Object.h"
#include "Renderer/RHIFwd.h"
#include "Renderer/Vertex.h"
#include "MaterialInterface.generated.h"

namespace Lumina
{
    struct FMaterialUniforms;
    class CMaterial;
    struct FMaterialParameter;
    enum class EMaterialParameterType : uint8;
}

namespace Lumina
{

    REFLECT()
    enum class EMaterialType : uint8
    {
        None,
        PBR,
        PostProcess,
        UI,
        Terrain,
        Decal,
    };

    REFLECT()
    enum class EBlendMode : uint8
    {
        Opaque,
        Masked,
        Translucent,
        Additive,
    };

    REFLECT()
    enum class EMaterialShadingModel : uint8
    {
        Lit,
        Unlit,
    };

    REFLECT()
    class RUNTIME_API CMaterialInterface : public CObject
    {
        GENERATED_BODY()
    public:

        virtual CMaterial* GetMaterial() const { return nullptr; }
        virtual bool SetVectorValue(const FName& Name, const FVector4& Value) { return false; }
        virtual bool SetScalarValue(const FName& Name, const float Value) { return false; }
        virtual bool GetParameterValue(EMaterialParameterType Type, const FName& Name, FMaterialParameter& Param) { return false; }
        virtual FMaterialUniforms* GetMaterialUniforms() { return nullptr; }

        int32 GetMaterialIndex() const { return MaterialIndex; }
        void SetMaterialIndex(int32 Index) { MaterialIndex = Index; }

        virtual const FShaderEntry* GetVertexShader() const { return nullptr; }
        virtual const FShaderEntry* GetPixelShader() const { return nullptr; }

        virtual EMaterialType GetMaterialType() const { return EMaterialType::None; };

        virtual bool DoesCastShadows() const { return false; }
        virtual bool IsTwoSided() const { return false; }
        virtual bool IsTranslucent() { return false; }
        virtual bool IsMasked() { return false; }
        virtual bool IsAdditive() { return false; }
        virtual bool IsOpaque() { return true; }
        virtual bool IsUnlit() { return false; }
        virtual bool DisableDepthTest() { return false; }
        virtual EBlendMode GetBlendMode() { return EBlendMode::Opaque; }
        virtual EMaterialShadingModel GetShadingModel() { return EMaterialShadingModel::Lit; }
        virtual float GetOpacityMaskClipValue() { return 0.333f; }

        void SetReadyForRender(bool bReady) { bReadyForRender.store(bReady, std::memory_order_release); }
        bool IsReadyForRender() const { return bReadyForRender.load(std::memory_order_acquire); }

    protected:
        
        virtual void UpdateMaterialUniforms() { }

        std::atomic_bool        bReadyForRender;

        int32                   MaterialIndex = -1;
    };
}
