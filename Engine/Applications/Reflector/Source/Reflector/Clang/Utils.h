#pragma once
#include <clang/AST/Decl.h>
#include <clang/AST/Type.h>
#include <clang-c/CXFile.h>
#include <clang-c/CXSourceLocation.h>
#include <clang-c/CXString.h>
#include <clang-c/Index.h>
#include <EASTL/algorithm.h>
#include <EASTL/string.h>
#include <EASTL/vector.h>
#include <filesystem>
#include <system_error>
#include "xxhash.h"


namespace Lumina::ClangUtils
{
    // Canonicalize a path for the AllHeaders hash key (forward slashes, case preserved): the JSON
    // registration and parse-time cursor sides must agree byte-for-byte or types fail to register.
    inline eastl::string NormalizeHeaderPath(eastl::string Input)
    {
        if (Input.empty())
        {
            return Input;
        }

        std::error_code ErrorCode;
        std::filesystem::path Path(Input.c_str());
        std::filesystem::path Canonical = std::filesystem::weakly_canonical(Path, ErrorCode);
        if (ErrorCode)
        {
            Canonical = Path.lexically_normal();
        }

        eastl::string Result(Canonical.generic_string().c_str());
        eastl::replace(Result.begin(), Result.end(), '\\', '/');
        return Result;
    }

    inline eastl::string GetString(const CXString& string)
    {
        eastl::string str = clang_getCString(string);
        clang_disposeString(string);
        return str;
    }

    inline uint32_t GetCursorLineNumber(const CXCursor& Cr)
    {
        uint32_t Line = 0;
        uint32_t Column = 0;
        uint32_t Offset = 0;

        CXSourceLocation Location = clang_getCursorLocation(Cr);
        clang_getSpellingLocation(Location, nullptr, &Line, &Column, &Offset);

        return Line;
    }

    inline eastl::string StripNamespace(const eastl::string& Input)
    {
        size_t Pos = Input.rfind("::");
        if (Pos != eastl::string::npos)
        {
            return Input.substr(Pos + 2); // skip past the last "::"
        }
        return Input; // return unchanged if no "::" found
    }

    inline eastl::string MakeCodeFriendlyNamespace(eastl::string Input)
    {
        const eastl::string From = "::";
        const eastl::string To = "_";

        size_t StartPos = 0;
        while ((StartPos = Input.find(From, StartPos)) != eastl::string::npos)
        {
            Input.replace(StartPos, From.length(), To);
            StartPos += To.length();
        }

        return Input;
    }

    
    inline eastl::string GetCursorDisplayName(const CXCursor& cr)
    {
        CXString displayName = clang_getCursorDisplayName(cr);
        eastl::string str = clang_getCString(displayName);
        clang_disposeString(displayName);
        return str;
    }

    inline eastl::string GetCursorSpelling(const CXCursor& Cr)
    {
        CXString Spelling = clang_getCursorSpelling(Cr);
        eastl::string Result = clang_getCString(Spelling);
        clang_disposeString(Spelling);
        return Result;
    }

    inline eastl::string GetHeaderPathForCursor(const CXCursor& Cursor)
    {
        CXFile File = nullptr;
        const CXSourceRange CursorRange = clang_getCursorExtent(Cursor);
        clang_getExpansionLocation(clang_getRangeStart(CursorRange), &File, nullptr, nullptr, nullptr);

        eastl::string HeaderFilePath;
        if (File != nullptr)
        {
            CXString ClangFilePath = clang_getFileName(File);
            HeaderFilePath = eastl::string(clang_getCString(ClangFilePath));
            clang_disposeString(ClangFilePath);

            HeaderFilePath = NormalizeHeaderPath(eastl::move(HeaderFilePath));
        }

        return HeaderFilePath;
    }

    inline uint32_t GetLineNumberForCursor(const CXCursor& cr)
    {
        uint32_t line, column, offset;
        CXSourceRange range = clang_getCursorExtent(cr);
        CXSourceLocation start = clang_getRangeStart(range);
        clang_getExpansionLocation( start, nullptr, &line, &column, &offset);
        return line;
    }

    // True when libclang fell back to its location-derived placeholder (e.g. "(unnamed struct at ...)"),
    // which would leak the build path into generated identifiers and break consumers.
    inline bool IsLibclangPlaceholderName(const eastl::string& Name)
    {
        return Name.find("(unnamed ") != eastl::string::npos
            || Name.find("(anonymous ") != eastl::string::npos;
    }

    /// Strips an elaborated type specifier that libclang sometimes spells, leaving the bare
    /// qualified name the reflection database keys on.
    inline void StripElaboratedPrefix(eastl::string& Name)
    {
        for (const char* Prefix : { "struct ", "class ", "enum ", "union " })
        {
            const size_t PrefixLength = strlen(Prefix);
            if (Name.size() > PrefixLength && Name.compare(0, PrefixLength, Prefix) == 0)
            {
                Name.erase(0, PrefixLength);
                return;
            }
        }
    }

    /// Qualified name of a type as libclang spells it.
    ///
    /// libclang's spelling is the authoritative answer and needs no AST internals to obtain.
    inline bool GetSpelledTypeName(CXType Type, eastl::string& QualifiedName)
    {
        CXString Spelling = clang_getTypeSpelling(Type);
        const char* Text = clang_getCString(Spelling);

        QualifiedName = Text != nullptr ? Text : "";
        clang_disposeString(Spelling);

        StripElaboratedPrefix(QualifiedName);

        if (IsLibclangPlaceholderName(QualifiedName))
        {
            QualifiedName.clear();
            return false;
        }

        return !QualifiedName.empty();
    }

    /// The trailing component of a qualified name: "Lumina::Foo" -> "Foo".
    inline eastl::string UnqualifiedName(const eastl::string& Qualified)
    {
        const size_t Scope = Qualified.rfind("::");
        return Scope == eastl::string::npos ? Qualified : Qualified.substr(Scope + 2);
    }

    /// Splits a template argument list at top-level commas, so nested `<...>` stays intact.
    inline eastl::vector<eastl::string> SplitTemplateArguments(const eastl::string& Arguments)
    {
        eastl::vector<eastl::string> Result;
        int32_t Depth = 0;
        size_t Start = 0;

        for (size_t i = 0; i <= Arguments.size(); ++i)
        {
            const char C = i < Arguments.size() ? Arguments[i] : ',';
            if (C == '<' || C == '(')
            {
                ++Depth;
            }
            else if (C == '>' || C == ')')
            {
                --Depth;
            }
            else if (C == ',' && Depth == 0)
            {
                Result.push_back(Arguments.substr(Start, i - Start));
                Start = i + 1;
            }
        }

        return Result;
    }

    /// A C++ identifier built from a template argument: scopes dropped, punctuation folded to '_'.
    inline eastl::string MangleTemplateArgument(eastl::string Argument)
    {
        while (!Argument.empty() && (Argument.front() == ' ' || Argument.front() == '\t'))
        {
            Argument.erase(0, 1);
        }
        while (!Argument.empty() && (Argument.back() == ' ' || Argument.back() == '\t'))
        {
            Argument.pop_back();
        }

        StripElaboratedPrefix(Argument);

        eastl::string Result;
        eastl::string Segment;

        auto FlushSegment = [&]()
        {
            if (!Segment.empty())
            {
                if (!Result.empty())
                {
                    Result += '_';
                }
                Result += Segment;
                Segment.clear();
            }
        };

        for (size_t i = 0; i < Argument.size(); ++i)
        {
            const char C = Argument[i];
            if ((C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9') || C == '_')
            {
                Segment += C;
                continue;
            }

            // A scope resolution keeps only the trailing component, so Lumina::FVector3 mangles to FVector3.
            if (C == ':' && i + 1 < Argument.size() && Argument[i + 1] == ':')
            {
                Segment.clear();
                ++i;
                continue;
            }

            FlushSegment();
        }

        FlushSegment();
        return Result;
    }

    /// "Lumina::TVec<float, 3>" -> "TVec_float_3"; ResolveArgument renames an argument to its reflected name.
    template<typename TResolveArgument>
    inline eastl::string MangleTemplateSpelling(const eastl::string& Spelling, TResolveArgument&& ResolveArgument)
    {
        const size_t Open = Spelling.find('<');
        const size_t Close = Spelling.rfind('>');
        if (Open == eastl::string::npos || Close == eastl::string::npos || Close < Open)
        {
            return {};
        }

        eastl::string Result = UnqualifiedName(Spelling.substr(0, Open));

        for (eastl::string Argument : SplitTemplateArguments(Spelling.substr(Open + 1, Close - Open - 1)))
        {
            while (!Argument.empty() && Argument.front() == ' ')
            {
                Argument.erase(0, 1);
            }
            while (!Argument.empty() && Argument.back() == ' ')
            {
                Argument.pop_back();
            }

            const eastl::string Resolved = ResolveArgument(Argument);
            const eastl::string Mangled = MangleTemplateArgument(Resolved.empty() ? Argument : Resolved);
            if (!Mangled.empty())
            {
                Result += '_';
                Result += Mangled;
            }
        }

        return Result;
    }

    /// Rewrites the engine's container and core-type aliases onto their reflected spellings.
    inline void NormalizeEngineTypeName(eastl::string& Name)
    {
        struct FAlias { const char* From; const char* To; };
        static const FAlias Aliases[] =
        {
            { "eastl::vector",       "Lumina::TVector"    },
            { "eastl::hash_map",     "Lumina::THashMap"   },
            { "eastl::optional",     "Lumina::TOptional"  },
            { "eastl::basic_string", "Lumina::FString"    },
            // A fixed container differs from its growable counterpart only in where the storage lives, which
            // reflection never sees, so it reflects AS that counterpart instead of as a type of its own.
            { "eastl::fixed_string",   "Lumina::FString"  },
            { "eastl::fixed_vector",   "Lumina::TVector"  },
            { "eastl::fixed_hash_map", "Lumina::THashMap" },
            { "FString",             "Lumina::FString"    },
            { "FName",               "Lumina::FName"      },
            { "TObjectPtr",          "Lumina::TObjectPtr" },
            { "CObject",             "Lumina::CObject"    },
            { "CClass",              "Lumina::CClass"     },
        };

        for (const FAlias& Alias : Aliases)
        {
            if (Name == Alias.From)
            {
                Name = Alias.To;
                return;
            }
        }
    }

    /// Qualified name of a declaration, assembled from its semantic parents.
    ///
    /// Uses only libclang's C API, so it yields the bare qualified name ("eastl::vector") without
    /// template arguments, which is what the alias normalization and the reflection database key on.
    inline bool GetQualifiedNameFromDeclCursor(CXCursor Decl, eastl::string& QualifiedName)
    {
        if (clang_Cursor_isNull(Decl) || clang_isInvalid(clang_getCursorKind(Decl)))
        {
            return false;
        }

        CXString NameString = clang_getCursorSpelling(Decl);
        const char* NameText = clang_getCString(NameString);
        QualifiedName = NameText != nullptr ? NameText : "";
        clang_disposeString(NameString);

        if (QualifiedName.empty() || IsLibclangPlaceholderName(QualifiedName))
        {
            QualifiedName.clear();
            return false;
        }

        for (CXCursor Parent = clang_getCursorSemanticParent(Decl);
             !clang_Cursor_isNull(Parent);
             Parent = clang_getCursorSemanticParent(Parent))
        {
            const CXCursorKind ParentKind = clang_getCursorKind(Parent);
            if (ParentKind == CXCursor_TranslationUnit || clang_isInvalid(ParentKind))
            {
                break;
            }

            CXString ParentString = clang_getCursorSpelling(Parent);
            const char* ParentText = clang_getCString(ParentString);
            const eastl::string ParentName = ParentText != nullptr ? ParentText : "";
            clang_disposeString(ParentString);

            // An inline or anonymous namespace contributes no qualification.
            if (!ParentName.empty())
            {
                QualifiedName = ParentName + "::" + QualifiedName;
            }
        }

        return true;
    }

    /// The written type when libclang exposes one, else the canonical fallback. Keeps a typedef or
    /// alias spelling its own name instead of collapsing to whatever it resolves to.
    inline CXType PreferWritten(CXType Written, CXType Canonical)
    {
        return Written.kind != CXType_Invalid ? Written : Canonical;
    }

    /// Qualified, normalized name of a type, resolved entirely through libclang's C API.
    ///
    /// The Reflector links libclang for its C ABI but compiles against the LLVM headers, so
    /// reinterpreting CXType::data as a clang::QualType only works while the two agree exactly.
    /// Where they do not, the reinterpreted pointer is neither null nor valid: naming silently
    /// yields nothing and dereferencing it faults. Everything here goes through the stable API
    /// instead, so no result depends on clang's internal layout.
    inline bool GetQualifiedNameForCXType(CXType Type, eastl::string& QualifiedName)
    {
        QualifiedName.clear();

        const CXType Canonical = clang_getCanonicalType(Type);

        switch (Canonical.kind)
        {
        case CXType_Invalid:
        case CXType_Unexposed:
            // Fall back to the spelling; an unexposed type still spells correctly.
            return GetSpelledTypeName(Type, QualifiedName);

        case CXType_ConstantArray:
        case CXType_IncompleteArray:
        case CXType_VariableArray:
        case CXType_DependentSizedArray:
            return GetQualifiedNameForCXType(PreferWritten(clang_getArrayElementType(Type),
                clang_getArrayElementType(Canonical)), QualifiedName);

        case CXType_Pointer:
        case CXType_LValueReference:
        case CXType_RValueReference:
            // Recurse rather than spelling the whole thing: the spelling of a pointer to an
            // anonymous record bakes a build path into a generated identifier.
            //
            // Unwrap the written type, not the canonical one. The engine's math types are alias
            // templates, so canonicalizing "const FTransform&" first yields TTransform<float> and
            // the reflection database, which is keyed on FTransform, no longer recognizes it. A
            // by-value FTransform never hit this because it is named from the written type below.
            return GetQualifiedNameForCXType(PreferWritten(clang_getPointeeType(Type),
                clang_getPointeeType(Canonical)), QualifiedName);

        case CXType_Bool:       QualifiedName = "bool";   return true;
        case CXType_Char_S:     QualifiedName = "int8";   return true;
        case CXType_Char_U:     QualifiedName = "uint8";  return true;
        case CXType_UChar:      QualifiedName = "uint8";  return true;
        case CXType_SChar:      QualifiedName = "int8";   return true;
        case CXType_Char16:     QualifiedName = "uint16"; return true;
        case CXType_Char32:     QualifiedName = "uint32"; return true;
        case CXType_UShort:     QualifiedName = "uint16"; return true;
        case CXType_Short:      QualifiedName = "int16";  return true;
        case CXType_UInt:       QualifiedName = "uint32"; return true;
        case CXType_Int:        QualifiedName = "int32";  return true;
        case CXType_ULongLong:  QualifiedName = "uint64"; return true;
        case CXType_LongLong:   QualifiedName = "int64";  return true;
        case CXType_Float:      QualifiedName = "float";  return true;
        case CXType_Double:     QualifiedName = "double"; return true;

        default:
            break;
        }

        // Records and enums are named from their declaration, which gives the qualified name
        // without template arguments so the alias table below can match it.
        if (Canonical.kind == CXType_Record || Canonical.kind == CXType_Enum)
        {
            // Name from the written type first so a typedef or alias keeps its own name;
            // fall back to the canonical declaration when that yields nothing.
            if (!GetQualifiedNameFromDeclCursor(clang_getTypeDeclaration(Type), QualifiedName)
                && !GetQualifiedNameFromDeclCursor(clang_getTypeDeclaration(Canonical), QualifiedName))
            {
                return false;
            }

            NormalizeEngineTypeName(QualifiedName);
            return true;
        }

        if (!GetSpelledTypeName(Type, QualifiedName))
        {
            return false;
        }

        NormalizeEngineTypeName(QualifiedName);
        return true;
    }

    /// Qualified name of the type a declaration cursor declares, as libclang spells it.
    inline bool GetQualifiedNameForDeclCursor(CXCursor Cursor, eastl::string& QualifiedName)
    {
        CXString Spelling = clang_getTypeSpelling(clang_getCursorType(Cursor));
        const char* Text = clang_getCString(Spelling);

        QualifiedName = Text != nullptr ? Text : "";
        clang_disposeString(Spelling);

        StripElaboratedPrefix(QualifiedName);

        // Anonymous and locally declared types spell as "(unnamed struct at <path>)", which would
        // bake a build path into a generated identifier.
        if (IsLibclangPlaceholderName(QualifiedName))
        {
            QualifiedName.clear();
            return false;
        }

        return !QualifiedName.empty();
    }

    /// True when an integer type is unsigned.
    inline bool IsUnsignedIntegerType(CXType Type)
    {
        switch (clang_getCanonicalType(Type).kind)
        {
        case CXType_Bool:
        case CXType_Char_U:
        case CXType_UChar:
        case CXType_Char16:
        case CXType_Char32:
        case CXType_UShort:
        case CXType_UInt:
        case CXType_ULong:
        case CXType_ULongLong:
        case CXType_UInt128:
            return true;

        default:
            return false;
        }
    }

    /// Printable C++ type expression for casts in generated code, falling back to the semantic
    /// qualified name when libclang's printer would emit an "(unnamed ...)" placeholder.
    inline eastl::string GetSafeTypeAsString(CXType Type)
    {
        eastl::string Result;
        CXString Spelling = clang_getTypeSpelling(Type);
        const char* Text = clang_getCString(Spelling);
        Result = Text != nullptr ? Text : "";
        clang_disposeString(Spelling);

        if (!IsLibclangPlaceholderName(Result))
        {
            return Result;
        }

        eastl::string SemanticName;
        if (GetQualifiedNameForCXType(Type, SemanticName))
        {
            return SemanticName;
        }

        return Result;
    }


    inline uint64_t HashString(const eastl::string& str)
    {
        return XXH64(str.data(), strlen(str.c_str()), 0);
    }
}
