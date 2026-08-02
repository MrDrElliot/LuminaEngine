#pragma once
#include "Containers/Array.h"
#include "Containers/String.h"


enum class ERHIBindingResourceType : uint8;
enum class ERHIShaderType : uint8;

namespace Lumina
{
    struct FShaderBinding
    {
        FString Name;
        uint32 Set;
        uint32 Binding;
        uint32 Size;
        ERHIBindingResourceType Type;
    };

    struct FShaderReflection
    {
        ERHIShaderType ShaderType;
        TVector<FShaderBinding> Bindings;
    };
    
    struct FShaderHeader
    {
        FString             DebugName;
        TVector<FString>    Defines;
        TVector<uint32>     Binaries;
        uint64              Hash;
        FShaderReflection   Reflection;
    };
}
