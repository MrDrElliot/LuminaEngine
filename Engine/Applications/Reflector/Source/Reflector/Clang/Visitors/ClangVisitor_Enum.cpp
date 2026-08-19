#include "ClangVisitor.h"
#include "Reflector/Clang/ClangParserContext.h"
#include "Reflector/Clang/Utils.h"
#include "Reflector/Diagnostics/LRTDiagnostics.h"
#include "Reflector/ReflectionSpecifiers.h"
#include "Reflector/Types/ReflectedType.h"

namespace Lumina::Reflection::Visitor
{
    static CXChildVisitResult VisitEnumContents(CXCursor Cursor, CXCursor, CXClientData pClientData)
    {
        FClangParserContext* pContext = static_cast<FClangParserContext*>(pClientData);
        FReflectedEnum* Enum = (FReflectedEnum*)pContext->ParentReflectedType;
        
        CXCursorKind kind = clang_getCursorKind(Cursor);
        
        if (kind == CXCursor_EnumConstantDecl)
        {
            eastl::string DisplayName = ClangUtils::GetCursorDisplayName(Cursor);

            // Read with the enum's own signedness. Reading an unsigned constant as signed
            // sign-extends it, so a flag like `All = 0xFFFF` on a uint16 enum would come back as
            // 0xFFFFFFFF and no longer fit the type the binding emitter gives it.
            const CXType Underlying = clang_getEnumDeclIntegerType(clang_getCursorSemanticParent(Cursor));

            const uint32_t Value = ClangUtils::IsUnsignedIntegerType(Underlying)
                ? (uint32_t)clang_getEnumConstantDeclUnsignedValue(Cursor)
                : (uint32_t)(int32_t)clang_getEnumConstantDeclValue(Cursor);

            FReflectedEnum::FConstant Constant;
            Constant.Label = DisplayName;
            Constant.ID = eastl::string(DisplayName);
            Constant.Value = Value;
            
            const CXString CommentString = clang_Cursor_getBriefCommentText(Cursor);
            if (CommentString.data != nullptr)
            {
                Constant.Description = clang_getCString(CommentString);
            }
            clang_disposeString(CommentString);

            Enum->AddConstant(Constant);
        }

        return CXChildVisit_Continue;
    }
    
    CXChildVisitResult VisitEnum(CXCursor Cursor, CXCursor, FClangParserContext* Context)
    {
        eastl::string CursorName = ClangUtils::GetCursorDisplayName(Cursor);

        void* Data = clang_getCursorType(Cursor).data[0];
        if(Data == nullptr)
        {
            return CXChildVisit_Continue;
        }

        FReflectionMacro Macro;
        if(!Context->TryFindMacroForCursor(Context->ReflectedHeader->HeaderPath, Cursor, Macro))
        {
            return CXChildVisit_Continue;
        }

        
        eastl::string FullyQualifiedName;
        if (!ClangUtils::GetQualifiedNameForDeclCursor(Cursor, FullyQualifiedName))
        {
            LRT_ERROR(Cursor, EDiagId::BadTypePrefix,
                "Reflected enum '%s' has no usable qualified name. Anonymous enums cannot be "
                "reflected; give it a name at namespace or class scope.",
                CursorName.c_str());
            return CXChildVisit_Continue;
        }
        
        if(Macro.Type != EReflectionMacro::Reflect)
        {
            return CXChildVisit_Continue;
        }

        // Naming convention: reflected enums are prefixed with `E`.
        if (CursorName.empty() || CursorName[0] != 'E')
        {
            LRT_WARNING(Cursor, EDiagId::BadTypePrefix,
                "Reflected enum '%s' should be prefixed with 'E' (e.g. 'E%s').",
                CursorName.c_str(), CursorName.c_str());
        }

        // Through the C API rather than by casting Cursor.data to a clang::EnumDecl: that cast is
        // only valid while the LLVM headers match the linked libclang exactly, and when they do
        // not it yields a pointer that is neither null nor safe to dereference.
        const CXType IntegerType = clang_getEnumDeclIntegerType(Cursor);

        if (IntegerType.kind == CXType_Invalid)
        {
            LRT_ERROR(Cursor, EDiagId::BadTypePrefix,
                "Reflected enum '%s' has no underlying integer type. Give it an explicit "
                "underlying type, for example `enum class %s : uint8`.",
                CursorName.c_str(), CursorName.c_str());
            return CXChildVisit_Continue;
        }
        
        FReflectedEnum* ReflectedEnum = Context->ReflectionDatabase.GetOrCreateReflectedType<FReflectedEnum>(FStringHash(FullyQualifiedName));
        ReflectedEnum->DisplayName = CursorName;
        ReflectedEnum->Header = Context->ReflectedHeader;
        ReflectedEnum->Type = FReflectedType::EType::Enum;
        ReflectedEnum->LineNumber = ClangUtils::GetCursorLineNumber(Cursor);
        ReflectedEnum->GenerateMetadata(Macro.MacroContents);
        ValidateSpecifiers(Cursor, ESpecifierTarget::Reflect, ReflectedEnum->Metadata);

        // Record the underlying integer size + signedness so the C# emitter can give the generated enum a
        // matching explicit backing type, letting it mirror by value inside a blittable struct at any width.
        const long long EnumSizeBytes = clang_Type_getSizeOf(IntegerType);
        ReflectedEnum->UnderlyingSize = (EnumSizeBytes > 0) ? (uint32_t)EnumSizeBytes : 4;
        ReflectedEnum->bUnsignedUnderlying = ClangUtils::IsUnsignedIntegerType(IntegerType);

        if (!Context->CurrentNamespace.empty())
        {
            ReflectedEnum->Namespace = Context->CurrentNamespace;
        }
        
        FReflectedType* PreviousParentType = Context->ParentReflectedType;
        
        Context->ParentReflectedType = ReflectedEnum;
        clang_visitChildren(Cursor, VisitEnumContents, Context);
        Context->ParentReflectedType = PreviousParentType;
        
        
        Context->ReflectionDatabase.AddReflectedType(ReflectedEnum);
        
        return CXChildVisit_Continue;

    }
}
