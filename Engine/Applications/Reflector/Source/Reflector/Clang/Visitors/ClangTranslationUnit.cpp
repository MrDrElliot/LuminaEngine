#include "Reflector/Utils/StringOps.h"
#include "ClangTranslationUnit.h"
#include "ClangVisitor.h"
#include "Reflector/Clang/ClangParserContext.h"
#include "Reflector/Clang/Utils.h"
#include "Reflector/Diagnostics/LRTDiagnostics.h"
#include <algorithm>
#include <string>
#include <StringHash.h>
#include <clang-c/CXSourceLocation.h>
#include <cstdint>

namespace Lumina::Reflection
{
	uint64_t GCursorsVisited;
	uint64_t GCursorsInReflectedHeaders;

	CXChildVisitResult VisitTranslationUnit(CXCursor Cursor, CXCursor Parent, CXClientData ClientData)
	{
		GCursorsVisited++;

		CXSourceLocation Loc = clang_getCursorLocation(Cursor);
		if (clang_Location_isInSystemHeader(Loc))
		{
			return CXChildVisit_Continue;
		}

		FClangParserContext* ParserContext = (FClangParserContext*)ClientData;

		FReflectedHeader* Header = ParserContext->ResolveHeaderForCursor(Cursor);
		if (Header == nullptr)
		{
			return CXChildVisit_Continue;
		}

		GCursorsInReflectedHeaders++;

		ParserContext->ReflectedHeader = Header;

		// Named lazily, since clang_getCursorDisplayName on a function builds its whole signature.
		switch (clang_getCursorKind(Cursor))
		{
		case (CXCursor_MacroExpansion):
		{
			return Visitor::VisitMacro(Cursor, Parent, ParserContext);
		}

		case (CXCursor_InclusionDirective):
		{
			// Captured so post-parse validation can enforce that the generated companion is included last.
			FIncludeRef IncludeRef;
			IncludeRef.Spelling = ClangUtils::GetCursorDisplayName(Cursor);
			IncludeRef.Basename = IncludeRef.Spelling;
			std::replace(IncludeRef.Basename.begin(), IncludeRef.Basename.end(), '\\', '/');
			const size_t SlashIdx = IncludeRef.Basename.find_last_of('/');
			if (SlashIdx != std::string::npos)
			{
				IncludeRef.Basename.erase(0, SlashIdx + 1);
			}
			Lumina::StringOps::ToLower(IncludeRef.Basename);

			uint32_t Line = 0;
			clang_getExpansionLocation(Loc, nullptr, &Line, nullptr, nullptr);
			IncludeRef.LineNumber = Line;

			Header->Includes.push_back(std::move(IncludeRef));
			return CXChildVisit_Continue;
		}

		case(CXCursor_ClassDecl):
		{
			ParserContext->PushNamespace(ClangUtils::CursorDisplayName(Cursor).View());
			clang_visitChildren(Cursor, VisitTranslationUnit, ClientData);
			ParserContext->PopNamespace();

			return Visitor::VisitClass(Cursor, Parent, ParserContext);
		}

		case(CXCursor_StructDecl):
		{
			ParserContext->PushNamespace(ClangUtils::CursorDisplayName(Cursor).View());
			clang_visitChildren(Cursor, VisitTranslationUnit, ClientData);
			ParserContext->PopNamespace();

			return Visitor::VisitStructure(Cursor, Parent, ParserContext);
		}

		case(CXCursor_TypeAliasDecl):
		case(CXCursor_TypedefDecl):
		{
			return Visitor::VisitTypeAlias(Cursor, Parent, ParserContext);
		}

		case(CXCursor_ClassTemplate):
		case(CXCursor_ClassTemplatePartialSpecialization):
		{
			ParserContext->PushNamespace(ClangUtils::CursorDisplayName(Cursor).View());
			clang_visitChildren(Cursor, VisitTranslationUnit, ClientData);
			ParserContext->PopNamespace();

			return Visitor::VisitClassTemplate(Cursor, Parent, ParserContext);
		}

		case(CXCursor_EnumDecl):
		{
			return Visitor::VisitEnum(Cursor, Parent, ParserContext);
		}

		case(CXCursor_FunctionDecl):
		{
			return Visitor::VisitFunction(Cursor, Parent, ParserContext);
		}

		case(CXCursor_Namespace):
		{
			ParserContext->PushNamespace(ClangUtils::CursorDisplayName(Cursor).View());
			clang_visitChildren(Cursor, VisitTranslationUnit, ClientData);
			ParserContext->PopNamespace();

			return CXChildVisit_Continue;
		}

		default:
		{
			return CXChildVisit_Continue;
		}
		}
	}
}
