#include "Platform/GenericPlatform.h"
#include "World/Entity/Components/DynamicMeshComponent.h"
#include "Scripting/DotNet/DotNetExport.h"

// Hand-written native -> C# bindings for SDynamicMeshComponent's bulk vertex-stream uploads. These can't be
// auto-generated FUNCTION(Script) methods because a member function can't take a Span<T>; the C# facade
// (LuminaSharp/World/DynamicMeshComponent.cs) declares matching [NativeCall] partials whose Span<T> args
// expand to (T*, int) and whose leading argument is the component Handle. The rest of the build API
// (AddSection/Commit/Clear/...) is reflected normally on the component. Game thread only.

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
