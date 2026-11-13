#pragma once

#include <vector>

#include "Reflector/Public/ReflectionModel.h"
#include "Reflector/Utils/MetadataUtils.h"

namespace Lumina
{
    class FReflectedProperty;
    class FReflectedFunction;
    struct FFieldInfo;
}

namespace Lumina::Reflection
{
    class FReflectedWorkspace;
    class FReflectionDatabase;

    // Flattens what the Clang front end parsed into the model every generator reads.
    class FReflectionModelBuilder
    {
    public:

        static FReflectionModel Build(const FReflectedWorkspace& Workspace, const FReflectionDatabase& Database);

    private:

        FStringId Intern(std::string_view Text);
        FIndexRange AddMetadata(const std::vector<FMetadataPair>& Metadata);
        FIndexRange AddStringList(const std::vector<std::string>& Strings);
        void AddProperty(const FReflectedProperty& Property, FStringId File);
        void AddArgument(const FFieldInfo& Field);
        void FillFunction(uint32_t Index, const FReflectedFunction& Function, FStringId File);

        FReflectionModel Model;
    };
}
