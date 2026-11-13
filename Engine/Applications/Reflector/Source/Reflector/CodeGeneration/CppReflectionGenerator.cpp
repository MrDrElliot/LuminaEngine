#include "CppReflectionGenerator.h"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

#include "GeneratedFiles.h"
#include "ReflectionNames.h"
#include "Reflector/ReflectionSpecifiers.h"

namespace Lumina::Reflection
{
    namespace
    {
        // Must match ReflectionStep.UnityShardCount in LuminaBuildTool.
        constexpr int kUnityShardCount = 8;

        // Runtime lands on the 8-shard cap, while a plugin with ten types stays at one and compiles as before.
        constexpr size_t kGeneratedSourcesPerShard = 20;

        constexpr const char* kModuleTableFile = "ReflectionModule.gen.cpp";

        constexpr const char* kUnityStubContents =
            "// Reflection unity stub.\n"
            "// This project has no reflected types yet; this file exists\n"
            "// only so the vcxproj's source list resolves. The Reflector\n"
            "// will overwrite it the moment a reflected type appears.\n";

        // Emits one header's C++, asking the claim registry where each metadata entry lands.
        class FCppEmitter
        {
        public:

            FCppEmitter(FCodeWriter& InWriter, const FSpecifierRegistry& InSpecifiers) : Writer(InWriter), Specifiers(InSpecifiers) {}

            void DefineInitialHeader(const FTypeView& Type, const std::string& FileID);
            void DefineSecondaryHeader(const FTypeView& Type, const std::string& FileID);
            void DeclareImplementation(const FTypeView& Type);
            void EmitTypeLinks(const TViewRange<FTypeView>& Types);

        private:

            void DeclareEnum(const FTypeView& Type);
            void DeclareStruct(const FTypeView& Type);
            void DeclareClass(const FTypeView& Type);

            void EmitMetadataArray(std::string_view SymbolBase, const FMetadataRange& Metadata, ESpecifierTarget Target);
            std::string MetadataField(std::string_view SymbolBase, const FMetadataRange& Metadata, ESpecifierTarget Target) const;
            std::vector<FMetadataView> StoredMetadata(const FMetadataRange& Metadata, ESpecifierTarget Target, size_t& OutNumRuntime) const;

            void EmitTypeFunctions(const FTypeView& Type, const std::string& ExtraConstruction = {});
            void EmitDescriptor(const FPropertyView& Property);
            void EmitMetadataArrays(const FTypeView& Type);
            void EmitPropertyTable(const FTypeView& Type);
            void DefinePropertyTable(const FTypeView& Type);
            void EmitLayoutAsserts(const FTypeView& Type);
            void EmitFunctionFrames(const FTypeView& Type);
            void EmitFunctionTable(const FTypeView& Type);
            void DefineFunctionParameterTables(const FTypeView& Type);
            void DefineFunctionTable(const FTypeView& Type);
            void OpenNamespace(const FTypeView& Type);
            void CloseNamespace(const FTypeView& Type);

            FCodeWriter&              Writer;
            const FSpecifierRegistry& Specifiers;
        };

        // The runtime type that describes this one.
        const char* RuntimeTypeName(const FTypeView& Type)
        {
            switch (Type.Kind())
            {
            case ETypeKind::Class: return "CClass";
            case ETypeKind::Enum:  return "CEnum";
            default:               return "CStruct";
            }
        }

        // The accessor generated code and traits reach the type through.
        const char* AccessorName(const FTypeView& Type)
        {
            switch (Type.Kind())
            {
            case ETypeKind::Class: return "StaticClass";
            case ETypeKind::Enum:  return "StaticEnum";
            default:               return "StaticStruct";
            }
        }

        // Declared in the type's own namespace so a class can befriend it.
        std::string TypeInfoName(const FTypeView& Type)
        {
            return "FTypeInfo_" + Type.DisplayName();
        }

        std::string TypeInfoQualifiedName(const FTypeView& Type)
        {
            return Type.Namespace().empty() ? ("::" + TypeInfoName(Type)) : ("::" + Type.Namespace() + "::" + TypeInfoName(Type));
        }

        // Named without the type, so a module table can list it by declaration alone.
        std::string TypeFunctionName(const FTypeView& Type)
        {
            return "GetType_" + Names::FriendlyFromParts(Type.Namespace(), Type.DisplayName());
        }

        bool HasOwnProperties(const FTypeView& Type)
        {
            const auto Properties = Type.Properties();
            return std::any_of(Properties.begin(), Properties.end(), [](const FPropertyView& Property) { return !Property.IsInner(); });
        }

        bool HasNativeFunctions(const FTypeView& Type)
        {
            const auto Functions = Type.Functions();
            return std::any_of(Functions.begin(), Functions.end(), [](const FFunctionView& Function) { return Function.ReflectsNatively(); });
        }

        // The descriptor fields naming the member tables, empty for a table that was not emitted.
        std::string MemberTableFields(const FTypeView& Type)
        {
            std::string Fields;
            if (HasOwnProperties(Type))
            {
                Fields += ", .Properties = Properties, .NumProperties = (Lumina::uint32)std::size(Properties)";
            }
            if (HasNativeFunctions(Type))
            {
                Fields += ", .Functions = Functions, .NumFunctions = (Lumina::uint32)std::size(Functions)";
            }
            return Fields;
        }

        // The forward declaration must name the same underlying type as the real one or MSVC rejects it C3433.
        const char* UnderlyingTypeName(uint32_t Size, bool bUnsigned)
        {
            switch (Size)
            {
            case 1:  return bUnsigned ? "uint8"  : "int8";
            case 2:  return bUnsigned ? "uint16" : "int16";
            case 8:  return bUnsigned ? "uint64" : "int64";
            default: return bUnsigned ? "uint32" : "int32";
            }
        }

        void EmitFileBanner(FCodeWriter& Writer)
        {
            Writer.Line("//*************************************************************************");
            Writer.Line("// Generated by Lumina Reflection Tool. ");
            Writer.Line("// This is an auto-generated file - DO NOT EDIT.");
            Writer.Line("//*************************************************************************");
            Writer.Line();
        }

        // Generated sources must open with their own module's PCH, since MSVC rejects anything else under /Yu.
        std::string PchInclude(const FModuleView& Module)
        {
            return Module.PrecompiledHeader().empty() ? std::string() : "#include \"" + Module.PrecompiledHeader() + "\"\n";
        }

        // Guarded, since a unity shard concatenates files that would each declare the same thing.
        void EmitGuardedDeclaration(FCodeWriter& Writer, const std::string& Guard, const std::string& Declaration)
        {
            Writer.Linef("#ifndef LRT_DECL_%s", Guard.c_str());
            Writer.Linef("#define LRT_DECL_%s", Guard.c_str());
            Writer.Line(Declaration);
            Writer.Line("#endif");
        }

        int ShardCountFor(size_t SourceCount)
        {
            if (SourceCount <= kGeneratedSourcesPerShard)
            {
                return 1;
            }

            const size_t Wanted = (SourceCount + kGeneratedSourcesPerShard - 1) / kGeneratedSourcesPerShard;
            return static_cast<int>(Wanted < kUnityShardCount ? Wanted : kUnityShardCount);
        }

        uint64_t StableHash(const std::string& Text)
        {
            uint64_t Hash = 14695981039346656037ULL;
            for (const char Character : Text)
            {
                Hash = (Hash ^ static_cast<unsigned char>(Character)) * 1099511628211ULL;
            }
            return Hash;
        }

        uint64_t Scramble(uint64_t Value)
        {
            Value ^= Value >> 33;
            Value *= 0xFF51AFD7ED558CCDULL;
            Value ^= Value >> 33;
            Value *= 0xC4CEB9FE1A85EC53ULL;
            return Value ^ (Value >> 33);
        }

        // Highest-weight shard, so adding a reflected type restripes one shard rather than all of them.
        int ShardFor(const std::string& SourceName, int ActiveShards)
        {
            const uint64_t Seed = StableHash(SourceName);

            int Best = 0;
            uint64_t BestWeight = 0;
            for (int Shard = 0; Shard < ActiveShards; ++Shard)
            {
                const uint64_t Weight = Scramble(Seed ^ (static_cast<uint64_t>(Shard + 1) * 0x9E3779B97F4A7C15ULL));
                if (Weight > BestWeight)
                {
                    BestWeight = Weight;
                    Best = Shard;
                }
            }
            return Best;
        }

        std::string UnityPath(const FModuleView& Module, int Shard)
        {
            return GeneratedFiles::ModuleDir(Module) + "/ReflectionUnity_" + std::to_string(Shard) + ".gen.cpp";
        }

        // Every type a module reflects, in one table its module registers once at load.
        std::string ModuleTableContents(const FModuleView& Module)
        {
            std::vector<FTypeView> Types = GeneratedFiles::ModuleTypes(Module);
            std::sort(Types.begin(), Types.end(), [](const FTypeView& A, const FTypeView& B) { return TypeFunctionName(A) < TypeFunctionName(B); });

            FCodeWriter Writer;
            EmitFileBanner(Writer);
            Writer.Line("#include \"Core/Object/ObjectBase.h\"");
            Writer.Line();

            Writer.Line("namespace Lumina::Generated");
            Writer.Line("{");
            for (const FTypeView& Type : Types)
            {
                Writer.Linef("\tLumina::%s* %s();", RuntimeTypeName(Type), TypeFunctionName(Type).c_str());
            }
            Writer.Line("}");
            Writer.Line();

            Writer.Line("namespace");
            Writer.Line("{");

            auto EmitTable = [&](ETypeKind Kind, const char* Constructor, const char* Table) -> std::string
            {
                std::string Rows;
                for (const FTypeView& Type : Types)
                {
                    if (Type.Kind() == Kind)
                    {
                        Rows += "\t\t&Lumina::Generated::" + TypeFunctionName(Type) + ",\n";
                    }
                }
                if (Rows.empty())
                {
                    return std::string(".") + Table + " = nullptr";
                }

                Writer.Linef("\tconstexpr Lumina::%s %s[] =", Constructor, Table);
                Writer.Line("\t{");
                Writer.Append(Rows);
                Writer.Line("\t};");
                Writer.Line();
                return std::string(".") + Table + " = " + Table + ", .Num" + Table + " = (Lumina::uint32)std::size(" + Table + ")";
            };

            const std::string Enums = EmitTable(ETypeKind::Enum, "FEnumConstructor", "Enums");
            const std::string Structs = EmitTable(ETypeKind::Struct, "FStructConstructor", "Structs");
            const std::string Classes = EmitTable(ETypeKind::Class, "FClassConstructor", "Classes");

            Writer.Linef("\tconstexpr Lumina::FCompiledInTypes Types = { %s, %s, %s, .Module = \"%s\", .LayoutHash = %sULL };",
                Enums.c_str(), Structs.c_str(), Classes.c_str(), Module.Name().c_str(), GeneratedFiles::HexLiteral(GeneratedFiles::ModuleLayoutHash(Module)).c_str());
            Writer.Line("\tconst Lumina::FTypeRegistrar Registrar(Types);");
            Writer.Line("}");

            return Writer.Release();
        }

        // Every shard must exist even at zero reflected types, or premake's static list points at missing files.
        void WriteUnityBuildFiles(FGeneratedOutputs& Outputs, const FModuleView& Module, std::vector<std::string> SourceNames)
        {
            std::sort(SourceNames.begin(), SourceNames.end());
            const int ActiveShards = ShardCountFor(SourceNames.size());

            std::vector<std::string> Shards(kUnityShardCount);
            for (const std::string& Source : SourceNames)
            {
                Shards[ShardFor(Source, ActiveShards)] += "#include \"" + Source + "\"\n";
            }

            // The module table rides in the first shard, which a project with any reflected type always compiles.
            if (!SourceNames.empty())
            {
                Outputs.Open(GeneratedFiles::ModuleDir(Module) + "/" + kModuleTableFile).Append(ModuleTableContents(Module));
                Shards[0] += std::string("#include \"") + kModuleTableFile + "\"\n";
            }

            for (int Shard = 0; Shard < kUnityShardCount; ++Shard)
            {
                Outputs.Open(UnityPath(Module, Shard)).Append(Shards[Shard].empty() ? std::string(kUnityStubContents) : PchInclude(Module) + Shards[Shard]);
            }
        }

        void WriteHeaderContent(FCodeWriter& Writer, const FSpecifierRegistry& Specifiers, const FHeaderView& Header)
        {
            const std::string FileID = Names::MakeFileIDForHeaderPath(Header.Path());
            FCppEmitter Emitter(Writer, Specifiers);

            Writer.Line("#pragma once");
            Writer.Line();
            EmitFileBanner(Writer);

            Writer.Line("#include \"Core/Object/ObjectMacros.h\"");
            Writer.BlankLines(2);

            // Guard against double-inclusion without a #pragma once in the upstream header.
            Writer.Linef("#ifdef %s_generated_h", FileID.c_str());
            Writer.Line("#error Already included, missing #pragma once");
            Writer.Line("#endif");
            Writer.Linef("#define %s_generated_h", FileID.c_str());
            Writer.BlankLines(3);

            // Forward declarations, accessor specializations and DECLARE_CLASS macros.
            for (const FTypeView Type : Header.Types())
            {
                Emitter.DefineInitialHeader(Type, FileID);
            }

            Writer.BlankLines(3);
            Writer.Line("//--------------------------------------------------------------------------------------");
            Writer.BlankLines(3);

            // GENERATED_BODY expansion macros.
            for (const FTypeView Type : Header.Types())
            {
                Emitter.DefineSecondaryHeader(Type, FileID);
            }

            Writer.BlankLines(3);
            Writer.Line("#undef CURRENT_FILE_ID ");
            Writer.Linef("\t #define CURRENT_FILE_ID %s", FileID.c_str());
        }

        void WriteSourceContent(FCodeWriter& Writer, const FSpecifierRegistry& Specifiers, const FHeaderView& Header)
        {
            FCppEmitter Emitter(Writer, Specifiers);
            EmitFileBanner(Writer);

            const FModuleView Module = Header.Module();
            if (!Module.PrecompiledHeader().empty())
            {
                Writer.Linef("#include \"%s\"", Module.PrecompiledHeader().c_str());
            }

            Writer.Linef("#include \"%s\"", HeaderIncludePath(Header).c_str());
            Writer.Line("#include \"World/Entity/Components/Component.h\"");
            Writer.Line("#include \"World/ECS/EventMeta.h\"");
            // Engine modules get this through their PCH, but a game project's chain does not reach it.
            Writer.Line("#include \"World/Entity/Systems/EntitySystem.h\"");
            Writer.Line("#include \"Core/Profiler/Profile.h\"");
            Writer.Line("#include \"Core/Math/Hash/Hash.h\"");
            Writer.Line("#include \"Core/Object/Class.h\"");
            Writer.Line("#include \"Core/Object/ObjectCore.h\"");
            Writer.Line("#include \"Core/Reflection/Type/Function.h\"");
            Writer.Line("#include \"Core/Reflection/PropertyTypeTraits.h\"");
            Writer.Line("#include \"Scripting/ScriptReturn.h\"");
            Writer.Line("#include <utility>");
            // Pull the headers and placement-new only when the header actually declares a Scriptable class.
            const auto Types = Header.Types();
            if (std::any_of(Types.begin(), Types.end(), [](const FTypeView& Type) { return Type.Kind() == ETypeKind::Class && Type.HasMetadata("Scriptable"); }))
            {
                Writer.Line("#include \"Core/Object/ScriptClass.h\"");
                Writer.Line("#include \"Scripting/ScriptableObject.h\"");
                Writer.Line("#include \"Scripting/DotNet/DotNetHost.h\"");
                Writer.Line("#include <new>");
            }
            Writer.BlankLines(2);

            Emitter.EmitTypeLinks(Types);

            Writer.Line("//*************************************************************************");
            Writer.Line("// Type Implementations. ");
            Writer.Line("//*************************************************************************");
            Writer.Line();
            for (const FTypeView Type : Types)
            {
                Emitter.DeclareImplementation(Type);
            }

            Writer.BlankLines(2);
        }
    }

    // Entries every build keeps, then the editor's, which a build without the editor counts off the end.
    std::vector<FMetadataView> FCppEmitter::StoredMetadata(const FMetadataRange& Metadata, ESpecifierTarget Target, size_t& OutNumRuntime) const
    {
        std::vector<FMetadataView> Stored;
        for (const ESpecifierStorage Wanted : { ESpecifierStorage::Metadata, ESpecifierStorage::EditorMetadata })
        {
            for (const FMetadataView Pair : Metadata)
            {
                if (!Pair.Key().empty() && Specifiers.GetStorage(Target, Pair.Key()) == Wanted)
                {
                    Stored.push_back(Pair);
                }
            }
            if (Wanted == ESpecifierStorage::Metadata)
            {
                OutNumRuntime = Stored.size();
            }
        }
        return Stored;
    }

    // A static FMetadataEntry array named after SymbolBase, holding what Target stores as metadata.
    void FCppEmitter::EmitMetadataArray(std::string_view SymbolBase, const FMetadataRange& Metadata, ESpecifierTarget Target)
    {
        size_t NumRuntime = 0;
        const std::vector<FMetadataView> Stored = StoredMetadata(Metadata, Target, NumRuntime);
        if (Stored.empty())
        {
            return;
        }

        Writer.Linef("static constexpr Lumina::FMetadataEntry %.*s_Metadata[] = {", static_cast<int>(SymbolBase.size()), SymbolBase.data());
        for (const FMetadataView& Pair : Stored)
        {
            Writer.Linef("\t{ \"%s\", \"%s\" },", Pair.Key().c_str(), Pair.Value().c_str());
        }
        Writer.Line("};");
    }

    // The trailing LUMINA_METADATA field of a descriptor, or nothing when Target stores no metadata.
    std::string FCppEmitter::MetadataField(std::string_view SymbolBase, const FMetadataRange& Metadata, ESpecifierTarget Target) const
    {
        size_t NumRuntime = 0;
        if (StoredMetadata(Metadata, Target, NumRuntime).empty())
        {
            return {};
        }
        return " LUMINA_METADATA(" + std::string(SymbolBase) + "_Metadata, " + std::to_string(NumRuntime) + ")";
    }

    void FCppEmitter::OpenNamespace(const FTypeView& Type)
    {
        if (!Type.Namespace().empty())
        {
            Writer.Linef("namespace %s", Type.Namespace().c_str());
            Writer.Line("{");
        }
    }

    void FCppEmitter::CloseNamespace(const FTypeView& Type)
    {
        if (!Type.Namespace().empty())
        {
            Writer.Line("}");
        }
        Writer.Line();
    }

    // The type function and the accessor specialization that forwards to it. ExtraConstruction runs once, after the descriptor is built.
    void FCppEmitter::EmitTypeFunctions(const FTypeView& Type, const std::string& ExtraConstruction)
    {
        const char* Kind = RuntimeTypeName(Type);
        const std::string Api = Names::ProjectApiMacro(Type.Module().Name());
        const std::string Function = TypeFunctionName(Type);

        Writer.Line("namespace Lumina::Generated");
        Writer.Line("{");
        Writer.Linef("	Lumina::%s* %s()", Kind, Function.c_str());
        Writer.Line("	{");
        Writer.Linef("		static Lumina::%s* Type = nullptr;", Kind);
        Writer.Line("		if (Type == nullptr)");
        Writer.Line("		{");
        Writer.Linef("			Lumina::Construct%s(&Type, %s::Desc);", Kind, TypeInfoQualifiedName(Type).c_str());
        if (!ExtraConstruction.empty())
        {
            Writer.Linef("			%s", ExtraConstruction.c_str());
        }
        Writer.Line("		}");
        Writer.Line("		return Type;");
        Writer.Line("	}");
        Writer.Line("}");
        Writer.Line();

        Writer.Linef("template<> %s Lumina::%s* Lumina::%s<%s>()", Api.c_str(), Kind, AccessorName(Type), Type.CppQualifiedName().c_str());
        Writer.Line("{");
        Writer.Linef("	return Lumina::Generated::%s();", Function.c_str());
        Writer.Line("}");
        Writer.Line();
    }

    // One FPropertyDesc row, whose type the runtime's trait derives from the member itself.
    void FCppEmitter::EmitDescriptor(const FPropertyView& Property)
    {
        const std::string& Name = Property.Name();
        const std::string& Outer = Property.Outer();
        const std::string Member = Outer + "::" + Name;
        const std::string Value = Property.ReflectedAsType().empty() ? ("decltype(" + Member + ")") : Property.ReflectedAsType();

        std::string Row = "{ .Name = \"" + Name + "\", .Offset = offsetof(" + Outer + ", " + Name + "), .Flags = "
            + PropertyFlagsToString(Property.Flags()) + ", .Type = Lumina::PropertyTypeOf<" + Value + ">";

        if (!Property.Getter().empty())
        {
            Row += ", .Getter = +[](const void* Container, void* Out) { *static_cast<" + Value + "*>(Out) = static_cast<" + Value
                + ">(static_cast<const " + Outer + "*>(Container)->" + Property.Getter() + "()); }";
        }
        if (!Property.Setter().empty())
        {
            Row += ", .Setter = +[](void* Container, const void* In) { static_cast<" + Outer + "*>(Container)->" + Property.Setter()
                + "(*static_cast<const " + Value + "*>(In)); }";
        }
        Row += MetadataField(Name, Property.Metadata(), ESpecifierTarget::Property);

        std::string EditorFlags;
        for (const FMetadataView Pair : Property.Metadata())
        {
            if (Specifiers.GetStorage(ESpecifierTarget::Property, Pair.Key()) == ESpecifierStorage::EditorFlag)
            {
                EditorFlags += (EditorFlags.empty() ? "" : " | ") + std::string("Lumina::EPropertyEditorFlags::") + Pair.Key();
            }
        }
        if (!EditorFlags.empty())
        {
            Row += ", .EditorFlags = " + EditorFlags;
        }

        Writer.Line((Row + " },").c_str());
    }

    void FCppEmitter::EmitMetadataArrays(const FTypeView& Type)
    {
        EmitMetadataArray(Names::FriendlyFromQualified(Type.QualifiedName()), Type.Metadata(), ESpecifierTarget::Reflect);

        for (const FPropertyView Property : Type.Properties())
        {
            if (!Property.IsInner())
            {
                EmitMetadataArray(Property.Name(), Property.Metadata(), ESpecifierTarget::Property);
            }
        }

        for (const FFunctionView Function : Type.Functions())
        {
            if (Function.ReflectsNatively())
            {
                EmitMetadataArray("Func_" + Function.Name(), Function.Metadata(), ESpecifierTarget::Function);
            }
        }
    }

    void FCppEmitter::EmitPropertyTable(const FTypeView& Type)
    {
        const auto Properties = Type.Properties();
        const size_t Count = std::count_if(Properties.begin(), Properties.end(), [](const FPropertyView& Property) { return !Property.IsInner(); });
        if (Count == 0)
        {
            return;
        }

        Writer.Linef("static const Lumina::FPropertyDesc Properties[%zu];", Count);
        Writer.Line();
    }

    void FCppEmitter::DefinePropertyTable(const FTypeView& Type)
    {
        if (!HasOwnProperties(Type))
        {
            return;
        }

        Writer.Linef("constexpr Lumina::FPropertyDesc %s::Properties[] =", TypeInfoName(Type).c_str());
        Writer.BeginBlock();
        for (const FPropertyView Property : Type.Properties())
        {
            if (!Property.IsInner())
            {
                EmitDescriptor(Property);
            }
        }
        Writer.PopIndent();
        Writer.Line("};");
        Writer.Line();
    }

    // Holds the real compiler to the clang layout the C# bindings bake in as constants.
    void FCppEmitter::EmitLayoutAsserts(const FTypeView& Type)
    {
        const std::string& DisplayName = Type.DisplayName();
        if (Type.NativeSize() > 0 && Type.Kind() != ETypeKind::Class)
        {
            Writer.Linef("static_assert(sizeof(%s) == %lld, \"%s changed size since the reflector laid it out; rebuild so the C# bindings match.\");",
                Type.CppQualifiedName().c_str(), (long long)Type.NativeSize(), DisplayName.c_str());
        }

        for (const FPropertyView Property : Type.Properties())
        {
            if (Property.IsInner() || Property.NativeOffset() < 0)
            {
                continue;
            }
            Writer.Linef("static_assert(offsetof(%s, %s) == %lld, \"%s::%s moved since the reflector laid it out; rebuild so the C# bindings match.\");",
                Property.Outer().c_str(), Property.Name().c_str(), (long long)Property.NativeOffset(), DisplayName.c_str(), Property.Name().c_str());
        }
    }

    // The frame struct and thunk for each function, at file scope since offsetof is taken against them.
    void FCppEmitter::EmitFunctionFrames(const FTypeView& Type)
    {
        for (const FFunctionView Function : Type.Functions())
        {
            if (!Function.ReflectsNatively())
            {
                continue;
            }

            const std::string& Name = Function.Name();
            const std::string Parms = Names::FunctionParmsStruct(Type.DisplayName(), Name);
            const std::string Thunk = Names::FunctionThunk(Type.DisplayName(), Name);
            const auto Parameters = Function.Parameters();

            Writer.Linef("struct %s", Parms.c_str());
            Writer.Line("{");
            for (const FParameterView Parameter : Parameters)
            {
                Writer.Linef("\t%s %s;", Parameter.StorageType().c_str(), Parameter.Property().Name().c_str());
            }
            Writer.Line("};");
            Writer.Line();

            // A static takes no object, and anything else dispatches through the pointer, which is what makes a virtual reach its override.
            std::string Arguments;
            const size_t NumArguments = Function.ReturnIndex() >= 0 ? Parameters.size() - 1 : Parameters.size();
            for (size_t Index = 0; Index < NumArguments; ++Index)
            {
                if (Index != 0)
                {
                    Arguments += ", ";
                }

                // An object argument is held as a handle, cast unqualified since every reflected class shares one root.
                const FParameterView Parameter = Parameters[Index];
                const std::string& ParamName = Parameter.Property().Name();
                if (!Parameter.ObjectCastType().empty())
                {
                    Arguments += "(" + Parameter.ObjectCastType() + ")P." + ParamName + ".Get()";
                }
                else if (Parameter.IsRvalue())
                {
                    Arguments += "std::move(P." + ParamName + ")";
                }
                else
                {
                    Arguments += "P." + ParamName;
                }
            }

            // The real class name, since a call has to name the type rather than whatever alias reflects it.
            const std::string& Owner = Type.CppQualifiedName();
            const std::string Call = Function.IsStatic()
                ? (Owner + "::" + Name + "(" + Arguments + ")")
                : ("((" + Owner + "*)Context)->" + Name + "(" + Arguments + ")");

            Writer.Linef("static void %s(const Lumina::FFunction&, void* Context, void* Frame)", Thunk.c_str());
            Writer.Line("{");
            if (!Parameters.empty())
            {
                Writer.Linef("\t%s& P = *(%s*)Frame;", Parms.c_str(), Parms.c_str());
            }
            else
            {
                Writer.Line("\t(void)Frame;");
            }
            if (Function.IsStatic())
            {
                Writer.Line("\t(void)Context;");
            }
            if (Function.ReturnIndex() >= 0)
            {
                // An object return goes into a handle to CObject, spelled out where the class is only forward declared.
                const FParameterView Return = Parameters[static_cast<size_t>(Function.ReturnIndex())];
                const std::string Assigned = Return.ObjectCastType().empty() ? Call : ("(Lumina::CObject*)(" + Call + ")");
                Writer.Linef("\tP.%s = %s;", Return.Property().Name().c_str(), Assigned.c_str());
            }
            else
            {
                Writer.Linef("\t%s;", Call.c_str());
            }
            Writer.Line("}");
            Writer.Line();
        }
    }

    void FCppEmitter::EmitFunctionTable(const FTypeView& Type)
    {
        if (!HasNativeFunctions(Type))
        {
            return;
        }

        size_t Count = 0;
        for (const FFunctionView Function : Type.Functions())
        {
            if (!Function.ReflectsNatively())
            {
                continue;
            }
            ++Count;
            if (!Function.Parameters().empty())
            {
                Writer.Linef("static const Lumina::FPropertyDesc %s[%zu];", Names::FunctionParamsTable(Function.Name()).c_str(), Function.Parameters().size());
                Writer.Line();
            }
        }

        Writer.Linef("static const Lumina::FFunctionDesc Functions[%zu];", Count);
        Writer.Line();
    }

    void FCppEmitter::DefineFunctionTable(const FTypeView& Type)
    {
        if (!HasNativeFunctions(Type))
        {
            return;
        }

        Writer.Linef("constexpr Lumina::FFunctionDesc %s::Functions[] =", TypeInfoName(Type).c_str());
        Writer.BeginBlock();
        for (const FFunctionView Function : Type.Functions())
        {
            if (!Function.ReflectsNatively())
            {
                continue;
            }

            std::string Flags = "Lumina::EFunctionFlags::None";
            if (Function.IsStatic())  { Flags += " | Lumina::EFunctionFlags::Static"; }
            if (Function.IsConst())   { Flags += " | Lumina::EFunctionFlags::Const"; }
            if (Function.IsVirtual()) { Flags += " | Lumina::EFunctionFlags::Virtual"; }
            for (const FMetadataView Pair : Function.Metadata())
            {
                if (Pair.Key() == "Rpc")
                {
                    if (Pair.Value() == "Broadcast")  { Flags += " | Lumina::EFunctionFlags::RpcBroadcast"; }
                    else if (Pair.Value() == "Host")  { Flags += " | Lumina::EFunctionFlags::RpcHost"; }
                    else if (Pair.Value() == "Owner") { Flags += " | Lumina::EFunctionFlags::RpcOwner"; }
                }
                else if (Pair.Key() == "Bind")
                {
                    Flags += " | Lumina::EFunctionFlags::UIBind";
                }
                else if (Pair.Key() == "NetFlags")
                {
                    if (Pair.Value().find("Unreliable") != std::string::npos) { Flags += " | Lumina::EFunctionFlags::NetUnreliable"; }
                    if (Pair.Value().find("OwnerOnly") != std::string::npos)  { Flags += " | Lumina::EFunctionFlags::NetOwnerOnly"; }
                }
            }

            const std::string& Name = Function.Name();
            const std::string ParamsTable = Names::FunctionParamsTable(Name);
            const std::string Params = Function.Parameters().empty()
                ? std::string()
                : (", .Params = " + ParamsTable + ", .NumParams = (Lumina::uint16)std::size(" + ParamsTable + ")");

            Writer.Linef("{ .Name = \"%s\", .Flags = %s%s, .ReturnIndex = %d, .FrameSize = (Lumina::uint16)sizeof(%s), .Thunk = &%s%s },",
                Name.c_str(), Flags.c_str(), Params.c_str(), Function.ReturnIndex(),
                Names::FunctionParmsStruct(Type.DisplayName(), Name).c_str(),
                Names::FunctionThunk(Type.DisplayName(), Name).c_str(),
                MetadataField("Func_" + Name, Function.Metadata(), ESpecifierTarget::Function).c_str());
        }
        Writer.PopIndent();
        Writer.Line("};");
        Writer.Line();
    }

    void FCppEmitter::DefineFunctionParameterTables(const FTypeView& Type)
    {
        for (const FFunctionView Function : Type.Functions())
        {
            if (!Function.ReflectsNatively() || Function.Parameters().empty())
            {
                continue;
            }

            Writer.Linef("constexpr Lumina::FPropertyDesc %s::%s[] =", TypeInfoName(Type).c_str(), Names::FunctionParamsTable(Function.Name()).c_str());
            Writer.BeginBlock();
            for (const FParameterView Parameter : Function.Parameters())
            {
                EmitDescriptor(Parameter.Property());
            }
            Writer.PopIndent();
            Writer.Line("};");
            Writer.Line();
        }
    }

    void FCppEmitter::DefineInitialHeader(const FTypeView& Type, const std::string& FileID)
    {
        const std::string Api = Names::ProjectApiMacro(Type.Module().Name());
        const std::string& Namespace = Type.Namespace();

        switch (Type.Kind())
        {
        case ETypeKind::Enum:
        {
            // At global scope this would create a second distinct type and any TU seeing both gets C2872.
            const char* Underlying = UnderlyingTypeName(Type.UnderlyingSize(), Type.IsUnderlyingUnsigned());
            if (!Namespace.empty())
            {
                Writer.Linef("namespace %s { enum class %s : %s; }", Namespace.c_str(), Type.DisplayName().c_str(), Underlying);
            }
            else
            {
                Writer.Linef("enum class %s : %s;", Type.DisplayName().c_str(), Underlying);
            }
            Writer.Linef("template<> %s Lumina::CEnum* Lumina::StaticEnum<%s>();", Api.c_str(), Type.QualifiedName().c_str());
            Writer.Line();
            break;
        }
        case ETypeKind::Struct:
        {
            // An alias name belongs to the using, which is declared after this header is included, so it gets no forward declaration.
            if (Type.IsAlias())
            {
                return;
            }
            if (!Namespace.empty())
            {
                Writer.Linef("namespace %s { struct %s; }", Namespace.c_str(), Type.CppName().c_str());
            }
            else
            {
                Writer.Linef("struct %s;", Type.CppName().c_str());
            }
            Writer.Linef("template<> %s Lumina::CStruct* Lumina::StaticStruct<%s>();", Api.c_str(), Type.CppQualifiedName().c_str());
            break;
        }
        case ETypeKind::Class:
        {
            const std::string Package = Names::TypePackage(Type.Module().Name());
            const char* DisplayName = Type.DisplayName().c_str();
            if (!Namespace.empty())
            {
                Writer.Linef("namespace %s { class %s; }", Namespace.c_str(), DisplayName);
            }
            else
            {
                Writer.Linef("\tclass %s;", DisplayName);
            }

            Writer.Linef("template<> %s Lumina::CClass* Lumina::StaticClass<%s>();", Api.c_str(), Type.CppQualifiedName().c_str());

            Writer.Linef("#define %s_%u_CLASS \\", FileID.c_str(), Type.Location().Line);
            Writer.Macro("public:");

            // Exports only GetPrivateStaticClass() so the type is referenceable without force-exporting members.
            const char* ClassApi = Type.HasMetadata("MinimalAPI") ? Api.c_str() : "NO_API";
            Writer.Macrof("\tDECLARE_CLASS(%s, %s, %s, \"%s\", %s)", Namespace.c_str(), DisplayName, Type.Parent().c_str(), Package.c_str(), ClassApi);
            Writer.Macrof("\tDEFINE_CLASS_FACTORY(%s::%s)", Namespace.c_str(), DisplayName);
            Writer.Macrof("\tDECLARE_SERIALIZER(%s, %s)", Namespace.c_str(), DisplayName);
            Writer.FinalizeMacro();
            Writer.BlankLines(2);
            break;
        }
        }
    }

    void FCppEmitter::DefineSecondaryHeader(const FTypeView& Type, const std::string& FileID)
    {
        switch (Type.Kind())
        {
        case ETypeKind::Enum:
            // Enums have no GENERATED_BODY, since StaticEnum is a specialization declared in the header.
            break;
        case ETypeKind::Struct:
        {
            // An alias has no struct body to inject GENERATED_BODY into.
            if (Type.IsAlias())
            {
                Writer.BlankLines(2);
                return;
            }

            Writer.Linef("#define %s_%u_GENERATED_BODY \\", FileID.c_str(), Type.GeneratedBodyLine());
            Writer.Macrof("friend struct %s;", TypeInfoName(Type).c_str());

            // Exports StaticStruct() so the type is referenceable without force-exporting every member.
            if (Type.HasMetadata("MinimalAPI"))
            {
                Writer.Macrof("static %s class Lumina::CStruct* StaticStruct();", Names::ProjectApiMacro(Type.Module().Name()).c_str());
            }
            else
            {
                Writer.Macro("static class Lumina::CStruct* StaticStruct();");
            }

            if (!Type.Parent().empty())
            {
                Writer.Macrof("using Super = %s::%s;", Type.Namespace().c_str(), Type.Parent().c_str());
            }

            Writer.FinalizeMacro();
            Writer.BlankLines(2);
            break;
        }
        case ETypeKind::Class:
            Writer.Linef("#define %s_%u_GENERATED_BODY \\", FileID.c_str(), Type.GeneratedBodyLine());
            Writer.Macrof("\t%s_%u_CLASS", FileID.c_str(), Type.Location().Line);
            Writer.Macro("private:");
            Writer.FinalizeMacro();
            Writer.BlankLines(2);
            break;
        }
    }

    void FCppEmitter::DeclareImplementation(const FTypeView& Type)
    {
        switch (Type.Kind())
        {
        case ETypeKind::Enum:   DeclareEnum(Type);   break;
        case ETypeKind::Struct: DeclareStruct(Type); break;
        case ETypeKind::Class:  DeclareClass(Type);  break;
        }
    }

    void FCppEmitter::DeclareEnum(const FTypeView& Type)
    {
        const std::string MetadataSymbol = Names::FriendlyFromQualified(Type.QualifiedName());

        OpenNamespace(Type);
        Writer.Linef("struct %s", TypeInfoName(Type).c_str());
        Writer.BeginBlock();

        EmitMetadataArray(MetadataSymbol, Type.Metadata(), ESpecifierTarget::Reflect);

        Writer.Line("static constexpr Lumina::FEnumeratorDesc Enumerators[] =");
        Writer.BeginBlock();
        for (const FEnumConstantView Constant : Type.Constants())
        {
            Writer.Linef("{ \"%s::%s\", %u },", Type.DisplayName().c_str(), Constant.Label().c_str(), Constant.Value());
        }
        Writer.PopIndent();
        Writer.Line("};");
        Writer.Line();

        Writer.Linef("static constexpr Lumina::FEnumDesc Desc = { .Name = \"%s\", .Enumerators = Enumerators, "
            ".NumEnumerators = (Lumina::uint32)std::size(Enumerators)%s };",
            Type.DisplayName().c_str(), MetadataField(MetadataSymbol, Type.Metadata(), ESpecifierTarget::Reflect).c_str());

        Writer.PopIndent();
        Writer.Line("};");
        CloseNamespace(Type);

        EmitTypeFunctions(Type);
    }

    void FCppEmitter::DeclareStruct(const FTypeView& Type)
    {
        const std::string& Cpp = Type.CppQualifiedName();
        const std::string& DisplayName = Type.DisplayName();

        const std::string OffsetBase = Names::OffsetBaseTypeName(Cpp, Type.Namespace(), DisplayName);
        if (OffsetBase != Cpp)
        {
            Writer.Linef("using %s = %s;", OffsetBase.c_str(), Cpp.c_str());
            Writer.Line();
        }

        Writer.Linef("// Begin %s", DisplayName.c_str());
        OpenNamespace(Type);
        EmitFunctionFrames(Type);

        Writer.Linef("struct %s", TypeInfoName(Type).c_str());
        Writer.BeginBlock();

        Writer.Linef("static Lumina::FStructOps* MakeOps() { return Lumina::MakeStructOps<%s>(); }", Cpp.c_str());
        Writer.Line();

        EmitMetadataArrays(Type);
        EmitPropertyTable(Type);
        EmitFunctionTable(Type);
        EmitLayoutAsserts(Type);

        const std::string Super = Type.Parent().empty() ? std::string() : (", .Super = &" + Cpp + "::Super::StaticStruct");
        Writer.Linef("static constexpr Lumina::FStructDesc Desc = { .Name = \"%s\"%s, .MakeOps = &MakeOps, "
            ".Size = sizeof(%s), .Alignment = alignof(%s)%s%s };",
            DisplayName.c_str(), Super.c_str(), Cpp.c_str(), Cpp.c_str(), MemberTableFields(Type).c_str(),
            MetadataField(Names::FriendlyFromQualified(Type.QualifiedName()), Type.Metadata(), ESpecifierTarget::Reflect).c_str());

        Writer.PopIndent();
        Writer.Line("};");
        DefinePropertyTable(Type);
        DefineFunctionParameterTables(Type);
        DefineFunctionTable(Type);
        CloseNamespace(Type);

        // Components register their ops and events their queue type, so both are reachable by name from a script.
        std::string Registration;
        if (Type.HasMetadata("Component"))
        {
            Registration = "Lumina::Meta::RegisterComponentMeta<" + Cpp + ">();";
        }
        else if (Type.HasMetadata("Event"))
        {
            Registration = "Lumina::Meta::RegisterEventMeta<" + Cpp + ">();";
        }
        else if (Type.IsResource())
        {
            Registration = "Lumina::Meta::RegisterResourceMeta<" + Cpp + ">();";
        }
        EmitTypeFunctions(Type, Registration);

        // An alias has no StaticStruct member to define.
        if (!Type.IsAlias())
        {
            Writer.Linef("Lumina::CStruct* %s::StaticStruct()", Cpp.c_str());
            Writer.Line("{");
            Writer.Linef("\treturn Lumina::Generated::%s();", TypeFunctionName(Type).c_str());
            Writer.Line("}");
        }

        Writer.Linef("//~ End %s", DisplayName.c_str());
        Writer.Line();
    }

    void FCppEmitter::DeclareClass(const FTypeView& Type)
    {
        const std::string& DisplayName = Type.DisplayName();
        Writer.Linef("// Begin %s", DisplayName.c_str());
        Writer.Linef("IMPLEMENT_CLASS(%s, %s)", Type.Namespace().c_str(), DisplayName.c_str());
        Writer.Line();

        OpenNamespace(Type);
        EmitFunctionFrames(Type);

        Writer.Linef("struct %s", TypeInfoName(Type).c_str());
        Writer.BeginBlock();

        EmitMetadataArrays(Type);
        EmitPropertyTable(Type);
        EmitFunctionTable(Type);
        EmitLayoutAsserts(Type);

        Writer.Linef("static constexpr Lumina::FClassDesc Desc = { .StaticClass = &%s::StaticClass%s%s };",
            Type.CppQualifiedName().c_str(), MemberTableFields(Type).c_str(),
            MetadataField(Names::FriendlyFromQualified(Type.QualifiedName()), Type.Metadata(), ESpecifierTarget::Reflect).c_str());

        Writer.PopIndent();
        Writer.Line("};");
        DefinePropertyTable(Type);
        DefineFunctionParameterTables(Type);
        DefineFunctionTable(Type);
        CloseNamespace(Type);

        EmitTypeFunctions(Type);

        Writer.Linef("//~ End %s", DisplayName.c_str());
        Writer.Line();
    }

    // Every StaticStruct, StaticClass and StaticEnum the traits reach, declared before that first use.
    void FCppEmitter::EmitTypeLinks(const TViewRange<FTypeView>& Types)
    {
        auto Declare = [&](const FTypeView& Type)
        {
            const std::string Api = Names::ProjectApiMacro(Type.Module().Name());
            const std::string Friendly = Names::FriendlyFromParts(Type.Namespace(), Type.DisplayName());
            EmitGuardedDeclaration(Writer, std::string(AccessorName(Type)) + "_" + Friendly,
                "template<> " + Api + " Lumina::" + RuntimeTypeName(Type) + "* Lumina::" + AccessorName(Type) + "<" + Type.CppQualifiedName() + ">();");
        };

        auto DeclareFor = [&](const FPropertyView& Property)
        {
            if (Property.Link() == EReflectedLink::None || Property.TypeName().empty())
            {
                return;
            }
            if (const FTypeView Linked = Property.GetModel().FindType(Property.TypeName()))
            {
                Declare(Linked);
            }
        };

        for (const FTypeView Type : Types)
        {
            Declare(Type);
            for (const FPropertyView Property : Type.Properties())
            {
                DeclareFor(Property);
            }
            for (const FFunctionView Function : Type.Functions())
            {
                for (const FPropertyView Parameter : Function.ParameterProperties())
                {
                    DeclareFor(Parameter);
                }
            }
        }

        Writer.Line();
    }

    void FCppReflectionGenerator::ClaimSpecifiers(FSpecifierRegistry& Registry)
    {
        for (const ESpecifierConsumer Consumer : { ESpecifierConsumer::Reflector, ESpecifierConsumer::Runtime, ESpecifierConsumer::Editor })
        {
            ClaimBuiltinSpecifiers(Registry, Consumer);
        }
    }

    void FCppReflectionGenerator::GenerateModule(const FModuleContext& Context)
    {
        std::vector<FHeaderView> Dirty;
        std::vector<std::string> SourceNames;
        std::unordered_set<std::string> Expected;

        // A free-function-only header needs a generated .cpp but no .generated.h.
        for (const FHeaderView Header : Context.Module.Headers())
        {
            if (Header.Types().empty() && Header.FreeFunctions().empty())
            {
                continue;
            }

            SourceNames.push_back(Header.FileName() + ".generated.cpp");
            Expected.insert(SourceNames.back());
            if (!Header.Types().empty())
            {
                Expected.insert(Header.FileName() + ".generated.h");
            }
            if (Header.IsDirty())
            {
                Dirty.push_back(Header);
            }
        }

        const FSpecifierRegistry& Specifiers = Context.Run.Specifiers;
        Context.Run.Workers.ParallelFor(Dirty.size(), [&](size_t Index)
        {
            const FHeaderView Header = Dirty[Index];
            if (!Header.Types().empty())
            {
                WriteHeaderContent(Context.Outputs.Open(GeneratedFiles::HeaderPath(Header)), Specifiers, Header);
            }
            WriteSourceContent(Context.Outputs.Open(GeneratedFiles::SourcePath(Header)), Specifiers, Header);
        });

        WriteUnityBuildFiles(Context.Outputs, Context.Module, std::move(SourceNames));
        GeneratedFiles::SweepOrphans(Context.Outputs, GeneratedFiles::ModuleDir(Context.Module), Expected, { ".generated.cpp", ".generated.h" });
    }
}
