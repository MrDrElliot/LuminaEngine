#pragma once

#include "Reflector/Public/ReflectionGenerator.h"

namespace Lumina::Reflection
{
    // The C# mirror of every reflected type and SCRIPT_EXPORT function, and the native thunks the mirrors call.
    class FCSharpBindingGenerator final : public IReflectionGenerator
    {
    public:

        std::string_view GetName() const override { return "C# bindings"; }
        void ClaimSpecifiers(FSpecifierRegistry& Registry) override;
        void GenerateModule(const FModuleContext& Context) override;
        void EndRun(const FRunContext& Context, FGeneratedOutputs& Outputs) override;
    };
}
