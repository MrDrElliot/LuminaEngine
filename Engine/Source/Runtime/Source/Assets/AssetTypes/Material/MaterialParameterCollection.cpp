#include "RuntimePCH.h"
#include "MaterialParameterCollection.h"

#include "Core/Engine/Engine.h"
#include "Memory/MemoryTracking.h"
#include "Renderer/MaterialManager.h"
#include "Renderer/RenderManager.h"
#include "Log/Log.h"

namespace Lumina
{
    namespace
    {
        void WarnOverBudget(const CObject* Collection, const char* Kind, uint32 Declared, uint32 Capacity)
        {
            LOG_ERROR("Parameter collection '{}' declares {} {} parameters, past the budget of {}. The ones "
                      "past the budget read zero in every shader that samples them.",
                      Collection->GetName(), Declared, Kind, Capacity);
        }
    }

    int32 CMaterialParameterCollection::FindScalarIndex(const FName& Name) const
    {
        for (uint32 i = 0; i < (uint32)ScalarParameters.size() && i < MAX_COLLECTION_SCALARS; ++i)
        {
            if (ScalarParameters[i].ParameterName == Name)
            {
                return (int32)i;
            }
        }
        return INDEX_NONE;
    }

    int32 CMaterialParameterCollection::FindVectorIndex(const FName& Name) const
    {
        for (uint32 i = 0; i < (uint32)VectorParameters.size() && i < MAX_COLLECTION_VECTORS; ++i)
        {
            if (VectorParameters[i].ParameterName == Name)
            {
                return (int32)i;
            }
        }
        return INDEX_NONE;
    }

    void CMaterialParameterCollection::RebuildUniforms()
    {
        Uniforms = FMaterialCollectionUniforms{};

        if (ScalarParameters.size() > MAX_COLLECTION_SCALARS)
        {
            WarnOverBudget(this, "scalar", (uint32)ScalarParameters.size(), MAX_COLLECTION_SCALARS);
        }
        if (VectorParameters.size() > MAX_COLLECTION_VECTORS)
        {
            WarnOverBudget(this, "vector", (uint32)VectorParameters.size(), MAX_COLLECTION_VECTORS);
        }

        for (uint32 i = 0; i < (uint32)ScalarParameters.size() && i < MAX_COLLECTION_SCALARS; ++i)
        {
            Uniforms.Scalars[i] = ScalarParameters[i].DefaultValue;
        }
        for (uint32 i = 0; i < (uint32)VectorParameters.size() && i < MAX_COLLECTION_VECTORS; ++i)
        {
            Uniforms.Vectors[i] = VectorParameters[i].DefaultValue;
        }

        if (CollectionIndex != INDEX_NONE)
        {
            if (FRenderManager* RenderManager = TryRender())
            {
                RenderManager->GetCollectionManager().Update(CollectionIndex, Uniforms);
            }
        }
    }

    void CMaterialParameterCollection::PostLoad()
    {
        LUMINA_MEMORY_SCOPE("Materials");

        // Headless has no table, and every consumer already treats INDEX_NONE as no GPU slot.
        if (CollectionIndex == INDEX_NONE)
        {
            if (FRenderManager* RenderManager = TryRender())
            {
                CollectionIndex = RenderManager->GetCollectionManager().Acquire();
            }
        }

        RebuildUniforms();
    }

    void CMaterialParameterCollection::OnDestroy()
    {
        CObject::OnDestroy();

        if (CollectionIndex != INDEX_NONE)
        {
            if (FRenderManager* RenderManager = TryRender())
            {
                RenderManager->GetCollectionManager().Release(CollectionIndex);
            }
            CollectionIndex = INDEX_NONE;
        }
    }

    void CMaterialParameterCollection::PostPropertyChange(FProperty* ChangedProperty)
    {
        CObject::PostPropertyChange(ChangedProperty);

        // A rename or reorder moves the index a compiled material reads, so nothing here is targeted.
        RebuildUniforms();
    }

    bool CMaterialParameterCollection::SetScalarValue(const FName& Name, float Value)
    {
        const int32 Index = FindScalarIndex(Name);
        if (Index == INDEX_NONE)
        {
            return false;
        }

        Uniforms.Scalars[Index] = Value;

        if (CollectionIndex != INDEX_NONE)
        {
            if (FRenderManager* RenderManager = TryRender())
            {
                RenderManager->GetCollectionManager().UpdateRange(CollectionIndex,
                    CollectionScalarFieldOffset((uint32)Index), &Uniforms.Scalars[Index], sizeof(float));
            }
        }

        return true;
    }

    bool CMaterialParameterCollection::SetVectorValue(const FName& Name, FVector4 Value)
    {
        const int32 Index = FindVectorIndex(Name);
        if (Index == INDEX_NONE)
        {
            return false;
        }

        Uniforms.Vectors[Index] = Value;

        if (CollectionIndex != INDEX_NONE)
        {
            if (FRenderManager* RenderManager = TryRender())
            {
                RenderManager->GetCollectionManager().UpdateRange(CollectionIndex,
                    CollectionVectorFieldOffset((uint32)Index), &Uniforms.Vectors[Index], sizeof(FVector4));
            }
        }

        return true;
    }

    float CMaterialParameterCollection::GetScalarValue(const FName& Name, float Default) const
    {
        const int32 Index = FindScalarIndex(Name);
        return Index != INDEX_NONE ? Uniforms.Scalars[Index] : Default;
    }

    FVector4 CMaterialParameterCollection::GetVectorValue(const FName& Name, FVector4 Default) const
    {
        const int32 Index = FindVectorIndex(Name);
        return Index != INDEX_NONE ? Uniforms.Vectors[Index] : Default;
    }
}
