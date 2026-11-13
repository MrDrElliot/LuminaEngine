#include "ReflectionModelBuilder.h"

#include <algorithm>
#include <unordered_map>

#include "Reflector/ProjectSolution.h"
#include "Reflector/ReflectionCore/ReflectedProject.h"
#include "Reflector/ReflectionCore/ReflectionDatabase.h"
#include "Reflector/Types/Properties/ReflectedArrayProperty.h"
#include "Reflector/Types/Properties/ReflectedDelegateProperty.h"
#include "Reflector/Types/Properties/ReflectedMapProperty.h"
#include "Reflector/Types/Properties/ReflectedOptionalProperty.h"

namespace Lumina::Reflection
{
    namespace
    {
        // Indexed by EPropertyKind, ending at the last kind the runtime names.
        constexpr const char* kPropertyKindNames[] =
        {
            "UInt8", "UInt16", "UInt32", "UInt64", "Int8", "Int16", "Int32", "Int64",
            "Entity", "Float", "Double", "Bool", "FString", "FName",
            "Struct", "Enum", "Class", "Object", "SoftObject", "SubStruct", "ComponentRef", "InstancedStruct", "Delegate",
        };
        static_assert(std::size(kPropertyKindNames) == static_cast<size_t>(EPropertyKind::Array));

        EPropertyKind PropertyKindOf(const FReflectedProperty& Property)
        {
            if (const char* Name = Property.GetTypeName())
            {
                const auto Found = std::find_if(std::begin(kPropertyKindNames), std::end(kPropertyKindNames),
                    [Name](const char* Candidate) { return std::string_view(Candidate) == Name; });
                return static_cast<EPropertyKind>(Found - std::begin(kPropertyKindNames));
            }
            if (dynamic_cast<const FReflectedMapProperty*>(&Property) != nullptr)
            {
                return EPropertyKind::Map;
            }
            return dynamic_cast<const FReflectedOptionalProperty*>(&Property) != nullptr ? EPropertyKind::Optional : EPropertyKind::Array;
        }

        // The kind was read off the property first, so each cast below is to the class that kind is made by.
        std::vector<std::string> TypeArgumentsOf(const FReflectedProperty& Property, EPropertyKind Kind)
        {
            switch (Kind)
            {
            case EPropertyKind::Array:    return { static_cast<const FReflectedArrayProperty&>(Property).ElementTypeName };
            case EPropertyKind::Optional: return { static_cast<const FReflectedOptionalProperty&>(Property).ElementTypeName };
            case EPropertyKind::Map:      return { static_cast<const FReflectedMapProperty&>(Property).KeyTypeName, static_cast<const FReflectedMapProperty&>(Property).ValueTypeName };
            case EPropertyKind::Delegate: return static_cast<const FReflectedDelegateProperty&>(Property).ArgTypeNames;
            default:                      return {};
            }
        }

        // Enough for the whole engine, so interning never rehashes the lookup.
        constexpr size_t kExpectedStrings = 64 * 1024;

        uint32_t Count(size_t Size)
        {
            return static_cast<uint32_t>(Size);
        }
    }

    const char* PropertyKindName(EPropertyKind Kind)
    {
        const size_t Index = static_cast<size_t>(Kind);
        return Index < std::size(kPropertyKindNames) ? kPropertyKindNames[Index] : nullptr;
    }

    std::string HeaderIncludePath(const FHeaderView& Header)
    {
        const std::string& Path = Header.Path();
        const FModuleView Module = Header.Module();

        auto TryStrip = [&](const std::string& Prefix) -> std::string
        {
            if (Prefix.empty() || Prefix.size() >= Path.size() || Path.compare(0, Prefix.size(), Prefix) != 0)
            {
                return {};
            }
            size_t Pos = Prefix.size();
            while (Pos < Path.size() && (Path[Pos] == '/' || Path[Pos] == '\\'))
            {
                ++Pos;
            }
            return Pos >= Path.size() ? std::string() : Path.substr(Pos);
        };

        const std::string* BestPrefix = nullptr;
        std::string BestRel;
        for (const FStringId DirId : Module.IncludeDirs())
        {
            const std::string& Dir = Module.GetModel().String(DirId);
            std::string Rel = TryStrip(Dir);
            if (!Rel.empty() && (BestPrefix == nullptr || Dir.size() > BestPrefix->size()))
            {
                BestPrefix = &Dir;
                BestRel = std::move(Rel);
            }
        }
        if (BestPrefix != nullptr)
        {
            return BestRel;
        }

        if (std::string Rel = TryStrip(Module.Path()); !Rel.empty())
        {
            return Rel;
        }

        // No prefix matched, so keep it absolute rather than emit a path the compiler cannot resolve.
        return Path;
    }

    const std::string* FindMetadata(const FMetadataRange& Metadata, std::string_view Key)
    {
        for (const FMetadataView Entry : Metadata)
        {
            if (Entry.Key() == Key)
            {
                return &Entry.Value();
            }
        }
        return nullptr;
    }

    FStringId FReflectionModel::FindString(std::string_view Text) const
    {
        const auto Found = StringIds.find(Text);
        return Found != StringIds.end() ? Found->second : kInvalidIndex;
    }

    FTypeView FReflectionModel::FindType(std::string_view QualifiedName) const
    {
        const auto Found = TypesByName.find(FindString(QualifiedName));
        return Found != TypesByName.end() ? FTypeView(this, Found->second) : FTypeView();
    }

    FStringId FReflectionModelBuilder::Intern(std::string_view Text)
    {
        if (const auto Found = Model.StringIds.find(Text); Found != Model.StringIds.end())
        {
            return Found->second;
        }
        const FStringId Id = Count(Model.Strings.size());
        Model.StringIds.emplace(Model.Strings.emplace_back(Text), Id);
        return Id;
    }

    FIndexRange FReflectionModelBuilder::AddMetadata(const std::vector<FMetadataPair>& Metadata)
    {
        const FIndexRange Range{ Count(Model.MetadataRecords.size()), Count(Metadata.size()) };
        for (const FMetadataPair& Pair : Metadata)
        {
            Model.MetadataRecords.push_back({ Intern(Pair.Key), Intern(Pair.Value) });
        }
        return Range;
    }

    FIndexRange FReflectionModelBuilder::AddStringList(const std::vector<std::string>& Strings)
    {
        const FIndexRange Range{ Count(Model.StringLists.size()), Count(Strings.size()) };
        for (const std::string& String : Strings)
        {
            Model.StringLists.push_back(Intern(String));
        }
        return Range;
    }

    void FReflectionModelBuilder::AddProperty(const FReflectedProperty& Property, FStringId File)
    {
        FPropertyRecord Record;
        Record.Kind = PropertyKindOf(Property);
        Record.Link = Property.GetLink();
        Record.Flags = Property.PropertyFlags;
        Record.bInner = Property.bInner;
        Record.Name = Intern(Property.Name);
        Record.TypeName = Intern(Property.TypeName);
        Record.RawTypeName = Intern(Property.RawTypeName);
        Record.Outer = Intern(Property.Outer);
        Record.Getter = Intern(Property.GetterFunc);
        Record.Setter = Intern(Property.SetterFunc);
        Record.ReflectedAsType = Intern(Property.ReflectedAsType);
        Record.TypeArguments = AddStringList(TypeArgumentsOf(Property, Record.Kind));
        Record.Metadata = AddMetadata(Property.Metadata);
        Record.NativeOffset = Property.NativeOffset;
        Record.Location = { File, Property.LineNumber };
        Model.PropertyRecords.push_back(Record);
    }

    void FReflectionModelBuilder::AddArgument(const FFieldInfo& Field)
    {
        Model.ArgumentRecords.push_back({ Field.Flags, Intern(Field.Name), Intern(Field.TypeName), Intern(Field.RawFieldType), Intern(Field.DefaultValue) });
    }

    void FReflectionModelBuilder::FillFunction(uint32_t Index, const FReflectedFunction& Function, FStringId File)
    {
        FFunctionRecord Record;
        Record.Name = Intern(Function.Name);
        Record.Outer = Intern(Function.Outer);
        Record.QualifiedName = Intern(Function.QualifiedName);
        Record.CSharpTarget = Intern(Function.CSharpTarget);
        Record.Location = { File, Function.LineNumber };
        Record.ReturnIndex = Function.ReturnIndex;
        Record.bHasOmittedArgs = Function.bHasOmittedArgs;
        Record.bIsVirtual = Function.bIsVirtual;
        Record.bIsStatic = Function.bIsStatic;
        Record.bIsConst = Function.bIsConst;
        Record.bReflectNatively = Function.bReflectNatively;
        Record.bFreeFunction = Function.bFreeFunction;

        Record.Arguments = { Count(Model.ArgumentRecords.size()), Count(Function.Arguments.size()) };
        for (const FFieldInfo& Argument : Function.Arguments)
        {
            AddArgument(Argument);
        }
        if (Function.Return.has_value())
        {
            Record.Return = Count(Model.ArgumentRecords.size());
            AddArgument(*Function.Return);
        }

        Record.ParameterProperties = { Count(Model.PropertyRecords.size()), Count(Function.ParamEntries.size()) };
        for (const auto& Entry : Function.ParamEntries)
        {
            AddProperty(*Entry, File);
        }

        Record.Parameters = { Count(Model.ParameterRecords.size()), Count(Function.TopLevelParams.size()) };
        for (size_t Param = 0; Param < Function.TopLevelParams.size(); ++Param)
        {
            const auto Entry = std::find_if(Function.ParamEntries.begin(), Function.ParamEntries.end(),
                [&](const auto& Candidate) { return Candidate.get() == Function.TopLevelParams[Param]; });
            Model.ParameterRecords.push_back({ Record.ParameterProperties.First + Count(Entry - Function.ParamEntries.begin()),
                Intern(Function.ParameterStorageTypes[Param]), Intern(Function.ParameterObjectCastTypes[Param]), Function.ParameterIsRvalue[Param] });
        }

        Record.Metadata = AddMetadata(Function.Metadata);
        Model.FunctionRecords[Index] = Record;
    }

    FReflectionModel FReflectionModelBuilder::Build(const FReflectedWorkspace& Workspace, const FReflectionDatabase& Database)
    {
        FReflectionModelBuilder Builder;
        FReflectionModel& Model = Builder.Model;
        Model.StringIds.reserve(kExpectedStrings);
        Builder.Intern("");
        Model.WorkspaceDir = Builder.Intern(Workspace.GetPath());

        std::unordered_map<const FReflectedType*, uint32_t> TypeIndices;

        for (const auto& Project : Workspace.ReflectedProjects)
        {
            FModuleRecord Module;
            Module.Name = Builder.Intern(Project->Name);
            Module.Path = Builder.Intern(Project->Path);
            Module.CSharpBindingsDir = Builder.Intern(Project->CSharpBindingsDir);
            Module.GeneratedDir = Builder.Intern(Project->GeneratedDir);
            Module.PrecompiledHeader = Builder.Intern(Project->PrecompiledHeader);
            Module.IncludeDirs = Builder.AddStringList(Project->IncludeDirs);
            Module.bRouteTypeBindings = Project->bRouteTypeBindings;
            Module.bReferenceOnly = Project->bReferenceOnly;

            // Sorted, since the project holds its headers in hash order.
            std::vector<FReflectedHeader*> Headers;
            for (const auto& Entry : Project->Headers)
            {
                Headers.push_back(Entry.second.get());
            }
            std::sort(Headers.begin(), Headers.end(), [](const FReflectedHeader* A, const FReflectedHeader* B) { return A->HeaderPath < B->HeaderPath; });

            const uint32_t ModuleIndex = Count(Model.ModuleRecords.size());
            Module.Headers = { Count(Model.HeaderRecords.size()), Count(Headers.size()) };
            Model.ModuleRecords.push_back(Module);
            Model.HeaderRecords.resize(Model.HeaderRecords.size() + Headers.size());

            for (size_t HeaderOffset = 0; HeaderOffset < Headers.size(); ++HeaderOffset)
            {
                FReflectedHeader* Header = Headers[HeaderOffset];
                const uint32_t HeaderIndex = Module.Headers.First + Count(HeaderOffset);
                FHeaderRecord HeaderRecord;
                HeaderRecord.Module = ModuleIndex;
                HeaderRecord.FileName = Builder.Intern(Header->FileName);
                HeaderRecord.Path = Builder.Intern(Header->HeaderPath);
                HeaderRecord.bDirty = Header->bDirty;

                if (const auto Types = Database.ReflectedTypes.find(Header); Types != Database.ReflectedTypes.end())
                {
                    HeaderRecord.Types = { Count(Model.TypeRecords.size()), Count(Types->second.size()) };
                    Model.TypeRecords.resize(Model.TypeRecords.size() + Types->second.size());

                    for (size_t TypeOffset = 0; TypeOffset < Types->second.size(); ++TypeOffset)
                    {
                        const FReflectedType& Type = *Types->second[TypeOffset];
                        const uint32_t TypeIndex = HeaderRecord.Types.First + Count(TypeOffset);
                        TypeIndices.emplace(&Type, TypeIndex);

                        FTypeRecord Record;
                        Record.Kind = Type.Type == FReflectedType::EType::Class ? ETypeKind::Class
                            : (Type.Type == FReflectedType::EType::Enum ? ETypeKind::Enum : ETypeKind::Struct);
                        Record.Header = HeaderIndex;
                        Record.DisplayName = Builder.Intern(Type.DisplayName);
                        Record.CppName = Builder.Intern(Type.EmittedCppName());
                        Record.CppQualifiedName = Builder.Intern(Type.EmittedCppQualifiedName());
                        Record.QualifiedName = Builder.Intern(Type.QualifiedName);
                        Record.Namespace = Builder.Intern(Type.Namespace);
                        Record.Location = { HeaderRecord.Path, Type.LineNumber };
                        Record.GeneratedBodyLine = Type.GeneratedBodyLineNumber;
                        Record.NativeSize = Type.NativeSize;
                        Record.NativeAlign = Type.NativeAlign;
                        Record.bHasUnreflectedFields = Type.bHasUnreflectedFields;
                        Record.bIsAlias = Type.bIsAlias;
                        Record.bIsResource = Type.bIsResource;
                        Record.Metadata = Builder.AddMetadata(Type.Metadata);

                        if (const auto* Struct = dynamic_cast<const FReflectedStruct*>(&Type))
                        {
                            Record.Parent = Builder.Intern(Struct->Parent);
                        }
                        if (const auto* Enum = dynamic_cast<const FReflectedEnum*>(&Type))
                        {
                            Record.UnderlyingSize = Enum->UnderlyingSize;
                            Record.bUnsignedUnderlying = Enum->bUnsignedUnderlying;
                            Record.Constants = { Count(Model.EnumConstantRecords.size()), Count(Enum->Constants.size()) };
                            for (const FReflectedEnum::FConstant& Constant : Enum->Constants)
                            {
                                Model.EnumConstantRecords.push_back({ Builder.Intern(Constant.ID), Builder.Intern(Constant.Label), Builder.Intern(Constant.Description), Constant.Value });
                            }
                        }

                        Record.Properties = { Count(Model.PropertyRecords.size()), Count(Type.Props.size()) };
                        for (const auto& Property : Type.Props)
                        {
                            Builder.AddProperty(*Property, HeaderRecord.Path);
                        }

                        Record.Functions = { Count(Model.FunctionRecords.size()), Count(Type.Functions.size()) };
                        Model.FunctionRecords.resize(Model.FunctionRecords.size() + Type.Functions.size());
                        for (size_t FunctionOffset = 0; FunctionOffset < Type.Functions.size(); ++FunctionOffset)
                        {
                            Builder.FillFunction(Record.Functions.First + Count(FunctionOffset), *Type.Functions[FunctionOffset], HeaderRecord.Path);
                        }

                        Model.TypeRecords[TypeIndex] = Record;
                    }
                }

                if (const auto Functions = Database.FreeFunctions.find(Header); Functions != Database.FreeFunctions.end())
                {
                    HeaderRecord.FreeFunctions = { Count(Model.FunctionRecords.size()), Count(Functions->second.size()) };
                    Model.FunctionRecords.resize(Model.FunctionRecords.size() + Functions->second.size());
                    for (size_t FunctionOffset = 0; FunctionOffset < Functions->second.size(); ++FunctionOffset)
                    {
                        Builder.FillFunction(HeaderRecord.FreeFunctions.First + Count(FunctionOffset), *Functions->second[FunctionOffset], HeaderRecord.Path);
                    }
                }

                Model.HeaderRecords[HeaderIndex] = HeaderRecord;
            }
        }

        for (const auto& [Name, Type] : Database.TypeHashMap)
        {
            if (const auto Found = TypeIndices.find(Type); Found != TypeIndices.end())
            {
                Model.TypesByName.emplace(Builder.Intern(Name.c_str()), Found->second);
            }
        }

        return std::move(Builder.Model);
    }
}
