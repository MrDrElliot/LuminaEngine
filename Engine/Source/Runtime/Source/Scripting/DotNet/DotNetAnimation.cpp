#include "Platform/GenericPlatform.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "World/Entity/Components/AnimationGraphComponent.h"
#include "Scripting/DotNet/DotNetExport.h"
#include "Scripting/DotNet/ExportSignature.h"
#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "Core/Object/Class.h"

// Only the graph parameter block stays here, since it hands C# a raw pointer no binding can express.

using namespace Lumina;
using namespace Lumina::DotNet;

// Name-checked rather than trusted, since the offsets C# reads come from its own mirror.
LUMINA_DOTNET_EXPORT(void*, AnimGraph_GetParameterMemory)(void* Component, const char* TypeName, int32 Length)
{
    auto* Comp = static_cast<SAnimationGraphComponent*>(Component);
    if (Comp == nullptr || TypeName == nullptr || !Comp->Graph.IsValid())
    {
        return nullptr;
    }

    CStruct* Struct = Comp->Graph->GetParameterStruct();
    if (Struct == nullptr || Struct->GetName() != FName(FStringView(TypeName, (size_t)Length)))
    {
        return nullptr;
    }

    return Comp->GetParameterMemory();
}

LUMINA_DOTNET_SIGNATURES(
    LUMINA_DOTNET_SIG(AnimGraph_GetParameterMemory)
);
