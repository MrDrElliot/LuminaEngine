#include <clang/AST/Type.h>
#include <clang-c/CXString.h>
#include <clang-c/Index.h>
#include <optional>
#include <string>
#include <vector>
#include <string>
#include <Reflector/Types/ReflectedType.h>
#include "Reflector/Clang/ClangParserContext.h"
#include "Reflector/Clang/Utils.h"
#include "Reflector/CodeGeneration/ReflectionNames.h"
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
#include "Reflector/Types/Properties/ReflectedComponentRefProperty.h"

namespace Lumina::Reflection::Visitor
{
	// Escapes characters that would otherwise break a C string literal in the generated output.
	static std::string GetCursorComment(const CXCursor& Cursor)
	{
		return ClangUtils::EscapeStringLiteral(ClangUtils::GetBriefComment(Cursor));
	}

	// The name a type registers under, from REFLECT(ReflectedName = "X"), or empty when unset.
	static std::string FindReflectedNameOverride(const std::string& MacroContents)
	{
		const size_t Key = MacroContents.find("ReflectedName");
		if (Key == std::string::npos)
		{
			return {};
		}

		const size_t Open = MacroContents.find('"', Key);
		if (Open == std::string::npos)
		{
			return {};
		}

		const size_t Close = MacroContents.find('"', Open + 1);
		if (Close == std::string::npos)
		{
			return {};
		}

		return MacroContents.substr(Open + 1, Close - Open - 1);
	}

	// Lets a SIMD or unreflectable representation present its logical shape without hand-tracking layout.
	static void ApplyReflectAsOverride(FClangParserContext* Context, FReflectedType* Type,
	                                   FReflectedProperty* Property, const CXCursor& Cursor)
	{
		const std::string* Alias = nullptr;
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

		if (std::string_view(Property->GetTypeName()) != std::string_view("Struct"))
		{
			LRT_ERROR(Cursor, Reflection::EDiagId::UnknownPropertyType,
				"Property '%s' uses ReflectAs but its member is not a struct type; "
				"ReflectAs only reinterprets one struct layout as another.",
				Property->Name.c_str());
			return;
		}

		Property->TypeName = Alias->find("::") == std::string::npos
			? std::string("Lumina::") + *Alias
			: *Alias;
		Property->ReflectedAsType = Property->TypeName;
	}

	static void EnsureTemplateInstantiationReflected(FClangParserContext* Context, const FReflectedTemplate& Template,
	                                                 const CXType& Instantiation, const std::string& Spelling,
	                                                 const std::string& ReflectedQualifiedName, const std::string& MangledName);

	// Reflects the instantiation a property names, mangled because TRange<float> is not an identifier.
	static bool TryResolveTemplateInstantiation(FClangParserContext* Context, const CXType& FieldType, std::string& OutTypeName)
	{
		const CXType Canonical = clang_getCanonicalType(FieldType);
		if (Canonical.kind != CXType_Record)
		{
			return false;
		}

		const std::string Spelling = ClangUtils::GetString(clang_getTypeSpelling(Canonical));

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

		std::string TemplateName;
		if (!ClangUtils::GetQualifiedNameFromDeclCursor(TemplateCursor, TemplateName))
		{
			return false;
		}

		const auto Found = Context->ReflectedTemplates.find(FStringHash(TemplateName));
		if (Found == Context->ReflectedTemplates.end())
		{
			return false;
		}

		const std::string Mangled = ClangUtils::MangleTemplateSpelling(Spelling,
			[Context](std::string_view Argument) -> std::string
			{
				const auto Match = Context->AliasedInstantiations.find(FStringHash(Argument));
				return Match == Context->AliasedInstantiations.end() ? std::string() : Match->second.second;
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

	static std::optional<FFieldInfo> CreateFieldInfo(FClangParserContext* Context, const CXCursor& Cursor)
	{
		std::string CursorName = ClangUtils::GetCursorDisplayName(Cursor);

		CXType FieldType = clang_getCursorType(Cursor);

		std::string TypeSpelling;
		if (!ClangUtils::GetQualifiedNameForCXType(FieldType, TypeSpelling))
		{
			LRT_ERROR(Cursor, Reflection::EDiagId::FieldQualifyFailed,
				"Failed to qualify the type of property '%s' in '%s'.",
				CursorName.c_str(),
				Context->ParentReflectedType->GetTypeName().c_str());
			return std::nullopt;
		}

		EPropertyTypeFlags PropFlags = GetCoreTypeFromName(TypeSpelling);

		// An alias or typedef spells its own name, so the core template only shows once canonicalized.
		if (PropFlags == EPropertyTypeFlags::None)
		{
			std::string CanonicalSpelling;
			if (ClangUtils::GetQualifiedNameForCXType(clang_getCanonicalType(FieldType), CanonicalSpelling))
			{
				const EPropertyTypeFlags CanonicalFlags = GetCoreTypeFromName(CanonicalSpelling);
				if (CanonicalFlags != EPropertyTypeFlags::None)
				{
					PropFlags = CanonicalFlags;
					TypeSpelling = CanonicalSpelling;
				}
			}
		}

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
					"Property '%s' is a raw pointer ('%s'). Raw pointers to CObject are not reflectable; use TStrongObjectPtr<T> instead.",
					CursorName.c_str(), TypeSpelling.c_str());
				return std::nullopt;
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
		
		Info.Flags			= PropFlags;
		Info.Type			= FieldType;
		Info.OwningCursor	= Cursor;
		Info.Name			= CursorName;
		Info.TypeName		= TypeSpelling;
		Info.RawFieldType	= ClangUtils::GetSafeTypeAsString(FieldType);

		return Info;
	}

	static std::optional<FFieldInfo> CreateFuncField(FClangParserContext* Context, const CXType& FieldType)
	{
		std::string TypeSpelling;
		if (!ClangUtils::GetQualifiedNameForCXType(FieldType, TypeSpelling))
		{
			return std::nullopt;
		}

		EPropertyTypeFlags PropFlags = GetCoreTypeFromName(TypeSpelling);

		// An alias or typedef spells its own name, so the core template only shows once canonicalized.
		if (PropFlags == EPropertyTypeFlags::None)
		{
			std::string CanonicalSpelling;
			if (ClangUtils::GetQualifiedNameForCXType(clang_getCanonicalType(FieldType), CanonicalSpelling))
			{
				const EPropertyTypeFlags CanonicalFlags = GetCoreTypeFromName(CanonicalSpelling);
				if (CanonicalFlags != EPropertyTypeFlags::None)
				{
					PropFlags = CanonicalFlags;
					TypeSpelling = CanonicalSpelling;
				}
			}
		}

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
					PropFlags = GetCoreTypeFromName(TypeSpelling);
				}
			}
		}
		
		FFieldInfo Info;
		
		if (clang_isConstQualifiedType(FieldType))
		{
			Info.PropertyFlags |= EPropertyFlags::Const;
		}
		
		Info.Flags			= PropFlags;
		Info.Type			= FieldType;
		Info.Name			= "None";
		Info.TypeName		= TypeSpelling;
		Info.RawFieldType	= ClangUtils::GetSafeTypeAsString(FieldType);

		return Info;
	}

	static std::optional<FFieldInfo> CreateSubFieldInfo(FClangParserContext* Context, const CXType& FieldType, const FFieldInfo& ParentField)
	{
		std::string FieldName;
		if (!ClangUtils::GetQualifiedNameForCXType(FieldType, FieldName))
		{
			LRT_ERROR(ParentField.OwningCursor, Reflection::EDiagId::FieldQualifyFailed,
				"Failed to qualify the inner type of property '%s' in '%s'.",
				ParentField.Name.c_str(),
				Context->ParentReflectedType->GetTypeName().c_str());
			return std::nullopt;
		}

		EPropertyTypeFlags PropFlags = GetCoreTypeFromName(FieldName);

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
					"Inner element of property '%s' is a raw pointer ('%s'). Use TStrongObjectPtr<T> instead.",
					ParentField.Name.c_str(), FieldName.c_str());
				return std::nullopt;
			}
		}

		FFieldInfo Info;
		
		if (clang_isConstQualifiedType(FieldType))
		{
			Info.PropertyFlags |= EPropertyFlags::Const;
		}
		
		Info.Flags			= PropFlags;
		Info.Type			= FieldType;
		Info.OwningCursor	= ParentField.OwningCursor;
		Info.TypeName		= FieldName;
		Info.RawFieldType	= ParentField.RawFieldType;
		return Info;
	}

	template<std::derived_from<FReflectedProperty> T>
	static std::unique_ptr<T> CreateProperty(const FFieldInfo& Info)
	{
		std::unique_ptr<T> New = std::make_unique<T>();
		New->Name			= Info.Name;
		New->TypeName		= Info.TypeName;
		New->RawTypeName	= Info.RawFieldType;
		New->PropertyFlags	= Info.PropertyFlags;
		return New;
	}

	static bool CreatePropertyForType(FClangParserContext* Context, FReflectedStruct* Struct, FReflectedProperty*& OutProperty, const FFieldInfo& FieldInfo)
	{
		OutProperty = nullptr;
		
		std::unique_ptr<FReflectedProperty> NewProperty;
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
		case EPropertyTypeFlags::Entity:
		{
			NewProperty = CreateProperty<FReflectedEntityProperty>(FieldInfo);
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
				std::optional<FFieldInfo> SubType = CreateSubFieldInfo(Context, UnderlyingType, FieldInfo);
				if (!SubType.has_value())
				{
					return false;
				}

				SubType->Name = FieldInfo.Name + "_Inner";

				FReflectedProperty* FieldProperty;
				CreatePropertyForType(Context, Struct, FieldProperty, SubType.value());
				FieldProperty->bInner = true;
			}
		}
		break;
		case EPropertyTypeFlags::Object:
		{
			// A member is a TStrongObjectPtr<T>, whose T is the template argument. A function parameter is a raw
			// T*, where the class is the pointee; both spellings describe the same object property.
			CXType ArgType = clang_Type_getTemplateArgumentAsType(FieldInfo.Type, 0);
			if (ArgType.kind == CXType_Invalid)
			{
				ArgType = clang_getUnqualifiedType(clang_getPointeeType(clang_getCanonicalType(FieldInfo.Type)));
			}

			std::optional<FFieldInfo> ParamFieldInfo = CreateSubFieldInfo(Context, ArgType, FieldInfo);
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
			// The base-class filter T is what the property links against.
			const CXType ArgType = clang_Type_getTemplateArgumentAsType(FieldInfo.Type, 0);
			std::optional<FFieldInfo> ParamFieldInfo;
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
			// TSubStructOf<T> links against its base-struct filter T.
			const CXType ArgType = clang_Type_getTemplateArgumentAsType(FieldInfo.Type, 0);
			std::optional<FFieldInfo> ParamFieldInfo;
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
		case EPropertyTypeFlags::ComponentRef:
		{
			// TComponentRef<T> links against the component T it binds.
			const CXType ArgType = clang_Type_getTemplateArgumentAsType(FieldInfo.Type, 0);
			std::optional<FFieldInfo> ParamFieldInfo;
			if (ArgType.kind != CXType_Invalid)
			{
				ParamFieldInfo = CreateSubFieldInfo(Context, ArgType, FieldInfo);
			}
			if (!ParamFieldInfo.has_value())
			{
				return false;
			}

			ParamFieldInfo->Name = FieldInfo.Name;

			NewProperty = CreateProperty<FReflectedComponentRefProperty>(ParamFieldInfo.value());
		}
		break;
		case EPropertyTypeFlags::InstancedStruct:
		{
			// A bare FInstancedStruct constrains nothing, so it emits a null base and takes any struct.
			const CXType ArgType = clang_Type_getTemplateArgumentAsType(FieldInfo.Type, 0);
			std::optional<FFieldInfo> ParamFieldInfo;
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

			NewProperty = CreateProperty<FReflectedInstancedStructProperty>(ParamFieldInfo.value());
		}
		break;
		case EPropertyTypeFlags::SoftObject:
		{
			// FSoftObjectPath has no template arg, while TSoftObjectPtr<T> exposes T as the target class.
			const CXType ArgType = clang_Type_getTemplateArgumentAsType(FieldInfo.Type, 0);
			std::optional<FFieldInfo> ParamFieldInfo;
			if (ArgType.kind != CXType_Invalid)
			{
				ParamFieldInfo = CreateSubFieldInfo(Context, ArgType, FieldInfo);
			}
			if (!ParamFieldInfo.has_value())
			{
				// A bare FSoftObjectPath links against CObject.
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
			std::optional<FFieldInfo> ParamFieldInfo = CreateSubFieldInfo(Context, ArgType, FieldInfo);
			if (!ParamFieldInfo.has_value())
			{
				return false;
			}

			ParamFieldInfo->Name = FieldInfo.Name + "_Inner";

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
			NewProperty = std::move(ArrayProperty);

			FieldProperty->bInner = true; // This property "belongs" to the array.
		}
		break;
		case EPropertyTypeFlags::Map:
		{
			auto MapProperty = CreateProperty<FReflectedMapProperty>(FieldInfo);

			// THashMap args 0 and 1 are the key and value, and the rest are ignored.
			if (clang_Type_getNumTemplateArguments(FieldInfo.Type) < 2)
			{
				LRT_ERROR(FieldInfo.OwningCursor, Reflection::EDiagId::ArrayElementUnknown,
					"Map property '%s' is missing its key/value template arguments.", FieldInfo.Name.c_str());
				return false;
			}

			const CXType KeyType   = clang_Type_getTemplateArgumentAsType(FieldInfo.Type, 0);
			const CXType ValueType = clang_Type_getTemplateArgumentAsType(FieldInfo.Type, 1);

			std::optional<FFieldInfo> KeyInfo   = CreateSubFieldInfo(Context, KeyType, FieldInfo);
			std::optional<FFieldInfo> ValueInfo = CreateSubFieldInfo(Context, ValueType, FieldInfo);
			if (!KeyInfo.has_value() || !ValueInfo.has_value())
			{
				return false;
			}

			KeyInfo->Name = FieldInfo.Name + "_KeyInner";
			ValueInfo->Name = FieldInfo.Name + "_ValueInner";

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
			NewProperty = std::move(MapProperty);

			KeyProperty->bInner = true;   // Key "belongs" to the map.
			ValueProperty->bInner = true; // Value "belongs" to the map.
		}
		break;
		case EPropertyTypeFlags::Optional:
		{
			auto OptionalProperty = CreateProperty<FReflectedOptionalProperty>(FieldInfo);

			// TOptional<T> exposes T as its first template argument, the same shape as TVector<T>.
			const CXType ArgType = clang_Type_getTemplateArgumentAsType(FieldInfo.Type, 0);
			std::optional<FFieldInfo> ParamFieldInfo = CreateSubFieldInfo(Context, ArgType, FieldInfo);
			if (!ParamFieldInfo.has_value())
			{
				return false;
			}

			ParamFieldInfo->Name = FieldInfo.Name + "_Inner";

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
			NewProperty = std::move(OptionalProperty);

			FieldProperty->bInner = true; // Inner T is owned by the optional.
		}
		break;
		case EPropertyTypeFlags::Delegate:
		{
			// Canonical, so an alias or typedef exposes the same arguments a direct spelling does.
			const CXType DelegateType = clang_getCanonicalType(FieldInfo.Type);
			const int NumTemplateArgs = clang_Type_getNumTemplateArguments(DelegateType);

			std::vector<FFieldInfo> ArgFields;
			for (int ArgIndex = 0; ArgIndex < NumTemplateArgs; ++ArgIndex)
			{
				const CXType ArgType = clang_Type_getTemplateArgumentAsType(DelegateType, ArgIndex);
				if (ArgType.kind == CXType_Invalid || ArgType.kind == CXType_Void)
				{
					continue;
				}

				std::optional<FFieldInfo> ArgFieldInfo = CreateSubFieldInfo(Context, ArgType, FieldInfo);
				if (!ArgFieldInfo.has_value())
				{
					return false;
				}

				ArgFieldInfo->Name = FieldInfo.Name + "_Arg" + std::to_string(ArgFields.size());
				ArgFields.push_back(std::move(ArgFieldInfo.value()));
			}

			auto DelegateProperty = CreateProperty<FReflectedDelegateProperty>(FieldInfo);
			for (size_t ArgIndex = 0; ArgIndex < ArgFields.size(); ++ArgIndex)
			{
				DelegateProperty->ArgTypeNames.push_back(ArgFields[ArgIndex].TypeName);

				// Built only to prove the argument is reflectable, since the delegate's trait describes it.
				FReflectedProperty* ArgProperty;
				if (!CreatePropertyForType(Context, Struct, ArgProperty, ArgFields[ArgIndex]) || ArgProperty == nullptr)
				{
					LRT_ERROR(FieldInfo.OwningCursor, Reflection::EDiagId::UnknownPropertyType,
						"Delegate '%s' argument %d has type '%s' which is not reflectable.",
						FieldInfo.Name.c_str(), (int)ArgIndex, ArgFields[ArgIndex].TypeName.c_str());
					return false;
				}

				ArgProperty->bInner = true;
			}

			NewProperty = std::move(DelegateProperty);
		}
		break;
		default:
		{
			// Erroring here stops a field that slipped past every classifier from being silently dropped.
			LRT_ERROR(FieldInfo.OwningCursor, Reflection::EDiagId::UnknownPropertyType,
				"Property '%s' has type '%s' which is not supported by the reflector. "
				"Supported kinds: numeric, bool, FString/FName, enum, struct (REFLECT'd), "
				"TStrongObjectPtr<T>, TVector<T>, TOptional<T>.",
				FieldInfo.Name.c_str(), FieldInfo.TypeName.c_str());
		}
		break;
		}

		if (NewProperty != nullptr)
		{
			NewProperty->LineNumber = ClangUtils::GetCursorLineNumber(FieldInfo.OwningCursor);
			OutProperty = NewProperty.get();
			Struct->PushProperty(std::move(NewProperty));
		}

		return OutProperty != nullptr;
	}


	// Whether a record derives from CObject, which is what separates an object reference from a pointer to a
	// plain struct. CreateFuncField calls every pointer an object, and an ECS component is a struct, so a
	// raw pointer to one would otherwise be described as an object property naming a class that has none.
	static bool DerivesFromCObject(CXType RecordType, int Depth = 0)
	{
		if (Depth > 16)
		{
			return false;
		}

		const CXCursor Declaration = clang_getTypeDeclaration(clang_getCanonicalType(RecordType));
		if (clang_Cursor_isNull(Declaration) != 0)
		{
			return false;
		}

		if (ClangUtils::GetCursorSpelling(Declaration) == "CObject")
		{
			return true;
		}

		struct FSearch
		{
			bool bFound = false;
			int  Depth  = 0;
		};

		FSearch Search{ false, Depth };

		clang_visitChildren(Declaration, [](CXCursor Cursor, CXCursor, CXClientData Data) -> CXChildVisitResult
		{
			FSearch* State = static_cast<FSearch*>(Data);
			if (clang_getCursorKind(Cursor) == CXCursor_CXXBaseSpecifier
				&& DerivesFromCObject(clang_getCursorType(Cursor), State->Depth + 1))
			{
				State->bFound = true;
				return CXChildVisit_Break;
			}
			return CXChildVisit_Continue;
		}, &Search);

		return Search.bFound;
	}

	// The marker every world resource derives from, which is not a reflected parent of its own.
	static bool IsResourceMarker(CXType Type)
	{
		return ClangUtils::GetString(clang_getTypeSpelling(clang_getCanonicalType(Type))) == "Lumina::ECS::FResource";
	}

	static bool DerivesFromResource(CXType RecordType, int Depth = 0)
	{
		if (Depth > 16)
		{
			return false;
		}
		if (IsResourceMarker(RecordType))
		{
			return true;
		}

		const CXCursor Declaration = clang_getTypeDeclaration(clang_getCanonicalType(RecordType));
		if (clang_Cursor_isNull(Declaration) != 0)
		{
			return false;
		}

		struct FSearch
		{
			bool bFound = false;
			int  Depth  = 0;
		};

		FSearch Search{ false, Depth };
		clang_visitChildren(Declaration, [](CXCursor Cursor, CXCursor, CXClientData Data) -> CXChildVisitResult
		{
			FSearch* State = static_cast<FSearch*>(Data);
			if (clang_getCursorKind(Cursor) == CXCursor_CXXBaseSpecifier
				&& DerivesFromResource(clang_getCursorType(Cursor), State->Depth + 1))
			{
				State->bFound = true;
				return CXChildVisit_Break;
			}
			return CXChildVisit_Continue;
		}, &Search);

		return Search.bFound;
	}

	// Kinds a call frame can hold today.
	//
	// Containers and maps are out because their inners are created through the same factory, which attaches
	// them to the owning type, and untangling that for a parameter is worth doing on its own. Object and
	// class are out for a sharper reason: the property factory derives the pointee from a TStrongObjectPtr<T>'s
	// template argument, and a raw CWorld* parameter carries no such argument, so it cannot be qualified.
	// Both want CreateFuncField to describe the inner type, which is the next piece of this rather than a
	// reason to hold the rest back.
	static bool IsSupportedParameterKind(EPropertyTypeFlags Kind)
	{
		switch (Kind)
		{
		case EPropertyTypeFlags::Int8:
		case EPropertyTypeFlags::Int16:
		case EPropertyTypeFlags::Int32:
		case EPropertyTypeFlags::Int64:
		case EPropertyTypeFlags::UInt8:
		case EPropertyTypeFlags::UInt16:
		case EPropertyTypeFlags::UInt32:
		case EPropertyTypeFlags::UInt64:
		case EPropertyTypeFlags::Entity:
		case EPropertyTypeFlags::Float:
		case EPropertyTypeFlags::Double:
		case EPropertyTypeFlags::Bool:
		case EPropertyTypeFlags::Name:
		case EPropertyTypeFlags::String:
		case EPropertyTypeFlags::Struct:
		case EPropertyTypeFlags::Vector:
		case EPropertyTypeFlags::Optional:
		case EPropertyTypeFlags::Map:
		case EPropertyTypeFlags::Object:
		case EPropertyTypeFlags::Enum:
			return true;
		default:
			return false;
		}
	}

	// Turns the parsed signature into the properties the generated frame is described by. A function whose
	// arguments cannot all be described stays unreflected natively, which costs nothing that worked before.
	static void BuildNativeFunctionParameters(FClangParserContext* Context, FReflectedStruct* Struct,
	                                         FReflectedFunction& Function, const CXCursor& Cursor)
	{
		if (Function.bHasOmittedArgs)
		{
			return;
		}

		// Qualified, because offsetof against it is emitted at file scope while the frame itself is declared
		// inside the type's namespace.
		const std::string ParmsStruct = (Struct->Namespace.empty() ? std::string() : Struct->Namespace + "::")
			+ Names::FunctionParmsStruct(Struct->DisplayName, Function.Name);

		auto AddParameter = [&](const FFieldInfo& Info, const std::string& MemberName) -> bool
		{
			if (!IsSupportedParameterKind(Info.Flags))
			{
				return false;
			}

			const CXTypeKind Kind = clang_getCanonicalType(Info.Type).kind;
			const bool bIsRvalueReference = (Kind == CXType_RValueReference);

			// A raw pointer to anything but an object is a buffer, which no property can describe.
			if (Kind == CXType_Pointer && Info.Flags != EPropertyTypeFlags::Object)
			{
				return false;
			}

			const CXType Stored = clang_getUnqualifiedType(clang_getNonReferenceType(Info.Type));
			std::string StorageType = ClangUtils::GetSafeTypeAsString(Stored);
			if (StorageType.empty() || ClangUtils::IsLibclangPlaceholderName(StorageType))
			{
				return false;
			}

			// A handle keeps the object alive across the call, and TFrameObject keeps the class for the trait without needing its definition.
			std::string ObjectCastType;
			if (Info.Flags == EPropertyTypeFlags::Object)
			{
				const CXType Pointee = clang_getPointeeType(clang_getCanonicalType(Stored));
				if (!DerivesFromCObject(Pointee))
				{
					return false;
				}

				ObjectCastType = StorageType;
				StorageType = "Lumina::TFrameObject<" + ClangUtils::GetSafeTypeAsString(clang_getUnqualifiedType(Pointee)) + ">";
			}

			// Described as what the frame stores, not as the signature spelled it. A TVector<float> reaches
			// here as const TVector<float>&, and template arguments live on the record rather than on the
			// reference to it, so the inner type could not be found without looking through it first.
			FFieldInfo StoredInfo = Info;
			StoredInfo.Type = Stored;

			// The raw spelling drives a container's ops lookup and its casts, so it has to be the stored
			// type as well; left as written it would form a pointer to a reference.
			StoredInfo.RawFieldType = StorageType;

			// A container adds its inners here too, so everything added from this mark belongs to the
			// function. Taking only the outer back would leave the inners as fake members of the type.
			const size_t Mark = Struct->NumProperties();

			FReflectedProperty* Raw = nullptr;
			if (!CreatePropertyForType(Context, Struct, Raw, StoredInfo) || Raw == nullptr)
			{
				return false;
			}

			std::vector<std::unique_ptr<FReflectedProperty>> Taken = Struct->TakeBackPropertiesFrom(Mark);
			if (Taken.empty() || Taken.back().get() != Raw)
			{
				return false;
			}

			// The outer is pushed after its inners, so it is the last one taken and the only one that is
			// a frame member; an inner is described by its owner and emits offset 0.
			Taken.back()->Name  = MemberName;
			Taken.back()->Outer = ParmsStruct;

			FReflectedProperty* TopLevel = Taken.back().get();
			for (auto& Entry : Taken)
			{
				Function.ParamEntries.push_back(std::move(Entry));
			}

			Function.TopLevelParams.push_back(TopLevel);
			Function.ParameterStorageTypes.push_back(std::move(StorageType));
			Function.ParameterObjectCastTypes.push_back(std::move(ObjectCastType));
			Function.ParameterIsRvalue.push_back(bIsRvalueReference);
			return true;
		};

		auto Abandon = [&]
		{
			Function.ParamEntries.clear();
			Function.TopLevelParams.clear();
			Function.ParameterStorageTypes.clear();
			Function.ParameterObjectCastTypes.clear();
			Function.ParameterIsRvalue.clear();
			Function.ReturnIndex = -1;
		};

		for (const FFieldInfo& Argument : Function.Arguments)
		{
			if (!AddParameter(Argument, Argument.Name))
			{
				Abandon();
				return;
			}
		}

		if (Function.Return.has_value())
		{
			if (!AddParameter(Function.Return.value(), "ReturnValue"))
			{
				Abandon();
				return;
			}
			Function.ReturnIndex = (int)Function.TopLevelParams.size() - 1;
		}

		Function.bReflectNatively = true;
		(void)Cursor;
	}

	static bool CreateFunctionForType(const CXCursor& Cursor, FClangParserContext* Context, FReflectedStruct* Struct, FReflectedFunction*& OutFunction)
	{
		OutFunction = nullptr;
		
		auto NewFunction = std::make_unique<FReflectedFunction>();
		NewFunction->LineNumber = ClangUtils::GetCursorLineNumber(Cursor);
		NewFunction->Outer = Struct->DisplayName;
		NewFunction->Name = ClangUtils::GetCursorSpelling(Cursor);
		NewFunction->bIsVirtual = clang_CXXMethod_isVirtual(Cursor) != 0;

		int NumArgs = clang_Cursor_getNumArguments(Cursor);

		for (int i = 0; i < NumArgs; ++i)
		{
			CXCursor ArgCursor = clang_Cursor_getArgument(Cursor, i);
			std::string ArgName = ClangUtils::GetCursorSpelling(ArgCursor);
			CXType FieldType = clang_getCursorType(ArgCursor);
			auto Field = CreateFuncField(Context, FieldType);
			if (Field.has_value() && Field->Flags != EPropertyTypeFlags::None)
			{
				Field->OwningCursor = ArgCursor;
				Field->Name			= std::move(ArgName);
				Field->DefaultValue = ClangUtils::GetDefaultArgument(ArgCursor);
				NewFunction->AddArgument(std::move(Field.value()));
			}
			else
			{
				// Flagged so the C# binder skips it, since a generated thunk would call with too few arguments.
				NewFunction->bHasOmittedArgs = true;
				LRT_WARNING(ArgCursor, Reflection::EDiagId::FunctionFieldFailed,
					"Argument '%s' of function '%s' has an unsupported type and will be omitted from the script binding. Reflected function args accept core types, structs, enums, and TStrongObjectPtr<T>.",
					ArgName.c_str(), NewFunction->Name.c_str());
			}
		}
		
		CXType FuncType = clang_getCursorType(Cursor);
		CXType ResultType = clang_getResultType(FuncType);
		
		if (ResultType.kind != CXType_Void)
		{
			NewFunction->Return = CreateFuncField(Context, ResultType);
		}
		
		NewFunction->bIsStatic = clang_CXXMethod_isStatic(Cursor) != 0;
		NewFunction->bIsConst  = clang_CXXMethod_isConst(Cursor) != 0;

		BuildNativeFunctionParameters(Context, Struct, *NewFunction, Cursor);

		OutFunction = NewFunction.get();
		Struct->PushFunction(std::move(NewFunction));

		return NewFunction != nullptr;
	}

	// Negative results are libclang layout errors, such as a type still incomplete where the parse saw it.
	static void RecordNativeLayout(FReflectedType* Type, const CXType& RecordType)
	{
		const long long Size = clang_Type_getSizeOf(RecordType);
		const long long Align = clang_Type_getAlignOf(RecordType);
		Type->NativeSize = Size > 0 ? Size : -1;
		Type->NativeAlign = Align > 0 ? Align : -1;
	}

	static void ReflectField(FClangParserContext* Context, FReflectedStruct* Struct, const CXCursor& Cursor)
	{
		std::string Specifiers;
		if (!ClangUtils::TryGetAnnotation(Cursor, "lumina.property:", Specifiers))
		{
			// Data the reflector cannot see, so the C# emitter must not mirror this type by value.
			Struct->bHasUnreflectedFields |= !Context->bInAnonymousRecord;
			return;
		}

		std::optional<FFieldInfo> FieldInfo = CreateFieldInfo(Context, Cursor);
		if (!FieldInfo.has_value())
		{
			return;
		}

		FReflectedProperty* NewProperty;
		if (!CreatePropertyForType(Context, Struct, NewProperty, FieldInfo.value()))
		{
			return;
		}

		NewProperty->GenerateMetadata(Specifiers);
		ValidateSpecifiers(*Context->Specifiers, Cursor, ESpecifierTarget::Property, NewProperty->Metadata);
		ApplyReflectAsOverride(Context, Struct, NewProperty, Cursor);

		const long long OffsetInBits = clang_Cursor_getOffsetOfField(Cursor);
		NewProperty->NativeOffset = OffsetInBits >= 0 && OffsetInBits % 8 == 0 ? OffsetInBits / 8 : -1;

		if (std::string ConflictMessage; NewProperty->FindConflictingSpecifiers(ConflictMessage))
		{
			LRT_ERROR(Cursor, EDiagId::ConflictingSpecifiers, "%s", ConflictMessage.c_str());
		}
		NewProperty->ApplyDefaultVisibility();

		std::string Comment = GetCursorComment(Cursor);
		if (!Comment.empty())
		{
			NewProperty->Metadata.push_back({"ToolTip", std::move(Comment)});
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

		ReflectField(Context, Context->GetParentReflectedType<FReflectedStruct>(), Cursor);

		return CXVisit_Continue;
	}

	static void EnsureTemplateInstantiationReflected(FClangParserContext* Context, const FReflectedTemplate& Template,
	                                                 const CXType& Instantiation, const std::string& Spelling,
	                                                 const std::string& ReflectedQualifiedName, const std::string& MangledName)
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

		RecordNativeLayout(ReflectedStruct, Instantiation);

		// Registered before its fields are walked, so an instantiation that reaches itself terminates.
		Context->ReflectionDatabase.AddReflectedType(ReflectedStruct);

		FReflectedType* PreviousType = Context->ParentReflectedType;
		const bool bWasInAnonymousRecord = Context->bInAnonymousRecord;

		Context->ParentReflectedType = ReflectedStruct;
		Context->bInAnonymousRecord = false;

		clang_Type_visitFields(Instantiation, VisitAliasField, Context);

		Context->bInAnonymousRecord = bWasInAnonymousRecord;
		Context->ParentReflectedType = PreviousType;
	}

	template<typename TVisitType>
	static CXChildVisitResult VisitContents(CXCursor Cursor, CXCursor Parent, CXClientData pClientData)
	{
		FClangParserContext* Context = (FClangParserContext*)pClientData;
		CXCursorKind Kind = clang_getCursorKind(Cursor);
		TVisitType* Type = Context->GetParentReflectedType<TVisitType>();

		switch (Kind)
		{
		case(CXCursor_CXXBaseSpecifier):
		{
			if (Type->Parent.empty() && !IsResourceMarker(clang_getCursorType(Cursor)))
			{
				Type->Parent = ClangUtils::CursorDisplayName(Cursor).Get();
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
			ReflectField(Context, Type, Cursor);
		}
		break;
		case(CXCursor_CXXMethod):
		{
			std::string Specifiers;
			if (!ClangUtils::TryGetAnnotation(Cursor, "lumina.function:", Specifiers))
			{
				return CXChildVisit_Continue;
			}

			FReflectedFunction* NewFunction;
			CreateFunctionForType(Cursor, Context, Type, NewFunction);
			NewFunction->GenerateMetadata(Specifiers);
			ValidateSpecifiers(*Context->Specifiers, Cursor, ESpecifierTarget::Function, NewFunction->Metadata);

			std::string Comment = GetCursorComment(Cursor);
			if (!Comment.empty())
			{
				NewFunction->Metadata.push_back({"ToolTip", std::move(Comment)});
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
		if (!Context->TryFindMacroForCursor(Context->ReflectedHeader, Cursor, Macro))
		{
			return CXChildVisit_Continue;
		}

		if (Macro.Type != EReflectionMacro::Reflect)
		{
			Context->AddReflectedMacro(std::move(Macro));
			return CXChildVisit_Continue;
		}

		std::string QualifiedName;
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
		ValidateSpecifiers(*Context->Specifiers, Cursor, ESpecifierTarget::Reflect, FMetadataParser(Macro.MacroContents).Metadata);
		Template.Header = Context->ReflectedHeader;

		Context->ReflectedTemplates.insert_or_assign(FStringHash(QualifiedName), std::move(Template));

		return CXChildVisit_Continue;
	}

	// A REFLECT'd `using X = SomeRecord;`, which reflects the aliased record's real members under X.
	CXChildVisitResult VisitTypeAlias(CXCursor Cursor, CXCursor Parent, FClangParserContext* Context)
	{
		// Checked before touching the macro pool, or a member alias consumes its owner's REFLECT.
		const CXCursorKind ParentKind = clang_getCursorKind(clang_getCursorSemanticParent(Cursor));
		if (ParentKind != CXCursor_Namespace && ParentKind != CXCursor_TranslationUnit)
		{
			return CXChildVisit_Continue;
		}

		FReflectionMacro Macro;
		if (!Context->TryFindMacroForCursor(Context->ReflectedHeader, Cursor, Macro))
		{
			return CXChildVisit_Continue;
		}

		if (Macro.Type != EReflectionMacro::Reflect)
		{
			// Somebody else's macro that happens to sit above this alias; hand it back to its owner.
			Context->AddReflectedMacro(std::move(Macro));
			return CXChildVisit_Continue;
		}

		const std::string AliasName = ClangUtils::GetCursorDisplayName(Cursor);

		std::string QualifiedAliasName;
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

		CXFile TargetFile = nullptr;
		clang_getExpansionLocation(clang_getRangeStart(clang_getCursorExtent(TargetCursor)),
			&TargetFile, nullptr, nullptr, nullptr);
		if (TargetFile == nullptr)
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
			std::make_pair(QualifiedAliasName, AliasName));

		FReflectedStruct* ReflectedStruct = Context->ReflectionDatabase.GetOrCreateReflectedType<FReflectedStruct>(FStringHash(QualifiedAliasName));
		ReflectedStruct->DisplayName = AliasName;
		ReflectedStruct->bIsAlias = true;
		ReflectedStruct->GenerateMetadata(Macro.MacroContents);
		ValidateSpecifiers(*Context->Specifiers, Cursor, ESpecifierTarget::Reflect, ReflectedStruct->Metadata);
		ReflectedStruct->Header = Context->ReflectedHeader;
		ReflectedStruct->Type = FReflectedType::EType::Structure;
		ReflectedStruct->LineNumber = ClangUtils::GetCursorLineNumber(Cursor);
		ReflectedStruct->Namespace = Context->CurrentNamespace;
		RecordNativeLayout(ReflectedStruct, Target);

		std::string AliasComment = GetCursorComment(Cursor);
		if (!AliasComment.empty())
		{
			ReflectedStruct->Metadata.push_back({"ToolTip", std::move(AliasComment)});
		}

		FReflectedType* PreviousType = Context->ParentReflectedType;
		Context->ParentReflectedType = ReflectedStruct;
		Context->LastReflectedType = ReflectedStruct;

		clang_Type_visitFields(Target, VisitAliasField, Context);

		Context->ParentReflectedType = PreviousType;

		// A hand-written C# mirror is marshalled by value and never walked, so it needs no member list.
		if (ReflectedStruct->Props.empty() && !ReflectedStruct->HasMetadata("CSharpValueMirror"))
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
		// Every header carries helper, anonymous and union structs that must not affect the pass at all.
		FReflectionMacro Macro;
		if (!Context->TryFindMacroForCursor(Context->ReflectedHeader, Cursor, Macro))
		{
			return CXChildVisit_Continue;
		}

		std::string CursorName = ClangUtils::GetCursorDisplayName(Cursor);

		std::string FullyQualifiedCursorName;
		if (!ClangUtils::GetQualifiedNameForDeclCursor(Cursor, FullyQualifiedCursorName))
		{
			// Now that the REFLECT is confirmed, an unnameable type is a real error rather than a reason to bail.
			LRT_ERROR(Cursor, EDiagId::MissingGeneratedBody,
				"REFLECT'd struct '%s' has no usable qualified name. Anonymous and locally "
				"declared types cannot be reflected; declare it at namespace or class scope.",
				CursorName.c_str());
			return CXChildVisit_Continue;
		}

		FReflectionMacro GeneratedBody;
		if (!Context->TryFindGeneratedBodyMacro(Context->ReflectedHeader, Cursor, GeneratedBody))
		{
			LRT_ERROR(Cursor, EDiagId::MissingGeneratedBody,
				"REFLECT'd struct '%s' is missing a GENERATED_BODY() macro inside its body. "
				"Add `GENERATED_BODY()` as the first line of the struct.",
				CursorName.c_str());
			return CXChildVisit_Continue;
		}

		// Keyed under the alias so a property naming FTransform resolves to the VTransform backing it.
		const std::string CppName = CursorName;
		const std::string CppQualifiedName = FullyQualifiedCursorName;

		if (const std::string Alias = FindReflectedNameOverride(Macro.MacroContents); !Alias.empty())
		{
			const size_t Scope = FullyQualifiedCursorName.rfind("::");
			FullyQualifiedCursorName = Scope == std::string::npos
				? Alias
				: FullyQualifiedCursorName.substr(0, Scope + 2) + Alias;
			CursorName = Alias;
		}

		FReflectedStruct* ReflectedStruct = Context->ReflectionDatabase.GetOrCreateReflectedType<FReflectedStruct>(FStringHash(FullyQualifiedCursorName));
		ReflectedStruct->DisplayName = CursorName;
		ReflectedStruct->CppName = CppName;
		ReflectedStruct->CppQualifiedName = CppQualifiedName;
		ReflectedStruct->GenerateMetadata(Macro.MacroContents);
		ValidateSpecifiers(*Context->Specifiers, Cursor, ESpecifierTarget::Reflect, ReflectedStruct->Metadata);
		ReflectedStruct->Header = Context->ReflectedHeader;
		ReflectedStruct->Type = FReflectedType::EType::Structure;
		ReflectedStruct->GeneratedBodyLineNumber = GeneratedBody.LineNumber;
		ReflectedStruct->LineNumber = ClangUtils::GetCursorLineNumber(Cursor);
		ReflectedStruct->bIsResource = DerivesFromResource(clang_getCursorType(Cursor));
		RecordNativeLayout(ReflectedStruct, clang_getCursorType(Cursor));

		if (!Context->CurrentNamespace.empty())
		{
			ReflectedStruct->Namespace = Context->CurrentNamespace;
		}

		std::string StructComment = GetCursorComment(Cursor);
		if (!StructComment.empty())
		{
			ReflectedStruct->Metadata.push_back({"ToolTip", std::move(StructComment)});
		}

		FReflectedType* PreviousType = Context->ParentReflectedType;
		Context->ParentReflectedType = ReflectedStruct;
		Context->LastReflectedType = ReflectedStruct;

		clang_visitChildren(Cursor, VisitContents<FReflectedStruct>, Context);

		Context->ParentReflectedType = PreviousType;
		Context->ReflectionDatabase.AddReflectedType(ReflectedStruct);

		return CXChildVisit_Recurse;
	}

	// A namespace-scope SCRIPT_EXPORT free function, registered under the header with no owning type.
	CXChildVisitResult VisitFunction(CXCursor Cursor, CXCursor Parent, FClangParserContext* Context)
	{
		std::string Specifiers;
		if (!ClangUtils::TryGetAnnotation(Cursor, "lumina.export:", Specifiers))
		{
			return CXChildVisit_Continue;
		}

		auto NewFunction = std::make_unique<FReflectedFunction>();
		NewFunction->LineNumber = ClangUtils::GetCursorLineNumber(Cursor);
		NewFunction->bFreeFunction = true;
		NewFunction->Name = ClangUtils::GetCursorSpelling(Cursor);

		// Walks the semantic namespace parents, since CurrentNamespace carries no separators to form a path.
		std::string Qualified = NewFunction->Name;
		for (CXCursor P = clang_getCursorSemanticParent(Cursor);
			 !clang_Cursor_isNull(P) && clang_getCursorKind(P) == CXCursor_Namespace;
			 P = clang_getCursorSemanticParent(P))
		{
			Qualified = ClangUtils::GetCursorSpelling(P) + "::" + Qualified;
		}
		NewFunction->QualifiedName = Qualified;

		NewFunction->GenerateMetadata(Specifiers);
		ValidateSpecifiers(*Context->Specifiers, Cursor, ESpecifierTarget::ScriptExport, NewFunction->Metadata);
		for (const FMetadataPair& Meta : NewFunction->Metadata)
		{
			if (Meta.Key == "Class")
			{
				std::string Value = Meta.Value;
				if (Value.size() >= 2 && Value.front() == '"' && Value.back() == '"')
				{
					Value = Value.substr(1, Value.size() - 2);
				}
				NewFunction->CSharpTarget = std::move(Value);
				break;
			}
		}

		const int NumArgs = clang_Cursor_getNumArguments(Cursor);
		for (int i = 0; i < NumArgs; ++i)
		{
			CXCursor ArgCursor = clang_Cursor_getArgument(Cursor, i);
			std::string ArgName = ClangUtils::GetCursorSpelling(ArgCursor);
			CXType FieldType = clang_getCursorType(ArgCursor);
			auto Field = CreateFuncField(Context, FieldType);
			if (Field.has_value() && Field->Flags != EPropertyTypeFlags::None)
			{
				Field->OwningCursor = ArgCursor;
				Field->Name = std::move(ArgName);
				Field->DefaultValue = ClangUtils::GetDefaultArgument(ArgCursor);
				NewFunction->AddArgument(std::move(Field.value()));
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
		// Reflected or not comes first, so an ordinary helper class can never influence the pass.
		FReflectionMacro Macro;
		if (!Context->TryFindMacroForCursor(Context->ReflectedHeader, Cursor, Macro))
		{
			return CXChildVisit_Continue;
		}

		std::string CursorName = ClangUtils::GetCursorDisplayName(Cursor);

		std::string FullyQualifiedCursorName;
		if (!ClangUtils::GetQualifiedNameForDeclCursor(Cursor, FullyQualifiedCursorName))
		{
			LRT_ERROR(Cursor, EDiagId::MissingGeneratedBody,
				"REFLECT'd class '%s' has no usable qualified name. Anonymous and locally "
				"declared types cannot be reflected; declare it at namespace or class scope.",
				CursorName.c_str());
			return CXChildVisit_Continue;
		}

		FReflectionMacro GeneratedBody;
		if (!Context->TryFindGeneratedBodyMacro(Context->ReflectedHeader, Cursor, GeneratedBody))
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
		RecordNativeLayout(ReflectedClass, clang_getCursorType(Cursor));
		ReflectedClass->GenerateMetadata(Macro.MacroContents);
		ValidateSpecifiers(*Context->Specifiers, Cursor, ESpecifierTarget::Reflect, ReflectedClass->Metadata);

		if (!Context->CurrentNamespace.empty())
		{
			ReflectedClass->Namespace = Context->CurrentNamespace;
		}

		std::string ClassComment = GetCursorComment(Cursor);
		if (!ClassComment.empty())
		{
			ReflectedClass->Metadata.push_back({"ToolTip", std::move(ClassComment)});
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
