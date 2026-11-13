#pragma once

#include <cstdint>
#include <cstddef>
#include <deque>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Reflector/Public/PropertyFlags.h"
#include "Reflector/Public/ReflectorApi.h"

namespace Lumina::Reflection
{
    class FReflectionModel;
    class FReflectionModelBuilder;

    // Index into the model's string table, where zero is the empty string.
    using FStringId = uint32_t;

    inline constexpr uint32_t kInvalidIndex = UINT32_MAX;

    struct FIndexRange
    {
        uint32_t First = 0;
        uint32_t Count = 0;
    };

    struct FSourceLocation
    {
        FStringId File = 0;
        uint32_t  Line = 0;
    };

    enum class ETypeKind : uint8_t
    {
        Class,
        Struct,
        Enum,
    };

    // The runtime property kind; Array, Map and Optional carry their element spellings as type arguments.
    enum class EPropertyKind : uint8_t
    {
        UInt8, UInt16, UInt32, UInt64, Int8, Int16, Int32, Int64,
        Entity, Float, Double, Bool, String, Name,
        Struct, Enum, Class, Object, SoftObject, SubStruct, ComponentRef, InstancedStruct, Delegate,
        Array, Map, Optional,
    };

    // The spelling the runtime and the bindings key a kind by, or null for a container.
    LRT_API const char* PropertyKindName(EPropertyKind Kind);

    struct FMetadataRecord
    {
        FStringId Key   = 0;
        FStringId Value = 0;
    };

    struct FModuleRecord
    {
        FStringId   Name = 0;
        FStringId   Path = 0;
        FStringId   CSharpBindingsDir = 0;
        FStringId   GeneratedDir = 0;
        FStringId   PrecompiledHeader = 0;
        FIndexRange IncludeDirs;
        FIndexRange Headers;
        bool        bRouteTypeBindings = false;
        bool        bReferenceOnly = false;
    };

    struct FHeaderRecord
    {
        uint32_t    Module = kInvalidIndex;
        FStringId   FileName = 0;
        FStringId   Path = 0;
        FIndexRange Types;
        FIndexRange FreeFunctions;
        bool        bDirty = false;
    };

    struct FTypeRecord
    {
        ETypeKind       Kind = ETypeKind::Struct;
        uint32_t        Header = kInvalidIndex;
        FStringId       DisplayName = 0;
        FStringId       CppName = 0;
        FStringId       CppQualifiedName = 0;
        FStringId       QualifiedName = 0;
        FStringId       Namespace = 0;
        FStringId       Parent = 0;
        FSourceLocation Location;
        uint32_t        GeneratedBodyLine = 0;
        uint32_t        UnderlyingSize = 4;
        int64_t         NativeSize = -1;
        int64_t         NativeAlign = -1;
        FIndexRange     Properties;
        FIndexRange     Functions;
        FIndexRange     Metadata;
        FIndexRange     Constants;
        bool            bUnsignedUnderlying = false;
        bool            bHasUnreflectedFields = false;
        bool            bIsAlias = false;
        bool            bIsResource = false;
    };

    struct FPropertyRecord
    {
        EPropertyKind   Kind = EPropertyKind::Int32;
        EReflectedLink  Link = EReflectedLink::None;
        EPropertyFlags  Flags = EPropertyFlags::None;
        bool            bInner = false;
        FStringId       Name = 0;
        FStringId       TypeName = 0;
        FStringId       RawTypeName = 0;
        FStringId       Outer = 0;
        FStringId       Getter = 0;
        FStringId       Setter = 0;
        FStringId       ReflectedAsType = 0;
        FIndexRange     TypeArguments;
        FIndexRange     Metadata;
        int64_t         NativeOffset = -1;
        FSourceLocation Location;
    };

    struct FArgumentRecord
    {
        EPropertyTypeFlags TypeFlags = EPropertyTypeFlags::None;
        FStringId          Name = 0;
        FStringId          TypeName = 0;
        FStringId          RawTypeName = 0;
        FStringId          DefaultValue = 0;
    };

    // A member of a natively reflected function's frame struct.
    struct FParameterRecord
    {
        uint32_t  Property = kInvalidIndex;
        FStringId StorageType = 0;
        FStringId ObjectCastType = 0;
        bool      bRvalue = false;
    };

    struct FFunctionRecord
    {
        FStringId       Name = 0;
        FStringId       Outer = 0;
        FStringId       QualifiedName = 0;
        FStringId       CSharpTarget = 0;
        FSourceLocation Location;
        uint32_t        Return = kInvalidIndex;
        int32_t         ReturnIndex = -1;
        FIndexRange     Arguments;
        FIndexRange     ParameterProperties;
        FIndexRange     Parameters;
        FIndexRange     Metadata;
        bool            bHasOmittedArgs = false;
        bool            bIsVirtual = false;
        bool            bIsStatic = false;
        bool            bIsConst = false;
        bool            bReflectNatively = false;
        bool            bFreeFunction = false;
    };

    struct FEnumConstantRecord
    {
        FStringId Id = 0;
        FStringId Label = 0;
        FStringId Description = 0;
        uint32_t  Value = 0;
    };

    // A handle into one of the model's arrays, cheap to copy and valid for the life of the model.
    template<typename TRecord>
    class TModelView
    {
    public:

        TModelView() = default;
        TModelView(const FReflectionModel* InModel, uint32_t InIndex) : Model(InModel), Index(InIndex) {}

        explicit operator bool() const { return Model != nullptr && Index != kInvalidIndex; }
        bool operator==(const TModelView& Other) const { return Model == Other.Model && Index == Other.Index; }

        uint32_t GetIndex() const { return Index; }
        const FReflectionModel& GetModel() const { return *Model; }
        const TRecord& Record() const;

    protected:

        const std::string& Str(FStringId Id) const;

        const FReflectionModel* Model = nullptr;
        uint32_t                Index = kInvalidIndex;
    };

    // Declared before any view instantiates Record, since a specialization after that point is ill-formed.
    template<> const FModuleRecord& TModelView<FModuleRecord>::Record() const;
    template<> const FHeaderRecord& TModelView<FHeaderRecord>::Record() const;
    template<> const FTypeRecord& TModelView<FTypeRecord>::Record() const;
    template<> const FPropertyRecord& TModelView<FPropertyRecord>::Record() const;
    template<> const FFunctionRecord& TModelView<FFunctionRecord>::Record() const;
    template<> const FArgumentRecord& TModelView<FArgumentRecord>::Record() const;
    template<> const FParameterRecord& TModelView<FParameterRecord>::Record() const;
    template<> const FEnumConstantRecord& TModelView<FEnumConstantRecord>::Record() const;
    template<> const FMetadataRecord& TModelView<FMetadataRecord>::Record() const;

    template<typename TView>
    class TViewRange
    {
    public:

        class FIterator
        {
        public:
            using iterator_category = std::forward_iterator_tag;
            using value_type        = TView;
            using difference_type   = std::ptrdiff_t;
            using pointer           = void;
            using reference         = TView;

            FIterator(const FReflectionModel* InModel, uint32_t InIndex) : Model(InModel), Index(InIndex) {}
            TView operator*() const { return TView(Model, Index); }
            FIterator& operator++() { ++Index; return *this; }
            bool operator==(const FIterator& Other) const { return Index == Other.Index; }
        private:
            const FReflectionModel* Model;
            uint32_t                Index;
        };

        TViewRange(const FReflectionModel* InModel, FIndexRange InRange) : Model(InModel), Range(InRange) {}

        FIterator begin() const { return FIterator(Model, Range.First); }
        FIterator end() const { return FIterator(Model, Range.First + Range.Count); }
        size_t size() const { return Range.Count; }
        bool empty() const { return Range.Count == 0; }
        TView operator[](size_t Offset) const { return TView(Model, Range.First + static_cast<uint32_t>(Offset)); }

    private:

        const FReflectionModel* Model;
        FIndexRange             Range;
    };

    class FMetadataView : public TModelView<FMetadataRecord>
    {
    public:
        using TModelView::TModelView;
        const std::string& Key() const { return Str(Record().Key); }
        const std::string& Value() const { return Str(Record().Value); }
    };

    using FMetadataRange = TViewRange<FMetadataView>;

    // The value stored under Key, or null when there is none.
    LRT_API const std::string* FindMetadata(const FMetadataRange& Metadata, std::string_view Key);

    class FModuleView;
    class FHeaderView;
    class FTypeView;
    class FFunctionView;

    class FPropertyView : public TModelView<FPropertyRecord>
    {
    public:
        using TModelView::TModelView;
        EPropertyKind Kind() const { return Record().Kind; }
        EReflectedLink Link() const { return Record().Link; }
        EPropertyFlags Flags() const { return Record().Flags; }
        bool IsInner() const { return Record().bInner; }
        const std::string& Name() const { return Str(Record().Name); }
        const std::string& TypeName() const { return Str(Record().TypeName); }
        const std::string& RawTypeName() const { return Str(Record().RawTypeName); }
        const std::string& Outer() const { return Str(Record().Outer); }
        const std::string& Getter() const { return Str(Record().Getter); }
        const std::string& Setter() const { return Str(Record().Setter); }
        const std::string& ReflectedAsType() const { return Str(Record().ReflectedAsType); }
        int64_t NativeOffset() const { return Record().NativeOffset; }
        FSourceLocation Location() const { return Record().Location; }
        FMetadataRange Metadata() const;
        bool HasMetadata(std::string_view Key) const { return FindMetadata(Metadata(), Key) != nullptr; }

        // Element of an array or optional, key then value of a map, argument types of a delegate.
        std::span<const FStringId> TypeArguments() const;
        const std::string& TypeArgument(size_t Index) const { return Str(TypeArguments()[Index]); }
    };

    class FArgumentView : public TModelView<FArgumentRecord>
    {
    public:
        using TModelView::TModelView;
        EPropertyTypeFlags TypeFlags() const { return Record().TypeFlags; }
        const std::string& Name() const { return Str(Record().Name); }
        const std::string& TypeName() const { return Str(Record().TypeName); }
        const std::string& RawTypeName() const { return Str(Record().RawTypeName); }
        const std::string& DefaultValue() const { return Str(Record().DefaultValue); }
    };

    class FParameterView : public TModelView<FParameterRecord>
    {
    public:
        using TModelView::TModelView;
        FPropertyView Property() const { return FPropertyView(Model, Record().Property); }
        const std::string& StorageType() const { return Str(Record().StorageType); }
        const std::string& ObjectCastType() const { return Str(Record().ObjectCastType); }
        bool IsRvalue() const { return Record().bRvalue; }
    };

    class FFunctionView : public TModelView<FFunctionRecord>
    {
    public:
        using TModelView::TModelView;
        const std::string& Name() const { return Str(Record().Name); }
        const std::string& Outer() const { return Str(Record().Outer); }
        const std::string& QualifiedName() const { return Str(Record().QualifiedName); }
        const std::string& CSharpTarget() const { return Str(Record().CSharpTarget); }
        FSourceLocation Location() const { return Record().Location; }
        bool HasReturn() const { return Record().Return != kInvalidIndex; }
        FArgumentView Return() const { return FArgumentView(Model, Record().Return); }
        TViewRange<FArgumentView> Arguments() const { return { Model, Record().Arguments }; }

        // Every property describing the native frame, a container's inners just before it.
        TViewRange<FPropertyView> ParameterProperties() const { return { Model, Record().ParameterProperties }; }

        // The frame's members in declaration order, the return last when there is one.
        TViewRange<FParameterView> Parameters() const { return { Model, Record().Parameters }; }
        int32_t ReturnIndex() const { return Record().ReturnIndex; }
        FMetadataRange Metadata() const { return { Model, Record().Metadata }; }
        bool HasMetadata(std::string_view Key) const { return FindMetadata(Metadata(), Key) != nullptr; }
        bool HasOmittedArgs() const { return Record().bHasOmittedArgs; }
        bool IsVirtual() const { return Record().bIsVirtual; }
        bool IsStatic() const { return Record().bIsStatic; }
        bool IsConst() const { return Record().bIsConst; }
        bool ReflectsNatively() const { return Record().bReflectNatively; }
        bool IsFreeFunction() const { return Record().bFreeFunction; }
    };

    class FEnumConstantView : public TModelView<FEnumConstantRecord>
    {
    public:
        using TModelView::TModelView;
        const std::string& Id() const { return Str(Record().Id); }
        const std::string& Label() const { return Str(Record().Label); }
        const std::string& Description() const { return Str(Record().Description); }
        uint32_t Value() const { return Record().Value; }
    };

    class FTypeView : public TModelView<FTypeRecord>
    {
    public:
        using TModelView::TModelView;
        ETypeKind Kind() const { return Record().Kind; }
        FHeaderView Header() const;
        FModuleView Module() const;
        const std::string& DisplayName() const { return Str(Record().DisplayName); }
        const std::string& QualifiedName() const { return Str(Record().QualifiedName); }
        const std::string& Namespace() const { return Str(Record().Namespace); }
        const std::string& Parent() const { return Str(Record().Parent); }

        // The real C++ identifier, which differs from DisplayName when ReflectedName aliases the type.
        const std::string& CppName() const { return Str(Record().CppName); }
        const std::string& CppQualifiedName() const { return Str(Record().CppQualifiedName); }
        FSourceLocation Location() const { return Record().Location; }
        uint32_t GeneratedBodyLine() const { return Record().GeneratedBodyLine; }
        int64_t NativeSize() const { return Record().NativeSize; }
        int64_t NativeAlign() const { return Record().NativeAlign; }
        uint32_t UnderlyingSize() const { return Record().UnderlyingSize; }
        bool IsUnderlyingUnsigned() const { return Record().bUnsignedUnderlying; }
        bool HasUnreflectedFields() const { return Record().bHasUnreflectedFields; }
        bool IsAlias() const { return Record().bIsAlias; }
        bool IsResource() const { return Record().bIsResource; }
        TViewRange<FPropertyView> Properties() const { return { Model, Record().Properties }; }
        TViewRange<FFunctionView> Functions() const { return { Model, Record().Functions }; }
        TViewRange<FEnumConstantView> Constants() const { return { Model, Record().Constants }; }
        FMetadataRange Metadata() const { return { Model, Record().Metadata }; }
        const std::string* FindMetadata(std::string_view Key) const { return Reflection::FindMetadata(Metadata(), Key); }
        bool HasMetadata(std::string_view Key) const { return FindMetadata(Key) != nullptr; }
    };

    class FHeaderView : public TModelView<FHeaderRecord>
    {
    public:
        using TModelView::TModelView;
        FModuleView Module() const;
        const std::string& FileName() const { return Str(Record().FileName); }
        const std::string& Path() const { return Str(Record().Path); }
        bool IsDirty() const { return Record().bDirty; }
        TViewRange<FTypeView> Types() const { return { Model, Record().Types }; }
        TViewRange<FFunctionView> FreeFunctions() const { return { Model, Record().FreeFunctions }; }
    };

    // The spelling a generated file includes Header by, relative to its module's include paths when one holds it.
    LRT_API std::string HeaderIncludePath(const FHeaderView& Header);

    class FModuleView : public TModelView<FModuleRecord>
    {
    public:
        using TModelView::TModelView;
        const std::string& Name() const { return Str(Record().Name); }
        const std::string& Path() const { return Str(Record().Path); }
        const std::string& CSharpBindingsDir() const { return Str(Record().CSharpBindingsDir); }
        const std::string& GeneratedDir() const { return Str(Record().GeneratedDir); }
        const std::string& PrecompiledHeader() const { return Str(Record().PrecompiledHeader); }
        std::span<const FStringId> IncludeDirs() const;
        bool RoutesTypeBindings() const { return Record().bRouteTypeBindings; }

        // Parsed so other modules can name its types, and generates nothing itself.
        bool IsReferenceOnly() const { return Record().bReferenceOnly; }
        TViewRange<FHeaderView> Headers() const { return { Model, Record().Headers }; }
    };

    // Everything one run reflected, as flat arrays and interned strings, immutable once built.
    class LRT_API FReflectionModel
    {
    public:

        const std::string& String(FStringId Id) const { return Strings[Id]; }

        // The id of an interned string, or kInvalidIndex when no record names it.
        FStringId FindString(std::string_view Text) const;

        // The root the default Intermediates/Reflection and Intermediates/CSharpBindings directories live under.
        const std::string& WorkspacePath() const { return String(WorkspaceDir); }

        TViewRange<FModuleView> Modules() const { return { this, { 0, static_cast<uint32_t>(ModuleRecords.size()) } }; }
        TViewRange<FTypeView> Types() const { return { this, { 0, static_cast<uint32_t>(TypeRecords.size()) } }; }

        // The type registered under QualifiedName, or an invalid view.
        FTypeView FindType(std::string_view QualifiedName) const;

        const std::vector<FModuleRecord>& GetModuleRecords() const { return ModuleRecords; }
        const std::vector<FHeaderRecord>& GetHeaderRecords() const { return HeaderRecords; }
        const std::vector<FTypeRecord>& GetTypeRecords() const { return TypeRecords; }
        const std::vector<FPropertyRecord>& GetPropertyRecords() const { return PropertyRecords; }
        const std::vector<FFunctionRecord>& GetFunctionRecords() const { return FunctionRecords; }
        const std::vector<FArgumentRecord>& GetArgumentRecords() const { return ArgumentRecords; }
        const std::vector<FParameterRecord>& GetParameterRecords() const { return ParameterRecords; }
        const std::vector<FEnumConstantRecord>& GetEnumConstantRecords() const { return EnumConstantRecords; }
        const std::vector<FMetadataRecord>& GetMetadataRecords() const { return MetadataRecords; }
        const std::vector<FStringId>& GetStringLists() const { return StringLists; }

    private:

        friend class FReflectionModelBuilder;

        // A deque, so the views the lookup map keys by never move.
        std::deque<std::string>                         Strings;
        std::unordered_map<std::string_view, FStringId> StringIds;
        std::unordered_map<FStringId, uint32_t>         TypesByName;

        FStringId                         WorkspaceDir = 0;
        std::vector<FModuleRecord>        ModuleRecords;
        std::vector<FHeaderRecord>        HeaderRecords;
        std::vector<FTypeRecord>          TypeRecords;
        std::vector<FPropertyRecord>      PropertyRecords;
        std::vector<FFunctionRecord>      FunctionRecords;
        std::vector<FArgumentRecord>      ArgumentRecords;
        std::vector<FParameterRecord>     ParameterRecords;
        std::vector<FEnumConstantRecord>  EnumConstantRecords;
        std::vector<FMetadataRecord>      MetadataRecords;
        std::vector<FStringId>            StringLists;
    };

    template<typename TRecord>
    const std::string& TModelView<TRecord>::Str(FStringId Id) const
    {
        return Model->String(Id);
    }

    template<> inline const FModuleRecord& TModelView<FModuleRecord>::Record() const { return Model->GetModuleRecords()[Index]; }
    template<> inline const FHeaderRecord& TModelView<FHeaderRecord>::Record() const { return Model->GetHeaderRecords()[Index]; }
    template<> inline const FTypeRecord& TModelView<FTypeRecord>::Record() const { return Model->GetTypeRecords()[Index]; }
    template<> inline const FPropertyRecord& TModelView<FPropertyRecord>::Record() const { return Model->GetPropertyRecords()[Index]; }
    template<> inline const FFunctionRecord& TModelView<FFunctionRecord>::Record() const { return Model->GetFunctionRecords()[Index]; }
    template<> inline const FArgumentRecord& TModelView<FArgumentRecord>::Record() const { return Model->GetArgumentRecords()[Index]; }
    template<> inline const FParameterRecord& TModelView<FParameterRecord>::Record() const { return Model->GetParameterRecords()[Index]; }
    template<> inline const FEnumConstantRecord& TModelView<FEnumConstantRecord>::Record() const { return Model->GetEnumConstantRecords()[Index]; }
    template<> inline const FMetadataRecord& TModelView<FMetadataRecord>::Record() const { return Model->GetMetadataRecords()[Index]; }

    inline FMetadataRange FPropertyView::Metadata() const { return { Model, Record().Metadata }; }

    inline std::span<const FStringId> FPropertyView::TypeArguments() const
    {
        return std::span<const FStringId>(Model->GetStringLists()).subspan(Record().TypeArguments.First, Record().TypeArguments.Count);
    }

    inline std::span<const FStringId> FModuleView::IncludeDirs() const
    {
        return std::span<const FStringId>(Model->GetStringLists()).subspan(Record().IncludeDirs.First, Record().IncludeDirs.Count);
    }

    inline FHeaderView FTypeView::Header() const { return FHeaderView(Model, Record().Header); }
    inline FModuleView FTypeView::Module() const { return Header().Module(); }
    inline FModuleView FHeaderView::Module() const { return FModuleView(Model, Record().Module); }
}
