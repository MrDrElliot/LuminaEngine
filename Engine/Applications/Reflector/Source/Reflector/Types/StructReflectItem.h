#pragma once
#include "EASTL/string.h"


namespace Lumina::Reflection
{
    class FCodeWriter;
}

namespace Lumina
{
    class IStructReflectable
    {
    public:

        virtual ~IStructReflectable() = default;

    private:

        virtual void GenerateMetadata(const eastl::string& InMetadata) = 0;
        virtual bool GenerateLuaBinding(Reflection::FCodeWriter& Writer) { return false; }
    };
}
