#include "Platform/GenericPlatform.h"
#include "World/Entity/Components/DynamicMeshComponent.h"
#include "Scripting/DotNet/DotNetExport.h"

// A member function cannot take a Span, so the C# facade declares matching partials instead.

namespace Lumina
{
    namespace
    {
        FORCEINLINE SDynamicMeshComponent* AsDynamicMesh(void* Self)
        {
            return static_cast<SDynamicMeshComponent*>(Self);
        }
    }
}

using namespace Lumina;

LUMINA_DOTNET_EXPORT(void, DynMesh_SetPositions)(void* Self, const float* Data, int32 Count)
{
    if (SDynamicMeshComponent* Comp = AsDynamicMesh(Self))
    {
        Comp->SetPositionsData(Data, Count);
    }
}

LUMINA_DOTNET_EXPORT(void, DynMesh_SetNormals)(void* Self, const float* Data, int32 Count)
{
    if (SDynamicMeshComponent* Comp = AsDynamicMesh(Self))
    {
        Comp->SetNormalsData(Data, Count);
    }
}

LUMINA_DOTNET_EXPORT(void, DynMesh_SetUVs)(void* Self, const float* Data, int32 Count)
{
    if (SDynamicMeshComponent* Comp = AsDynamicMesh(Self))
    {
        Comp->SetUVsData(Data, Count);
    }
}

LUMINA_DOTNET_EXPORT(void, DynMesh_SetColorsFloat)(void* Self, const float* Data, int32 Count)
{
    if (SDynamicMeshComponent* Comp = AsDynamicMesh(Self))
    {
        Comp->SetColorsFloatData(Data, Count);
    }
}

LUMINA_DOTNET_EXPORT(void, DynMesh_SetColorsPacked)(void* Self, const uint32* Data, int32 Count)
{
    if (SDynamicMeshComponent* Comp = AsDynamicMesh(Self))
    {
        Comp->SetColorsPackedData(Data, Count);
    }
}

LUMINA_DOTNET_EXPORT(void, DynMesh_SetIndices)(void* Self, const uint32* Data, int32 Count)
{
    if (SDynamicMeshComponent* Comp = AsDynamicMesh(Self))
    {
        Comp->SetIndicesData(Data, Count);
    }
}
