#include "ReflectedFunction.h"

namespace Lumina
{
    void FReflectedFunction::GenerateMetadata(const std::string& InMetadata)
    {
        if (InMetadata.empty())
        {
            return;
        }

        FMetadataParser Parser(InMetadata);
        Metadata = std::move(Parser.Metadata);
    }
}
