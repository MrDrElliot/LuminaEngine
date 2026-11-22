#pragma once
#include <filesystem>

#include <string>
#include <vector>


namespace Lumina::Reflection
{
    class FReflectedProject;

    // Post-parse validation uses these to enforce that a reflected header ends its includes with its generated.h.
    struct FIncludeRef
    {
        std::string Spelling;     // The literal text inside the angle/quote brackets.
        std::string Basename;     // Lowercased filename component, used for matching.
        uint32_t      LineNumber = 0;
    };

    class FReflectedHeader
    {
    public:

        FReflectedHeader(FReflectedProject* InProject, const std::string& Path);

        std::filesystem::file_time_type StartingFileTime;
        std::string                   FileName;
        std::string                   HeaderPath;
        FReflectedProject*              Project;

        // Captured via CXCursor_InclusionDirective, ordered by source line (the GeneratedHeaderNotLast check relies on it).
        std::vector<FIncludeRef>      Includes;

        // True when the macro visitor saw REFLECT/GENERATED_BODY/PROPERTY/FUNCTION; gates reflection-dependent validation.
        bool                            bHasReflectionMacros = false;

        bool                            bDirty = false;
    };
}
