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
	uint64_t GTranslationUnitsVisited;
	uint64_t GTranslationUnitsParsed;

	CXChildVisitResult VisitTranslationUnit(CXCursor Cursor, CXCursor Parent, CXClientData ClientData)
	{
		GTranslationUnitsVisited++;

		CXSourceLocation Loc = clang_getCursorLocation(Cursor);
		if (clang_Location_isInSystemHeader(Loc))
		{
			return CXChildVisit_Continue;
		}

		FClangParserContext* ParserContext = (FClangParserContext*)ClientData;

		std::string FilePath = ClangUtils::GetHeaderPathForCursor(Cursor);
		if (FilePath.empty())
		{
			return CXChildVisit_Continue;
		}

		FStringHash Hash(FilePath);
		auto Itr = ParserContext->AllHeaders.find(Hash);
		if (Itr == ParserContext->AllHeaders.end())
		{
			return CXChildVisit_Continue;
		}

		GTranslationUnitsParsed++;

		ParserContext->ReflectedHeader = Itr->second;

		CXCursorKind CursorKind = clang_getCursorKind(Cursor);
		std::string CursorName = ClangUtils::GetCursorDisplayName(Cursor);

		switch (CursorKind)
		{
		case (CXCursor_MacroExpansion):
		{
			return Visitor::VisitMacro(Cursor, Parent, ParserContext);
		}

		case (CXCursor_InclusionDirective):
		{
			// Capture the include directive so post-parse validation can enforce
			// that reflection-bearing headers include their generated companion last.
			FIncludeRef IncludeRef;
			IncludeRef.Spelling = CursorName;
			IncludeRef.Basename = CursorName;
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

			ParserContext->ReflectedHeader->Includes.push_back(std::move(IncludeRef));
			return CXChildVisit_Continue;
		}

		case(CXCursor_ClassDecl):
		{
			ParserContext->PushNamespace(CursorName);
			clang_visitChildren(Cursor, VisitTranslationUnit, ClientData);
			ParserContext->PopNamespace();

			return Visitor::VisitClass(Cursor, Parent, ParserContext);
		}

		case(CXCursor_StructDecl):
		{
			ParserContext->PushNamespace(CursorName);
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
			ParserContext->PushNamespace(CursorName);
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
			ParserContext->PushNamespace(CursorName);
			clang_visitChildren(Cursor, VisitTranslationUnit, ClientData);
			ParserContext->PopNamespace();
		}

		return CXChildVisit_Continue;

		default:
		{
			return CXChildVisit_Continue;
		}
		}
	}
}
