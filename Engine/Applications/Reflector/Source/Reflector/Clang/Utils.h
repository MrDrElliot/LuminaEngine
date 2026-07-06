#pragma once
#include <clang/AST/Decl.h>
#include <clang/AST/Type.h>
#include <clang-c/CXFile.h>
#include <clang-c/CXSourceLocation.h>
#include <clang-c/CXString.h>
#include <clang-c/Index.h>
#include <EASTL/algorithm.h>
#include <EASTL/string.h>
#include <filesystem>
#include <system_error>
#include "xxhash.h"
#include "spdlog/spdlog.h"


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

    inline clang::QualType GetQualType(CXType type) 
    {
        return clang::QualType::getFromOpaquePtr(type.data[0]); 
    }
    
    // True when libclang fell back to its location-derived placeholder (e.g. "(unnamed struct at ...)"),
    // which would leak the build path into generated identifiers and break consumers.
    inline bool IsLibclangPlaceholderName(const eastl::string& Name)
    {
        return Name.find("(unnamed ") != eastl::string::npos
            || Name.find("(anonymous ") != eastl::string::npos;
    }

    inline bool GetQualifiedNameForType(clang::QualType Type, eastl::string& QualifiedName)
    {
        const clang::Type* pType = Type.getTypePtr();

        if (pType->isArrayType())
        {
            auto ElementType = pType->castAsArrayTypeUnsafe()->getElementType();
            if (!GetQualifiedNameForType(ElementType, QualifiedName))
            {
                return false;
            }
        }
        else if (pType->isBooleanType())
        {
            QualifiedName = "bool";
        }
        else if (pType->isBuiltinType())
        {
            const clang::BuiltinType* pBT = pType->getAs<clang::BuiltinType>();
            switch (pBT->getKind())
            {
                case clang::BuiltinType::Char_S:
                    QualifiedName = "int8";
                    break;

                case clang::BuiltinType::Char_U:
                    QualifiedName = "uint8";
                    break;

                case clang::BuiltinType::UChar:
                    QualifiedName = "uint8";
                    break;

                case clang::BuiltinType::SChar:
                    QualifiedName = "int8";
                    break;

                case clang::BuiltinType::Char16:
                    QualifiedName = "uint16";
                    break;

                case clang::BuiltinType::Char32:
                    QualifiedName = "uint32";
                    break;

                case clang::BuiltinType::UShort:
                    QualifiedName = "uint16";
                    break;

                case clang::BuiltinType::Short:
                    QualifiedName = "int16";
                    break;

                case clang::BuiltinType::UInt:
                    QualifiedName = "uint32";
                    break;

                case clang::BuiltinType::Int:
                    QualifiedName = "int32";
                    break;

                case clang::BuiltinType::ULongLong:
                    QualifiedName = "uint64";
                    break;

                case clang::BuiltinType::LongLong:
                    QualifiedName = "int64";
                    break;

                case clang::BuiltinType::Float:
                    QualifiedName = "float";
                    break;

                case clang::BuiltinType::Double:
                    QualifiedName = "double";
                    break;
                default:
                {
                    return false;
                }
            }
        }
        else if (pType->isPointerType())
        {
            // Recurse instead of getAsString(): the latter can emit "(unnamed struct at <path>)"
            // for anonymous records, baking the build path into generated identifiers.
            clang::QualType Pointee = pType->getAs<clang::PointerType>()->getPointeeType();
            if (!GetQualifiedNameForType(Pointee, QualifiedName))
            {
                return false;
            }
        }
        else if (pType->isReferenceType())
        {
            clang::QualType Pointee = pType->getAs<clang::ReferenceType>()->getPointeeType().getUnqualifiedType();
            if (!GetQualifiedNameForType(Pointee, QualifiedName))
            {
                return false;
            }
        }
        else if (pType->isRecordType())
        {
            if (pType->isTypedefNameType())
            {
                if (const auto* TypedefType = pType->getAs<clang::TypedefType>())
                {
                    const clang::TypedefNameDecl* Typedef = TypedefType->getDecl();
                    QualifiedName = Typedef->getQualifiedNameAsString().c_str();
                }
                else
                {
                    if (const auto* RecordType = pType->getAs<clang::RecordType>())
                    {
                        const clang::RecordDecl* Record = RecordType->getDecl();
                        QualifiedName = Record->getQualifiedNameAsString().c_str();
                    }
                }
            }
            else
            {
                if (const auto* RecordType = pType->getAs<clang::RecordType>())
                {
                    const clang::RecordDecl* Record = RecordType->getDecl();
                    QualifiedName = Record->getQualifiedNameAsString().c_str();
                }
            }
        }
        else if (pType->isEnumeralType())
        {
            const clang::NamedDecl* pNamedDecl = pType->getAs<clang::EnumType>()->getDecl();
            QualifiedName = pNamedDecl->getQualifiedNameAsString().c_str();
        }
        else if (pType->getTypeClass() == clang::Type::Typedef || pType->getTypeClass() == clang::Type::Using)
        {
            const clang::NamedDecl* pNamedDecl = pType->getAs<clang::TypedefType>()->getDecl();
            QualifiedName = pNamedDecl->getQualifiedNameAsString().c_str();
        }
        
        if (QualifiedName == "eastl::vector")
        {
            QualifiedName = "Lumina::TVector";
        }

        if (QualifiedName == "eastl::hash_map")
        {
            QualifiedName = "Lumina::THashMap";
        }

        if (QualifiedName == "eastl::optional")
        {
            QualifiedName = "Lumina::TOptional";
        }

        if (QualifiedName == "eastl::basic_string")
        {
            QualifiedName = "Lumina::FString";
        }
        
        if (QualifiedName == "eastl::fixed_string")
        {
            QualifiedName = "Lumina::FString";
        }

        if (QualifiedName == "FString")
        {
            QualifiedName = "Lumina::FString";
        }

        if (QualifiedName == "FName")
        {
            QualifiedName = "Lumina::FName";
        }
        
        if (QualifiedName == "TObjectPtr")
        {
            QualifiedName = "Lumina::TObjectPtr";
        }

        if (QualifiedName == "CObject")
        {
            QualifiedName = "Lumina::CObject";
        }

        if (QualifiedName == "CClass")
        {
            QualifiedName = "Lumina::CClass";
        }

        // Refuse libclang location-based placeholders; they produce broken
        // generated identifiers like "Construct_CStruct_(unnamed struct at ...)".
        if (IsLibclangPlaceholderName(QualifiedName))
        {
            QualifiedName.clear();
            return false;
        }

        return !QualifiedName.empty();
    }

    // Printable C++ type expression for casts in generated code; falls back to the
    // semantic qualified name when libclang's printer would emit an "(unnamed ...)" placeholder.
    inline eastl::string GetSafeTypeAsString(clang::QualType Type)
    {
        eastl::string Result = Type.getAsString().c_str();
        if (!IsLibclangPlaceholderName(Result))
        {
            return Result;
        }

        eastl::string SemanticName;
        if (GetQualifiedNameForType(Type, SemanticName))
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
