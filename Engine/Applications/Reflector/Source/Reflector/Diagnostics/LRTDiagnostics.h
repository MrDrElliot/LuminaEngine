#pragma once

#include <clang-c/Index.h>
#include <EASTL/string.h>
#include <cstdarg>
#include <cstdint>

namespace Lumina::Reflection
{
    // Stable, grep-able LRT diagnostic codes. Ranges: 1xxx property-type, 2xxx macro/declaration,
    // 3xxx function-signature (reserved), 9xxx driver/tool.
    enum class EDiagId : uint32_t
    {
        UnknownPropertyType    = 1000,  // PROPERTY field has a type the reflector cannot map.
        RawObjectPointer       = 1001,  // raw pointer to a CObject; caller must use TObjectPtr.
        ArrayElementUnknown    = 1002,  // element type of TVector<T> couldn't be resolved.
        OptionalElementUnknown = 1003,  // element type of TOptional<T> couldn't be resolved.
        FieldQualifyFailed     = 1004,  // clang couldn't qualify the field's type.
        FunctionFieldFailed    = 1005,  // function argument/return type couldn't be reflected.
        CircularHeaderInclude  = 1006,  // header A includes B (transitively) which includes A.
        UnreflectedPropertyType = 1007, // PROPERTY names a struct/class type that is not reflected.

        MissingGeneratedHeader = 2000,  // header has REFLECT/GENERATED_BODY/PROPERTY/FUNCTION but doesn't #include its <stem>.generated.h
        GeneratedHeaderNotLast = 2001,  // <stem>.generated.h is included but other includes follow it.
        MissingGeneratedBody   = 2002,  // class/struct uses REFLECT() but lacks a GENERATED_BODY() inside its body.
        WrongGeneratedHeader   = 2003,  // header includes a different file's .generated.h (copy-paste mistake).
        BadTypePrefix          = 2004,  // reflected class/struct/enum lacks the C-/S-/E- naming prefix.

        DriverMissingInput          = 9000,  // no JSON path on the command line.
        DriverInputUnreadable       = 9001,  // failed to open the JSON input file.
        DriverAmalgamationCreate    = 9002,  // couldn't create the amalgamation .gen.h.
        DriverClangParseFailure     = 9003,  // libclang returned a non-success CXErrorCode.
        DriverTranslationUnitWalk   = 9004,  // clang_visitChildren reported a problem.
    };

    struct FDiagLocation
    {
        eastl::string File;     // absolute path with forward slashes
        uint32_t      Line   = 0;
        uint32_t      Column = 0;
    };

    // Build a location from a clang cursor. Returns an empty File on failure.
    FDiagLocation MakeLocationFromCursor(const CXCursor& Cursor);

    // Singleton diagnostic sink; errors are formatted as MSBuild's `path(line,col): error LRTxxxx: message`
    // so they appear in the IDE problem list and count toward build failure.
    class FDiagnostics
    {
    public:

        static FDiagnostics& Get();

        void Errorf  (const FDiagLocation& Loc, EDiagId Id, const char* Fmt, ...);
        void Warningf(const FDiagLocation& Loc, EDiagId Id, const char* Fmt, ...);

        uint32_t GetErrorCount()   const { return ErrorCount; }
        uint32_t GetWarningCount() const { return WarningCount; }

        // Print a one-line summary like "[LRT] 3 error(s), 1 warning(s)".
        // No-op if both counters are zero.
        void PrintSummary() const;

    private:

        FDiagnostics() = default;

        void Emit(const char* Severity, const FDiagLocation& Loc, EDiagId Id, const char* Message);

        uint32_t ErrorCount   = 0;
        uint32_t WarningCount = 0;
    };
}

// Convenience macros so callsites stay short. Prefer these over hitting
// FDiagnostics::Get() directly.
#define LRT_ERROR(Cursor, Id, Fmt, ...) \
    ::Lumina::Reflection::FDiagnostics::Get().Errorf( \
        ::Lumina::Reflection::MakeLocationFromCursor(Cursor), (Id), Fmt, ##__VA_ARGS__)

#define LRT_WARNING(Cursor, Id, Fmt, ...) \
    ::Lumina::Reflection::FDiagnostics::Get().Warningf( \
        ::Lumina::Reflection::MakeLocationFromCursor(Cursor), (Id), Fmt, ##__VA_ARGS__)
