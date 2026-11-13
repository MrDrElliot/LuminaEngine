#pragma once

#include "Reflector/Public/ReflectionGenerator.h"

namespace Lumina::Reflection
{
    // Each header's .generated.h and .generated.cpp, the module table and the unity shards that compile them.
    class FCppReflectionGenerator final : public IReflectionGenerator
    {
    public:

        std::string_view GetName() const override { return "C++ reflection"; }
        void ClaimSpecifiers(FSpecifierRegistry& Registry) override;
        void GenerateModule(const FModuleContext& Context) override;
    };
}
