#include <clang/AST/Type.h>
#include <clang-c/CXString.h>
#include <clang-c/Index.h>
#include <EASTL/optional.h>
#include <EASTL/string.h>
#include <EASTL/vector.h>
#include <string>
#include <Reflector/Types/ReflectedType.h>
#include "Reflector/Clang/ClangParserContext.h"
#include "Reflector/Clang/Utils.h"
#include "Reflector/Diagnostics/LRTDiagnostics.h"
#include "Reflector/ReflectionCore/ReflectionMacro.h"
#include "Reflector/ReflectionSpecifiers.h"
#include "Reflector/Types/Functions/ReflectedFunction.h"
#include "Reflector/Types/Properties/ReflectedArrayProperty.h"
#include "Reflector/Types/Properties/ReflectedMapProperty.h"
#include "Reflector/Types/Properties/ReflectedClassProperty.h"
#include "Reflector/Types/Properties/ReflectedDelegateProperty.h"
#include "Reflector/Types/Properties/ReflectedEnumProperty.h"
#include "Reflector/Types/Properties/ReflectedNumericProperty.h"
#include "Reflector/Types/Properties/ReflectedObjectProperty.h"
#include "Reflector/Types/Properties/ReflectedOptionalProperty.h"
#include "Reflector/Types/Properties/ReflectedSoftObjectProperty.h"
#include "Reflector/Types/Properties/ReflectedStringProperty.h"
#include "Reflector/Types/Properties/ReflectedStructProperty.h"
#include "Reflector/Types/Properties/ReflectedInstancedStructProperty.h"
#include "Reflector/Types/Properties/ReflectedSubStructProperty.h"

namespace Lumina::Reflection::Visitor
{
	// Extract the brief doc comment above a cursor, escaping characters that would
	// break a C string literal in the generated output (\, ").
	static eastl::string GetCursorComment(const CXCursor& Cursor)
	{
		const CXString CommentString = clang_Cursor_getBriefCommentText(Cursor);
		eastl::string Result;
		if (CommentString.data != nullptr)
		{
			const char* Raw = clang_getCString(CommentString);
			if (Raw && Raw[0] != '\0')
			{
				for (const char* P = Raw; *P; ++P)
				{
					if (*P == '\\')
					{
						Result += "\\\\";
					}
					else if (*P == '"')
					{
						Result += "\\\"";
					}
					else
					{
						Result += *P;
					}
				}
			}
		}
		clang_disposeString(CommentString);
		return Result;
	}

	// The name a type registers under, from REFLECT(ReflectedName = "X"), or empty when unset.
	static eastl::string FindReflectedNameOverride(const eastl::string& MacroContents)
	{
		const size_t Key = MacroContents.find("ReflectedName");
		if (Key == eastl::string::npos)
		{
			return {};
		}

		const size_t Open = MacroContents.find('"', Key);
		if (Open == eastl::string::npos)
		{
			return {};
		}

		const size_t Close = MacroContents.find('"', Open + 1);
		if (Close == eastl::string::npos)
		{
			return {};
		}

		return MacroContents.substr(Open + 1, Close - Open - 1);
	}

	// Reflects a member as a different type, so a SIMD or otherwise unreflectable representation can
	// still present its logical shape. The offset stays the real member's, so no layout is hand-tracked.
	static void ApplyReflectAsOverride(FClangParserContext* Context, FReflectedType* Type,
	                                   FReflectedProperty* Property, const CXCursor& Cursor)
	{
		const eastl::string* Alias = nullptr;
		for (const FMetadataPair& Pair : Property->Metadata)
		{
			if (Pair.Key == "ReflectAs")
			{
				Alias = &Pair.Value;
				break;
			}
		}

		if (Alias == nullptr || Alias->empty())
		{
			return;
		}

		if (eastl::string_view(Property->GetTypeName()) != eastl::string_view("Struct"))
		{
			LRT_ERROR(Cursor, Reflection::EDiagId::UnknownPropertyType,
				"Property '%s' uses ReflectAs but its member is not a struct type; "
				"ReflectAs only reinterprets one struct layout as another.",
				Property->Name.c_str());
			return;
		}

		Property->TypeName = Alias->find("::") == eastl::string::npos
			? eastl::string("Lumina::") + *Alias
			: *Alias;
	}

	static void EnsureTemplateInstantiationReflected(FClangParserContext* Context, const FReflectedTemplate& Template,
	                                                 const CXType& Instantiation, const eastl::string& Spelling,
	                                                 const eastl::string& ReflectedQualifiedName, const eastl::string& MangledName);

	// Reflects the instantiation a property names, mangled because TRange<float> is not an identifier.
	static bool TryResolveTemplateInstantiation(FClangParserContext* Context, const CXType& FieldType, eastl::string& OutTypeName)
	{
		const CXType Canonical = clang_getCanonicalType(FieldType);
		if (Canonical.kind != CXType_Record)
		{
			return false;
		}

		const eastl::string Spelling = ClangUtils::GetString(clang_getTypeSpelling(Canonical));

		// A substituted template parameter carries no alias sugar, so FVector3 arrives as TVec<float, 3>.
		const auto Aliased = Context->AliasedInstantiations.find(FStringHash(Spelling));
		if (Aliased != Context->AliasedInstantiations.end())
		{
			OutTypeName = Aliased->second.first;
			return true;
		}

		const CXCursor TemplateCursor = clang_getSpecializedCursorTemplate(clang_getTypeDeclaration(Canonical));
		if (clang_Cursor_isNull(TemplateCursor))
		{
			return false;
		}

		eastl::string TemplateName;
		if (!ClangUtils::GetQualifiedNameFromDeclCursor(TemplateCursor, TemplateName))
		{
			return false;
		}

		const auto Found = Context->ReflectedTemplates.find(FStringHash(TemplateName));
		if (Found == Context->ReflectedTemplates.end())
		{
			return false;
		}

		const eastl::string Mangled = ClangUtils::MangleTemplateSpelling(Spelling,
			[Context](const eastl::string& Argument) -> eastl::string
			{
				const auto Match = Context->AliasedInstantiations.find(FStringHash(Argument));
				return Match == Context->AliasedInstantiations.end() ? eastl::string() : Match->second.second;
			});

		if (Mangled.empty())
		{
			return false;
		}

		const FReflectedTemplate& Template = Found->second;
		OutTypeName = Template.Namespace.empty() ? Mangled : Template.Namespace + "::" + Mangled;

		EnsureTemplateInstantiationReflected(Context, Template, Canonical, Spelling, OutTypeName, Mangled);
		return true;
	}

	static eastl::optional<FFieldInfo> CreateFieldInfo(FClangParserContext* Context, const CXCursor& Cursor)
	{
		eastl::string CursorName = ClangUtils::GetCursorDisplayName(Cursor);

		CXType FieldType = clang_getCursorType(Cursor);

		eastl::string TypeSpelling;
		if (!ClangUtils::GetQualifiedNameForCXType(FieldType, TypeSpelling))
		{
			LRT_ERROR(Cursor, Reflection::EDiagId::FieldQualifyFailed,
				"Failed to qualify the type of property '%s' in '%s'.",
				CursorName.c_str(),
				Context->ParentReflectedType->GetTypeName().c_str());
			return eastl::nullopt;
		}

		EPropertyTypeFlags PropFlags = GetCoreTypeFromName(TypeSpelling.c_str());

		// Is not a core type.
		if (PropFlags == EPropertyTypeFlags::None)
		{
			const CXTypeKind CanonicalKind = clang_getCanonicalType(FieldType).kind;

			if (CanonicalKind == CXType_Enum)
			{
				PropFlags = EPropertyTypeFlags::Enum;
			}
			else if (CanonicalKind == CXType_Record)
			{
				PropFlags = EPropertyTypeFlags::Struct;
				TryResolveTemplateInstantiation(Context, FieldType, TypeSpelling);
			}
			else if (CanonicalKind == CXType_Pointer)
			{
				LRT_ERROR(Cursor, Reflection::EDiagId::RawObjectPointer,
					"Property '%s' is a raw pointer ('%s'). Raw pointers to CObject are not reflectable; use TObjectPtr<T> instead.",
					CursorName.c_str(), TypeSpelling.c_str());
				return eastl::nullopt;
			}
		}
		
		FFieldInfo Info;
		
		if (clang_isConstQualifiedType(FieldType))
		{
			Info.PropertyFlags |= EPropertyFlags::Const;
		}

		switch (clang_getCXXAccessSpecifier(Cursor))
		{
			case CX_CXXPrivate:   Info.PropertyFlags |= EPropertyFlags::Private;   break;
			case CX_CXXProtected: Info.PropertyFlags |= EPropertyFlags::Protected; break;
			default: break;
		}
		
		if (clang_isPODType(FieldType))
		{
			Info.PropertyFlags |= EPropertyFlags::Trivial;
		}
		
		if (FieldType.kind >= CXType_FirstBuiltin && FieldType.kind <= CXType_LastBuiltin)
		{
			Info.PropertyFlags |= EPropertyFlags::Builtin;
		}
		
		Info.Flags			= PropFlags;
		Info.Type			= FieldType;
		Info.OwningCursor	= Cursor;
		Info.Name			= CursorName;
		Info.TypeName		= TypeSpelling;
		Info.RawFieldType	= ClangUtils::GetSafeTypeAsString(FieldType);

		return Info;
	}

	static eastl::optional<FFieldInfo> CreateFuncField(FClangParserContext* Context, const CXType& FieldType)
	{
		eastl::string TypeSpelling;
		if (!ClangUtils::GetQualifiedNameForCXType(FieldType, TypeSpelling))
		{
			return eastl::nullopt;
		}

		EPropertyTypeFlags PropFlags = GetCoreTypeFromName(TypeSpelling.c_str());

		// Is not a core type.
		if (PropFlags == EPropertyTypeFlags::None)
		{
			const CXTypeKind CanonicalKind = clang_getCanonicalType(FieldType).kind;

			if (CanonicalKind == CXType_Enum)
			{
				PropFlags = EPropertyTypeFlags::Enum;
			}
			else if (CanonicalKind == CXType_Record)
			{
				PropFlags = EPropertyTypeFlags::Struct;
			}
			else if (CanonicalKind == CXType_Pointer)
			{
				PropFlags = EPropertyTypeFlags::Object;
			}
			else if (CanonicalKind == CXType_LValueReference || CanonicalKind == CXType_RValueReference)
			{
				const CXType Referenced = clang_getPointeeType(clang_getCanonicalType(FieldType));

				if (clang_getCanonicalType(Referenced).kind == CXType_Record)
				{
					PropFlags = EPropertyTypeFlags::Struct;
				}
				else
				{
					ClangUtils::GetQualifiedNameForCXType(Referenced, TypeSpelling);
					PropFlags = GetCoreTypeFromName(TypeSpelling.c_str());
				}
			}
		}
		
		FFieldInfo Info;
		
		if (clang_isConstQualifiedType(FieldType))
		{
			Info.PropertyFlags |= EPropertyFlags::Const;
		}
		
		if (clang_isPODType(FieldType))
		{
			Info.PropertyFlags |= EPropertyFlags::Trivial;
		}
		
		if (FieldType.kind >= CXType_FirstBuiltin && FieldType.kind <= CXType_LastBuiltin)
		{
			Info.PropertyFlags |= EPropertyFlags::Builtin;
		}
		
		Info.Flags			= PropFlags;
		Info.Type			= FieldType;
		Info.Name			= "None";
		Info.TypeName		= TypeSpelling;
		Info.RawFieldType	= ClangUtils::GetSafeTypeAsString(FieldType);

		return Info;
	}

	static eastl::optional<FFieldInfo> CreateSubFieldInfo(FClangParserContext* Context, const CXType& FieldType, const FFieldInfo& ParentField)
	{
		eastl::string FieldName;
		if (!ClangUtils::GetQualifiedNameForCXType(FieldType, FieldName))
		{
			LRT_ERROR(ParentField.OwningCursor, Reflection::EDiagId::FieldQualifyFailed,
				"Failed to qualify the inner type of property '%s' in '%s'.",
				ParentField.Name.c_str(),
				Context->ParentReflectedType->GetTypeName().c_str());
			return eastl::nullopt;
		}

		EPropertyTypeFlags PropFlags = GetCoreTypeFromName(FieldName.c_str());

		// Is not a core type.
		if (PropFlags == EPropertyTypeFlags::None)
		{
			const CXTypeKind CanonicalKind = clang_getCanonicalType(FieldType).kind;

			if (CanonicalKind == CXType_Enum)
			{
				PropFlags = EPropertyTypeFlags::Enum;
			}
			else if (CanonicalKind == CXType_Record)
			{
				PropFlags = EPropertyTypeFlags::Struct;
				TryResolveTemplateInstantiation(Context, FieldType, FieldName);
			}
			else if (CanonicalKind == CXType_Pointer)
			{
				LRT_ERROR(ParentField.OwningCursor, Reflection::EDiagId::RawObjectPointer,
					"Inner element of property '%s' is a raw pointer ('%s'). Use TObjectPtr<T> instead.",
					ParentField.Name.c_str(), FieldName.c_str());
				return eastl::nullopt;
			}
		}

		FFieldInfo Info;
		
		if (clang_isConstQualifiedType(FieldType))
		{
			Info.PropertyFlags |= EPropertyFlags::Const;
		}
		
		if (clang_isPODType(FieldType))
		{
			Info.PropertyFlags |= EPropertyFlags::Trivial;
		}
		
		if (FieldType.kind >= CXType_FirstBuiltin && FieldType.kind <= CXType_LastBuiltin)
		{
			Info.PropertyFlags |= EPropertyFlags::Builtin;
		}
		
		Info.Flags			= PropFlags;
		Info.Type			= FieldType;
		Info.OwningCursor	= ParentField.OwningCursor;
		Info.TypeName		= FieldName;
		Info.RawFieldType	= ParentField.RawFieldType;
		return Info;
	}

	template<std::derived_from<FReflectedProperty> T>
	static eastl::unique_ptr<T> CreateProperty(const FFieldInfo& Info)
	{
		eastl::unique_ptr<T> New = eastl::make_unique<T>();
		New->Name			= Info.Name;
		New->TypeName		= Info.TypeName;
		New->RawTypeName	= Info.RawFieldType;
		New->PropertyFlags	= Info.PropertyFlags;
		return New;
	}

	static bool CreatePropertyForType(FClangParserContext* Context, FReflectedStruct* Struct, FReflectedProperty*& OutProperty, const FFieldInfo& FieldInfo)
	{
		OutProperty = nullptr;
		
		eastl::unique_ptr<FReflectedProperty> NewProperty;
		switch (FieldInfo.Flags)
		{
		case EPropertyTypeFlags::UInt8:
		{
			NewProperty = CreateProperty<FReflectedUInt8Property>(FieldInfo);
		}
		break;
		case EPropertyTypeFlags::UInt16:
		{
			NewProperty = CreateProperty<FReflectedUInt16Property>(FieldInfo);
		}
		break;
		case EPropertyTypeFlags::UInt32:
		{
			NewProperty = CreateProperty<FReflectedUInt32Property>(FieldInfo);
		}
		break;
		case EPropertyTypeFlags::UInt64:
		{
			NewProperty = CreateProperty<FReflectedUInt64Property>(FieldInfo);
		}
		break;
		case EPropertyTypeFlags::Int8:
		{
			NewProperty = CreateProperty<FReflectedInt8Property>(FieldInfo);
		}
		break;
		case EPropertyTypeFlags::Int16:
		{
			NewProperty = CreateProperty<FReflectedInt16Property>(FieldInfo);
		}
		break;
		case EPropertyTypeFlags::Int32:
		{
			NewProperty = CreateProperty<FReflectedInt32Property>(FieldInfo);
		}
		break;
		case EPropertyTypeFlags::Int64:
		{
			NewProperty = CreateProperty<FReflectedInt64Property>(FieldInfo);
		}
		break;
		case EPropertyTypeFlags::Float:
		{
			NewProperty = CreateProperty<FReflectedFloatProperty>(FieldInfo);
		}
		break;
		case EPropertyTypeFlags::Double:
		{
			NewProperty = CreateProperty<FReflectedDoubleProperty>(FieldInfo);
		}
		break;
		case EPropertyTypeFlags::Bool:
		{
			NewProperty = CreateProperty<FReflectedBoolProperty>(FieldInfo);
		}
		break;
		case EPropertyTypeFlags::String:
		{
			NewProperty = CreateProperty<FReflectedStringProperty>(FieldInfo);
		}
		break;
		case EPropertyTypeFlags::Name:
		{
			NewProperty = CreateProperty<FReflectedNameProperty>(FieldInfo);
		}
		break;
		case EPropertyTypeFlags::Struct:
		{
			NewProperty = CreateProperty<FReflectedStructProperty>(FieldInfo);
		}
		break;
		case EPropertyTypeFlags::Enum:
		{
			NewProperty = CreateProperty<FReflectedEnumProperty>(FieldInfo);
			const CXCursor EnumCursor = clang_getTypeDeclaration(FieldInfo.Type);

			if (clang_getCursorKind(EnumCursor) == CXCursor_EnumDecl)
			{
				CXType UnderlyingType = clang_getEnumDeclIntegerType(EnumCursor);
				eastl::optional<FFieldInfo> SubType = CreateSubFieldInfo(Context, UnderlyingType, FieldInfo);
				if (!SubType.has_value())
				{
					return false;
				}

				SubType->Name = FieldInfo.Name + "_Inner";
				SubType->PropertyFlags |= EPropertyFlags::SubField;

				FReflectedProperty* FieldProperty;
				CreatePropertyForType(Context, Struct, FieldProperty, SubType.value());
				FieldProperty->bInner = true;
			}
		}
		break;
		case EPropertyTypeFlags::Object:
		{
			const CXType ArgType = clang_Type_getTemplateArgumentAsType(FieldInfo.Type, 0);
			eastl::optional<FFieldInfo> ParamFieldInfo = CreateSubFieldInfo(Context, ArgType, FieldInfo);
			if (!ParamFieldInfo.has_value())
			{
				return false;
			}

			ParamFieldInfo->Name = FieldInfo.Name; // Replace the empty template property name with the parent.

			NewProperty = CreateProperty<FReflectedObjectProperty>(ParamFieldInfo.value());
		}
		break;
		case EPropertyTypeFlags::Class:
		{
			// TSubclassOf<T>: T is the base-class filter. Reflect against it so the emitted
			// Construct_CClass_<T>() symbol resolves.
			const CXType ArgType = clang_Type_getTemplateArgumentAsType(FieldInfo.Type, 0);
			eastl::optional<FFieldInfo> ParamFieldInfo;
			if (ArgType.kind != CXType_Invalid)
			{
				ParamFieldInfo = CreateSubFieldInfo(Context, ArgType, FieldInfo);
			}
			if (!ParamFieldInfo.has_value())
			{
				ParamFieldInfo = FieldInfo;
				ParamFieldInfo->TypeName = "Lumina::CObject";
			}

			ParamFieldInfo->Name = FieldInfo.Name;

			NewProperty = CreateProperty<FReflectedClassProperty>(ParamFieldInfo.value());
		}
		break;
		case EPropertyTypeFlags::SubStruct:
		{
			// TSubStructOf<T>: T is the base-struct filter; reflect against it for Construct_CStruct_<T>().
			const CXType ArgType = clang_Type_getTemplateArgumentAsType(FieldInfo.Type, 0);
			eastl::optional<FFieldInfo> ParamFieldInfo;
			if (ArgType.kind != CXType_Invalid)
			{
				ParamFieldInfo = CreateSubFieldInfo(Context, ArgType, FieldInfo);
			}
			if (!ParamFieldInfo.has_value())
			{
				return false;
			}

			ParamFieldInfo->Name = FieldInfo.Name;

			NewProperty = CreateProperty<FReflectedSubStructProperty>(ParamFieldInfo.value());
		}
		break;
		case EPropertyTypeFlags::InstancedStruct:
		{
			// A bare FInstancedStruct constrains nothing, so it emits a null base and takes any struct.
			const CXType ArgType = clang_Type_getTemplateArgumentAsType(FieldInfo.Type, 0);
			eastl::optional<FFieldInfo> ParamFieldInfo;
			if (ArgType.kind != CXType_Invalid)
			{
				ParamFieldInfo = CreateSubFieldInfo(Context, ArgType, FieldInfo);
			}
			if (!ParamFieldInfo.has_value())
			{
				ParamFieldInfo = FieldInfo;
				ParamFieldInfo->TypeName.clear();
			}

			ParamFieldInfo->Name = FieldInfo.Name;
			// FInstancedStruct owns heap memory, so never bulk-copy it as POD (the meta-struct's
			// POD-ness would otherwise leak onto the owning property).
			ParamFieldInfo->PropertyFlags &= ~EPropertyFlags::Trivial;

			NewProperty = CreateProperty<FReflectedInstancedStructProperty>(ParamFieldInfo.value());
		}
		break;
		case EPropertyTypeFlags::SoftObject:
		{
			// FSoftObjectPath has no template arg (target defaults to CObject, accepts any asset);
			// TSoftObjectPtr<T> exposes T as the target class.
			const CXType ArgType = clang_Type_getTemplateArgumentAsType(FieldInfo.Type, 0);
			eastl::optional<FFieldInfo> ParamFieldInfo;
			if (ArgType.kind != CXType_Invalid)
			{
				ParamFieldInfo = CreateSubFieldInfo(Context, ArgType, FieldInfo);
			}
			if (!ParamFieldInfo.has_value())
			{
				// Bare FSoftObjectPath: reflect against CObject so the emitted
				// Construct_CClass_<T>() symbol resolves instead of a nonexistent one.
				ParamFieldInfo = FieldInfo;
				ParamFieldInfo->TypeName = "Lumina::CObject";
			}

			ParamFieldInfo->Name = FieldInfo.Name;

			NewProperty = CreateProperty<FReflectedSoftObjectProperty>(ParamFieldInfo.value());
		}
		break;
		case EPropertyTypeFlags::Vector:
		{
			auto ArrayProperty = CreateProperty<FReflectedArrayProperty>(FieldInfo);

			const CXType ArgType = clang_Type_getTemplateArgumentAsType(FieldInfo.Type, 0);
			eastl::optional<FFieldInfo> ParamFieldInfo = CreateSubFieldInfo(Context, ArgType, FieldInfo);
			if (!ParamFieldInfo.has_value())
			{
				return false;
			}

			ParamFieldInfo->Name = FieldInfo.Name + "_Inner";
			ParamFieldInfo->PropertyFlags |= EPropertyFlags::SubField;

			FReflectedProperty* FieldProperty;
			CreatePropertyForType(Context, Struct, FieldProperty, ParamFieldInfo.value());
			if (FieldProperty == nullptr)
			{
				LRT_ERROR(FieldInfo.OwningCursor, Reflection::EDiagId::ArrayElementUnknown,
					"Array property '%s' has element type '%s' which is not reflectable.",
					FieldInfo.Name.c_str(), ParamFieldInfo->TypeName.c_str());
				return false;
			}

			ArrayProperty->ElementTypeName = ClangUtils::GetSafeTypeAsString(ArgType);
			NewProperty = eastl::move(ArrayProperty);

			FieldProperty->bInner = true; // This property "belongs" to the array.
		}
		break;
		case EPropertyTypeFlags::Map:
		{
			auto MapProperty = CreateProperty<FReflectedMapProperty>(FieldInfo);

			// THashMap<K, V, Hash, Equal, Alloc>: args 0 and 1 are the key and value; the rest are ignored.
			if (clang_Type_getNumTemplateArguments(FieldInfo.Type) < 2)
			{
				LRT_ERROR(FieldInfo.OwningCursor, Reflection::EDiagId::ArrayElementUnknown,
					"Map property '%s' is missing its key/value template arguments.", FieldInfo.Name.c_str());
				return false;
			}

			const CXType KeyType   = clang_Type_getTemplateArgumentAsType(FieldInfo.Type, 0);
			const CXType ValueType = clang_Type_getTemplateArgumentAsType(FieldInfo.Type, 1);

			eastl::optional<FFieldInfo> KeyInfo   = CreateSubFieldInfo(Context, KeyType, FieldInfo);
			eastl::optional<FFieldInfo> ValueInfo = CreateSubFieldInfo(Context, ValueType, FieldInfo);
			if (!KeyInfo.has_value() || !ValueInfo.has_value())
			{
				return false;
			}

			KeyInfo->Name = FieldInfo.Name + "_KeyInner";
			KeyInfo->PropertyFlags |= EPropertyFlags::SubField;
			ValueInfo->Name = FieldInfo.Name + "_ValueInner";
			ValueInfo->PropertyFlags |= EPropertyFlags::SubField;

			// Push the VALUE inner first, then the KEY inner, so Struct->Props ends up [Value, Key, Map]. The
			// runtime constructs the map (ReadMore = 2) and walks backward, attaching Key then Value -- matching
			// FMapProperty::AddProperty (Key first, Value second). This order is the ABI contract; do not swap.
			FReflectedProperty* ValueProperty = nullptr;
			CreatePropertyForType(Context, Struct, ValueProperty, ValueInfo.value());
			FReflectedProperty* KeyProperty = nullptr;
			CreatePropertyForType(Context, Struct, KeyProperty, KeyInfo.value());
			if (KeyProperty == nullptr || ValueProperty == nullptr)
			{
				LRT_ERROR(FieldInfo.OwningCursor, Reflection::EDiagId::ArrayElementUnknown,
					"Map property '%s' has a key or value type ('%s' / '%s') which is not reflectable.",
					FieldInfo.Name.c_str(), KeyInfo->TypeName.c_str(), ValueInfo->TypeName.c_str());
				return false;
			}

			MapProperty->KeyTypeName   = ClangUtils::GetSafeTypeAsString(KeyType);
			MapProperty->ValueTypeName = ClangUtils::GetSafeTypeAsString(ValueType);
			NewProperty = eastl::move(MapProperty);

			KeyProperty->bInner = true;   // Key "belongs" to the map.
			ValueProperty->bInner = true; // Value "belongs" to the map.
		}
		break;
		case EPropertyTypeFlags::Optional:
		{
			auto OptionalProperty = CreateProperty<FReflectedOptionalProperty>(FieldInfo);

			// TOptional<T> exposes T as the first template argument, same shape
			// as TVector<T>. Fail loudly when the payload type isn't reflectable.
			const CXType ArgType = clang_Type_getTemplateArgumentAsType(FieldInfo.Type, 0);
			eastl::optional<FFieldInfo> ParamFieldInfo = CreateSubFieldInfo(Context, ArgType, FieldInfo);
			if (!ParamFieldInfo.has_value())
			{
				return false;
			}

			ParamFieldInfo->Name = FieldInfo.Name + "_Inner";
			ParamFieldInfo->PropertyFlags |= EPropertyFlags::SubField;

			FReflectedProperty* FieldProperty;
			CreatePropertyForType(Context, Struct, FieldProperty, ParamFieldInfo.value());
			if (FieldProperty == nullptr)
			{
				LRT_ERROR(FieldInfo.OwningCursor, Reflection::EDiagId::OptionalElementUnknown,
					"Optional property '%s' has payload type '%s' which is not reflectable.",
					FieldInfo.Name.c_str(), ParamFieldInfo->TypeName.c_str());
				return false;
			}

			OptionalProperty->ElementTypeName = ClangUtils::GetSafeTypeAsString(ArgType);
			NewProperty = eastl::move(OptionalProperty);

			FieldProperty->bInner = true; // Inner T is owned by the optional.
		}
		break;
		case EPropertyTypeFlags::Delegate:
		{
			const CXType ArgType = clang_Type_getTemplateArgumentAsType(FieldInfo.Type, 0);
			if (ArgType.kind != CXType_Invalid && ArgType.kind != CXType_Void)
			{
				eastl::optional<FFieldInfo> ParamFieldInfo = CreateSubFieldInfo(Context, ArgType, FieldInfo);
				if (!ParamFieldInfo.has_value())
				{
					return false;
				}

				ParamFieldInfo->Name = FieldInfo.Name;

				auto DelegateProperty = CreateProperty<FReflectedDelegateProperty>(ParamFieldInfo.value());
				DelegateProperty->bHasPayload = true;
				NewProperty = eastl::move(DelegateProperty);
			}
			else
			{
				auto DelegateProperty = CreateProperty<FReflectedDelegateProperty>(FieldInfo);
				DelegateProperty->bHasPayload = false;
				NewProperty = eastl::move(DelegateProperty);
			}
		}
		break;
		default:
		{
			// Catch-all for fields that slipped past every classifier; erroring here
			// prevents silently dropping the property from the reflection database.
			LRT_ERROR(FieldInfo.OwningCursor, Reflection::EDiagId::UnknownPropertyType,
				"Property '%s' has type '%s' which is not supported by the reflector. "
				"Supported kinds: numeric, bool, FString/FName, enum, struct (REFLECT'd), "
				"TObjectPtr<T>, TVector<T>, TOptional<T>.",
				FieldInfo.Name.c_str(), FieldInfo.TypeName.c_str());
		}
		break;
		}

		if (NewProperty != nullptr)
		{
			OutProperty = NewProperty.get();
			Struct->PushProperty(eastl::move(NewProperty));
		}

		return OutProperty != nullptr;
	}


	static bool CreateFunctionForType(const CXCursor& Cursor, FClangParserContext* Context, FReflectedStruct* Struct, FReflectedFunction*& OutFunction)
	{
		OutFunction = nullptr;
		
		auto NewFunction = eastl::make_unique<FReflectedFunction>();
		NewFunction->Outer = Struct->DisplayName;
		NewFunction->Name = ClangUtils::GetCursorSpelling(Cursor);
		NewFunction->bIsVirtual = clang_CXXMethod_isVirtual(Cursor) != 0;

		int NumArgs = clang_Cursor_getNumArguments(Cursor);

		for (int i = 0; i < NumArgs; ++i)
		{
			CXCursor ArgCursor = clang_Cursor_getArgument(Cursor, i);
			eastl::string ArgName = ClangUtils::GetCursorSpelling(ArgCursor);
			CXType FieldType = clang_getCursorType(ArgCursor);
			auto Field = CreateFuncField(Context, FieldType);
			if (Field.has_value() && Field->Flags != EPropertyTypeFlags::None)
			{
				Field->OwningCursor = ArgCursor;
				Field->Name			= eastl::move(ArgName);
				NewFunction->AddArgument(eastl::move(Field.value()));
			}
			else
			{
				// Soft-fail: a missing arg only affects the Lua binding shape, not
				// memory layout (C++ still links). Warn so it shows in the build log.
				// Flag the function so the C# binder skips it - its reflected arg list is now shorter
				// than the real signature, so a generated thunk would call with too few arguments.
				NewFunction->bHasOmittedArgs = true;
				LRT_WARNING(ArgCursor, Reflection::EDiagId::FunctionFieldFailed,
					"Argument '%s' of function '%s' has an unsupported type and will be omitted from the script binding. Reflected function args accept core types, structs, enums, and TObjectPtr<T>.",
					ArgName.c_str(), NewFunction->Name.c_str());
			}
		}
		
		CXType FuncType = clang_getCursorType(Cursor);
		CXType ResultType = clang_getResultType(FuncType);
		
		if (ResultType.kind != CXType_Void)
		{
			NewFunction->Return = CreateFuncField(Context, ResultType);
		}
		
		OutFunction = NewFunction.get();
		Struct->PushFunction(eastl::move(NewFunction));

		return NewFunction != nullptr;
	}

	static void ReflectField(FClangParserContext* Context, FReflectedStruct* Struct, const CXCursor& Cursor,
	                         const eastl::string& MacroHeader, bool bConsumeMacro)
	{
		FReflectionMacro Macro;
		if (!Context->TryFindMacroForCursor(MacroHeader, Cursor, Macro, bConsumeMacro))
		{
			// Data the reflector cannot see, so the C# emitter must not mirror this type by value.
			Struct->bHasUnreflectedFields |= !Context->bInAnonymousRecord;
			return;
		}

		eastl::optional<FFieldInfo> FieldInfo = CreateFieldInfo(Context, Cursor);
		if (!FieldInfo.has_value())
		{
			return;
		}

		FReflectedProperty* NewProperty;
		if (!CreatePropertyForType(Context, Struct, NewProperty, FieldInfo.value()))
		{
			return;
		}

		NewProperty->GenerateMetadata(Macro.MacroContents);
		ValidateSpecifiers(Cursor, ESpecifierTarget::Property, NewProperty->Metadata);
		ApplyReflectAsOverride(Context, Struct, NewProperty, Cursor);

		if (eastl::string ConflictMessage; NewProperty->FindConflictingSpecifiers(ConflictMessage))
		{
			LRT_ERROR(Cursor, EDiagId::ConflictingSpecifiers, "%s", ConflictMessage.c_str());
		}

		eastl::string Comment = GetCursorComment(Cursor);
		if (!Comment.empty())
		{
			NewProperty->Metadata.push_back({"ToolTip", eastl::move(Comment)});
		}
	}

	// An implicitly instantiated specialization exposes no children, so its members come from the type.
	static CXVisitorResult VisitAliasField(CXCursor Cursor, CXClientData pClientData)
	{
		FClangParserContext* Context = (FClangParserContext*)pClientData;
		const CXType FieldType = clang_getCursorType(Cursor);

		if (clang_Cursor_isAnonymousRecordDecl(clang_getTypeDeclaration(FieldType)) != 0)
		{
			const bool bWasInAnonymousRecord = Context->bInAnonymousRecord;
			Context->bInAnonymousRecord = true;
			clang_Type_visitFields(FieldType, VisitAliasField, pClientData);
			Context->bInAnonymousRecord = bWasInAnonymousRecord;
			return CXVisit_Continue;
		}

		ReflectField(Context, Context->GetParentReflectedType<FReflectedStruct>(), Cursor,
			Context->AliasTargetMacroHeader, false);

		return CXVisit_Continue;
	}

	static void EnsureTemplateInstantiationReflected(FClangParserContext* Context, const FReflectedTemplate& Template,
	                                                 const CXType& Instantiation, const eastl::string& Spelling,
	                                                 const eastl::string& ReflectedQualifiedName, const eastl::string& MangledName)
	{
		if (Context->ReflectionDatabase.GetReflectedType<FReflectedType>(FStringHash(ReflectedQualifiedName)) != nullptr)
		{
			return;
		}

		FReflectedStruct* ReflectedStruct = Context->ReflectionDatabase.GetOrCreateReflectedType<FReflectedStruct>(FStringHash(ReflectedQualifiedName));
		ReflectedStruct->DisplayName = MangledName;
		ReflectedStruct->CppQualifiedName = Spelling;
		ReflectedStruct->Namespace = Template.Namespace;
		ReflectedStruct->bIsAlias = true;
		ReflectedStruct->GenerateMetadata(Template.MacroContents);
		ReflectedStruct->Header = Template.Header;
		ReflectedStruct->Type = FReflectedType::EType::Structure;

		// Registered before its fields are walked, so an instantiation that reaches itself terminates.
		Context->ReflectionDatabase.AddReflectedType(ReflectedStruct);

		FReflectedType* PreviousType = Context->ParentReflectedType;
		const eastl::string PreviousMacroHeader = Context->AliasTargetMacroHeader;
		const bool bWasInAnonymousRecord = Context->bInAnonymousRecord;

		Context->ParentReflectedType = ReflectedStruct;
		Context->AliasTargetMacroHeader = Template.HeaderPath;
		Context->bInAnonymousRecord = false;

		clang_Type_visitFields(Instantiation, VisitAliasField, Context);

		Context->bInAnonymousRecord = bWasInAnonymousRecord;
		Context->AliasTargetMacroHeader = PreviousMacroHeader;
		Context->ParentReflectedType = PreviousType;
	}

	template<typename TVisitType>
	static CXChildVisitResult VisitContents(CXCursor Cursor, CXCursor Parent, CXClientData pClientData)
	{
		FClangParserContext* Context = (FClangParserContext*)pClientData;
		eastl::string CursorName = ClangUtils::GetCursorDisplayName(Cursor);
		CXCursorKind Kind = clang_getCursorKind(Cursor);
		TVisitType* Type = Context->GetParentReflectedType<TVisitType>();
		
		const bool bWalkingAliasTarget = !Context->AliasTargetMacroHeader.empty();
		const eastl::string& MacroHeader = bWalkingAliasTarget
			? Context->AliasTargetMacroHeader
			: Context->ReflectedHeader->HeaderPath;

		switch (Kind)
		{
		case(CXCursor_CXXBaseSpecifier):
		{
			if (Type->Parent.empty())
			{
				Type->Parent = CursorName;
			}
		}
		break;
		case(CXCursor_StructDecl):
		case(CXCursor_UnionDecl):
		{
			// Only an anonymous record contributes its fields to the enclosing layout.
			if (clang_Cursor_isAnonymousRecordDecl(Cursor) == 0)
			{
				break;
			}

			const bool bWasInAnonymousRecord = Context->bInAnonymousRecord;
			Context->bInAnonymousRecord = true;
			clang_visitChildren(Cursor, VisitContents<TVisitType>, Context);
			Context->bInAnonymousRecord = bWasInAnonymousRecord;
		}
		break;
		case(CXCursor_FieldDecl):
		{
			ReflectField(Context, Type, Cursor, MacroHeader, !bWalkingAliasTarget);
		}
		break;
		case(CXCursor_CXXMethod):
		{
			FReflectionMacro Macro;
			if (!Context->TryFindMacroForCursor(MacroHeader, Cursor, Macro, !bWalkingAliasTarget))
			{
				return CXChildVisit_Continue;
			}

			FReflectedFunction* NewFunction;
			CreateFunctionForType(Cursor, Context, Type, NewFunction);
			NewFunction->GenerateMetadata(Macro.MacroContents);
			ValidateSpecifiers(Cursor, ESpecifierTarget::Function, NewFunction->Metadata);

			eastl::string Comment = GetCursorComment(Cursor);
			if (!Comment.empty())
			{
				NewFunction->Metadata.push_back({"ToolTip", eastl::move(Comment)});
			}
		}
		break;

		// Every other cursor kind in a struct body is not reflected surface.
		default:
			break;
		}

		return CXChildVisit_Continue;

	}

	CXChildVisitResult VisitClassTemplate(CXCursor Cursor, CXCursor Parent, FClangParserContext* Context)
	{
		FReflectionMacro Macro;
		if (!Context->TryFindMacroForCursor(Context->ReflectedHeader->HeaderPath, Cursor, Macro))
		{
			return CXChildVisit_Continue;
		}

		if (Macro.Type != EReflectionMacro::Reflect)
		{
			Context->AddReflectedMacro(eastl::move(Macro));
			return CXChildVisit_Continue;
		}

		eastl::string QualifiedName;
		if (!ClangUtils::GetQualifiedNameFromDeclCursor(Cursor, QualifiedName))
		{
			LRT_ERROR(Cursor, EDiagId::ReflectedAliasInvalid,
				"REFLECT'd class template '%s' has no usable qualified name.",
				ClangUtils::GetCursorDisplayName(Cursor).c_str());
			return CXChildVisit_Continue;
		}

		FReflectedTemplate Template;
		Template.QualifiedName = QualifiedName;
		Template.Namespace = Context->CurrentNamespace;
		Template.MacroContents = Macro.MacroContents;
		ValidateSpecifiers(Cursor, ESpecifierTarget::Reflect, FMetadataParser(Macro.MacroContents).Metadata);
		Template.HeaderPath = Context->ReflectedHeader->HeaderPath;
		Template.Header = Context->ReflectedHeader;

		Context->ReflectedTemplates.insert_or_assign(FStringHash(QualifiedName), eastl::move(Template));

		return CXChildVisit_Continue;
	}

	// A REFLECT'd `using X = SomeRecord;`, which reflects the aliased record's real members under X.
	CXChildVisitResult VisitTypeAlias(CXCursor Cursor, CXCursor Parent, FClangParserContext* Context)
	{
		// Checked before touching the macro pool: a member alias must not consume its owner's REFLECT.
		const CXCursorKind ParentKind = clang_getCursorKind(clang_getCursorSemanticParent(Cursor));
		if (ParentKind != CXCursor_Namespace && ParentKind != CXCursor_TranslationUnit)
		{
			return CXChildVisit_Continue;
		}

		FReflectionMacro Macro;
		if (!Context->TryFindMacroForCursor(Context->ReflectedHeader->HeaderPath, Cursor, Macro))
		{
			return CXChildVisit_Continue;
		}

		if (Macro.Type != EReflectionMacro::Reflect)
		{
			// Somebody else's macro that happens to sit above this alias; hand it back to its owner.
			Context->AddReflectedMacro(eastl::move(Macro));
			return CXChildVisit_Continue;
		}

		const eastl::string AliasName = ClangUtils::GetCursorDisplayName(Cursor);

		eastl::string QualifiedAliasName;
		if (!ClangUtils::GetQualifiedNameForDeclCursor(Cursor, QualifiedAliasName))
		{
			LRT_ERROR(Cursor, EDiagId::ReflectedAliasInvalid,
				"REFLECT'd alias '%s' has no usable qualified name. Declare it at namespace scope.",
				AliasName.c_str());
			return CXChildVisit_Continue;
		}

		const CXType Target = clang_getCanonicalType(clang_getTypedefDeclUnderlyingType(Cursor));
		const CXCursor TargetCursor = clang_getTypeDeclaration(Target);
		if (Target.kind != CXType_Record || clang_Cursor_isNull(TargetCursor))
		{
			LRT_ERROR(Cursor, EDiagId::ReflectedAliasInvalid,
				"REFLECT'd alias '%s' does not name a struct or class. Only record types can be reflected.",
				AliasName.c_str());
			return CXChildVisit_Continue;
		}

		const eastl::string TargetHeader = ClangUtils::GetHeaderPathForCursor(TargetCursor);
		if (TargetHeader.empty())
		{
			LRT_ERROR(Cursor, EDiagId::ReflectedAliasInvalid,
				"REFLECT'd alias '%s' resolves to a type with no source location.", AliasName.c_str());
			return CXChildVisit_Continue;
		}

		// An alias never requires its target to be complete, so an otherwise unused template is uninstantiated.
		if (clang_Type_getSizeOf(Target) == CXTypeLayoutError_Incomplete)
		{
			LRT_ERROR(Cursor, EDiagId::AliasNotInstantiated,
				"REFLECT'd alias '%s' names a type that is never instantiated, so the reflector cannot see "
				"its members. Add `static_assert(sizeof(%s) > 0);` after the alias, or use the type somewhere "
				"that requires it to be complete.",
				AliasName.c_str(), AliasName.c_str());
			return CXChildVisit_Continue;
		}

		Context->AliasedInstantiations.insert_or_assign(
			FStringHash(ClangUtils::GetString(clang_getTypeSpelling(Target))),
			eastl::make_pair(QualifiedAliasName, AliasName));

		FReflectedStruct* ReflectedStruct = Context->ReflectionDatabase.GetOrCreateReflectedType<FReflectedStruct>(FStringHash(QualifiedAliasName));
		ReflectedStruct->DisplayName = AliasName;
		ReflectedStruct->bIsAlias = true;
		ReflectedStruct->GenerateMetadata(Macro.MacroContents);
		ValidateSpecifiers(Cursor, ESpecifierTarget::Reflect, ReflectedStruct->Metadata);
		ReflectedStruct->Header = Context->ReflectedHeader;
		ReflectedStruct->Type = FReflectedType::EType::Structure;
		ReflectedStruct->LineNumber = ClangUtils::GetCursorLineNumber(Cursor);
		ReflectedStruct->Namespace = Context->CurrentNamespace;

		eastl::string AliasComment = GetCursorComment(Cursor);
		if (!AliasComment.empty())
		{
			ReflectedStruct->Metadata.push_back({"ToolTip", eastl::move(AliasComment)});
		}

		FReflectedType* PreviousType = Context->ParentReflectedType;
		Context->ParentReflectedType = ReflectedStruct;
		Context->LastReflectedType = ReflectedStruct;
		Context->AliasTargetMacroHeader = TargetHeader;

		clang_Type_visitFields(Target, VisitAliasField, Context);

		Context->AliasTargetMacroHeader.clear();
		Context->ParentReflectedType = PreviousType;

		if (ReflectedStruct->Props.empty())
		{
			LRT_ERROR(Cursor, EDiagId::ReflectedNoMembers,
				"REFLECT'd alias '%s' reflected no members. The aliased type's fields need PROPERTY() macros.",
				AliasName.c_str());
		}

		Context->ReflectionDatabase.AddReflectedType(ReflectedStruct);

		return CXChildVisit_Continue;
	}

	CXChildVisitResult VisitStructure(CXCursor Cursor, CXCursor Parent, FClangParserContext* Context)
	{
		eastl::string CursorName = ClangUtils::GetCursorDisplayName(Cursor);

		// Whether this struct is reflected at all is decided first. Every header carries ordinary
		// helper structs, anonymous structs and unions that we are not being asked to reflect, and
		// nothing about them should be able to affect the pass.
		FReflectionMacro Macro;
		if (!Context->TryFindMacroForCursor(Context->ReflectedHeader->HeaderPath, Cursor, Macro))
		{
			return CXChildVisit_Continue;
		}

		eastl::string FullyQualifiedCursorName;
		if (!ClangUtils::GetQualifiedNameForDeclCursor(Cursor, FullyQualifiedCursorName))
		{
			// Now that we know it was REFLECT'd, an unnameable type is a real error worth
			// reporting, rather than something to abandon the whole run over.
			LRT_ERROR(Cursor, EDiagId::MissingGeneratedBody,
				"REFLECT'd struct '%s' has no usable qualified name. Anonymous and locally "
				"declared types cannot be reflected; declare it at namespace or class scope.",
				CursorName.c_str());
			return CXChildVisit_Continue;
		}

		FReflectionMacro GeneratedBody;
		if (!Context->TryFindGeneratedBodyMacro(Context->ReflectedHeader->HeaderPath, Cursor, GeneratedBody))
		{
			LRT_ERROR(Cursor, EDiagId::MissingGeneratedBody,
				"REFLECT'd struct '%s' is missing a GENERATED_BODY() macro inside its body. "
				"Add `GENERATED_BODY()` as the first line of the struct.",
				CursorName.c_str());
			return CXChildVisit_Continue;
		}

		// Keyed under the alias so a property naming FTransform resolves to the VTransform backing it,
		// while CppName keeps the real identifier every emitted declaration has to use.
		const eastl::string CppName = CursorName;
		const eastl::string CppQualifiedName = FullyQualifiedCursorName;

		if (const eastl::string Alias = FindReflectedNameOverride(Macro.MacroContents); !Alias.empty())
		{
			const size_t Scope = FullyQualifiedCursorName.rfind("::");
			FullyQualifiedCursorName = Scope == eastl::string::npos
				? Alias
				: FullyQualifiedCursorName.substr(0, Scope + 2) + Alias;
			CursorName = Alias;
		}

		FReflectedStruct* ReflectedStruct = Context->ReflectionDatabase.GetOrCreateReflectedType<FReflectedStruct>(FStringHash(FullyQualifiedCursorName));
		ReflectedStruct->DisplayName = CursorName;
		ReflectedStruct->CppName = CppName;
		ReflectedStruct->CppQualifiedName = CppQualifiedName;
		ReflectedStruct->GenerateMetadata(Macro.MacroContents);
		ValidateSpecifiers(Cursor, ESpecifierTarget::Reflect, ReflectedStruct->Metadata);
		ReflectedStruct->Header = Context->ReflectedHeader;
		ReflectedStruct->Type = FReflectedType::EType::Structure;
		ReflectedStruct->GeneratedBodyLineNumber = GeneratedBody.LineNumber;
		ReflectedStruct->LineNumber = ClangUtils::GetCursorLineNumber(Cursor);

		if (!Context->CurrentNamespace.empty())
		{
			ReflectedStruct->Namespace = Context->CurrentNamespace;
		}

		eastl::string StructComment = GetCursorComment(Cursor);
		if (!StructComment.empty())
		{
			ReflectedStruct->Metadata.push_back({"ToolTip", eastl::move(StructComment)});
		}

		FReflectedType* PreviousType = Context->ParentReflectedType;
		Context->ParentReflectedType = ReflectedStruct;
		Context->LastReflectedType = ReflectedStruct;

		clang_visitChildren(Cursor, VisitContents<FReflectedStruct>, Context);

		Context->ParentReflectedType = PreviousType;
		Context->ReflectionDatabase.AddReflectedType(ReflectedStruct);

		return CXChildVisit_Recurse;
	}

	// A namespace-scope free function tagged with SCRIPT_EXPORT. Builds an FReflectedFunction (no owning
	// type) carrying the fully-qualified C++ name (the thunk's call target) and the target C# class, then
	// registers it under the header. Mirrors CreateFunctionForType's arg/return extraction.
	CXChildVisitResult VisitFunction(CXCursor Cursor, CXCursor Parent, FClangParserContext* Context)
	{
		FReflectionMacro Macro;
		if (!Context->TryFindMacroForCursor(Context->ReflectedHeader->HeaderPath, Cursor, Macro))
		{
			return CXChildVisit_Continue;
		}
		if (Macro.Type != EReflectionMacro::ScriptExport)
		{
			// Not ours (a stray REFLECT/FUNCTION above an unrelated free function); put it back for its owner.
			Context->AddReflectedMacro(eastl::move(Macro));
			return CXChildVisit_Continue;
		}

		auto NewFunction = eastl::make_unique<FReflectedFunction>();
		NewFunction->bFreeFunction = true;
		NewFunction->Name = ClangUtils::GetCursorSpelling(Cursor);

		// Fully-qualified C++ name by walking the semantic namespace parents (CurrentNamespace has no
		// separators, so it can't be used to form a :: path).
		eastl::string Qualified = NewFunction->Name;
		for (CXCursor P = clang_getCursorSemanticParent(Cursor);
			 !clang_Cursor_isNull(P) && clang_getCursorKind(P) == CXCursor_Namespace;
			 P = clang_getCursorSemanticParent(P))
		{
			Qualified = ClangUtils::GetCursorSpelling(P) + "::" + Qualified;
		}
		NewFunction->QualifiedName = Qualified;

		NewFunction->GenerateMetadata(Macro.MacroContents);
		ValidateSpecifiers(Cursor, ESpecifierTarget::ScriptExport, NewFunction->Metadata);
		for (const FMetadataPair& Meta : NewFunction->Metadata)
		{
			if (Meta.Key == "Class")
			{
				eastl::string Value = Meta.Value;
				if (Value.size() >= 2 && Value.front() == '"' && Value.back() == '"')
				{
					Value = Value.substr(1, Value.size() - 2);
				}
				NewFunction->CSharpTarget = eastl::move(Value);
				break;
			}
		}

		const int NumArgs = clang_Cursor_getNumArguments(Cursor);
		for (int i = 0; i < NumArgs; ++i)
		{
			CXCursor ArgCursor = clang_Cursor_getArgument(Cursor, i);
			eastl::string ArgName = ClangUtils::GetCursorSpelling(ArgCursor);
			CXType FieldType = clang_getCursorType(ArgCursor);
			auto Field = CreateFuncField(Context, FieldType);
			if (Field.has_value() && Field->Flags != EPropertyTypeFlags::None)
			{
				Field->OwningCursor = ArgCursor;
				Field->Name = eastl::move(ArgName);
				NewFunction->AddArgument(eastl::move(Field.value()));
			}
			else
			{
				NewFunction->bHasOmittedArgs = true;
				LRT_WARNING(ArgCursor, Reflection::EDiagId::FunctionFieldFailed,
					"Argument '%s' of SCRIPT_EXPORT function '%s' has an unsupported type and will block its C# binding.",
					ArgName.c_str(), NewFunction->Name.c_str());
			}
		}

		CXType ResultType = clang_getResultType(clang_getCursorType(Cursor));
		if (ResultType.kind != CXType_Void)
		{
			NewFunction->Return = CreateFuncField(Context, ResultType);
		}

		Context->ReflectionDatabase.AddFreeFunction(Context->ReflectedHeader, NewFunction.release());
		return CXChildVisit_Continue;
	}

	CXChildVisitResult VisitClass(CXCursor Cursor, CXCursor Parent, FClangParserContext* Context)
	{
		eastl::string CursorName = ClangUtils::GetCursorDisplayName(Cursor);

		// Reflected or not comes first, for the same reason as structs: an ordinary helper class
		// must never be able to influence the pass.
		FReflectionMacro Macro;
		if (!Context->TryFindMacroForCursor(Context->ReflectedHeader->HeaderPath, Cursor, Macro))
		{
			return CXChildVisit_Continue;
		}

		eastl::string FullyQualifiedCursorName;
		if (!ClangUtils::GetQualifiedNameForDeclCursor(Cursor, FullyQualifiedCursorName))
		{
			LRT_ERROR(Cursor, EDiagId::MissingGeneratedBody,
				"REFLECT'd class '%s' has no usable qualified name. Anonymous and locally "
				"declared types cannot be reflected; declare it at namespace or class scope.",
				CursorName.c_str());
			return CXChildVisit_Continue;
		}

		FReflectionMacro GeneratedBody;
		if (!Context->TryFindGeneratedBodyMacro(Context->ReflectedHeader->HeaderPath, Cursor, GeneratedBody))
		{
			LRT_ERROR(Cursor, EDiagId::MissingGeneratedBody,
				"REFLECT'd class '%s' is missing a GENERATED_BODY() macro inside its body. "
				"Add `GENERATED_BODY()` as the first line of the class.",
				CursorName.c_str());
			return CXChildVisit_Continue;
		}

		FReflectedClass* ReflectedClass = Context->ReflectionDatabase.GetOrCreateReflectedType<FReflectedClass>(FStringHash(FullyQualifiedCursorName));
		ReflectedClass->DisplayName = CursorName;
		ReflectedClass->Header = Context->ReflectedHeader;
		ReflectedClass->Type = FReflectedType::EType::Class;
		ReflectedClass->GeneratedBodyLineNumber = GeneratedBody.LineNumber;
		ReflectedClass->LineNumber = ClangUtils::GetCursorLineNumber(Cursor);
		ReflectedClass->GenerateMetadata(Macro.MacroContents);
		ValidateSpecifiers(Cursor, ESpecifierTarget::Reflect, ReflectedClass->Metadata);

		if (!Context->CurrentNamespace.empty())
		{
			ReflectedClass->Namespace = Context->CurrentNamespace;
		}

		eastl::string ClassComment = GetCursorComment(Cursor);
		if (!ClassComment.empty())
		{
			ReflectedClass->Metadata.push_back({"ToolTip", eastl::move(ClassComment)});
		}

		FReflectedType* PreviousType = Context->ParentReflectedType;
		Context->ParentReflectedType = ReflectedClass;
		Context->LastReflectedType = ReflectedClass;

		clang_visitChildren(Cursor, VisitContents<FReflectedClass>, Context);

		Context->ParentReflectedType = PreviousType;
		Context->ReflectionDatabase.AddReflectedType(ReflectedClass);

		return CXChildVisit_Recurse;
	}
}
