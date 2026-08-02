#pragma once

#include "Containers/String.h"
#include "Platform/GenericPlatform.h"

#include <initializer_list>

namespace Lumina::DotNet
{
    // The kind of value an FManagedValue carries across the boundary - a minimal, blittable-friendly set
    // matching the script value-blob wire format.
    enum class EManagedValueKind : uint8
    {
        Void,
        Bool,
        Int,
        Double,
        String,
    };

    // A boxed argument / return value for a dynamic managed call. Constructs implicitly from the common
    // scalar + string types so a call site reads naturally: Object.Invoke("Add", { 2, 3 }).
    class FManagedValue
    {
    public:

        FManagedValue() = default;
        FManagedValue(bool Value)           : Kind(EManagedValueKind::Bool),   IntValue(Value ? 1 : 0) {}
        FManagedValue(int32 Value)          : Kind(EManagedValueKind::Int),    IntValue(Value) {}
        FManagedValue(uint32 Value)         : Kind(EManagedValueKind::Int),    IntValue((int64)Value) {}
        FManagedValue(int64 Value)          : Kind(EManagedValueKind::Int),    IntValue(Value) {}
        FManagedValue(uint64 Value)         : Kind(EManagedValueKind::Int),    IntValue((int64)Value) {}
        FManagedValue(float Value)          : Kind(EManagedValueKind::Double), DoubleValue(Value) {}
        FManagedValue(double Value)         : Kind(EManagedValueKind::Double), DoubleValue(Value) {}
        FManagedValue(const char* Value)    : Kind(EManagedValueKind::String), StringValue(Value) {}
        FManagedValue(FStringView Value)    : Kind(EManagedValueKind::String), StringValue(Value.data(), Value.size()) {}
        FManagedValue(const FString& Value) : Kind(EManagedValueKind::String), StringValue(Value) {}

        EManagedValueKind GetKind() const { return Kind; }
        bool IsVoid() const { return Kind == EManagedValueKind::Void; }

        // Forgiving numeric getters: Int and Double interconvert, so AsInt() on a Double truncates.
        bool   AsBool() const { return Kind == EManagedValueKind::Double ? (DoubleValue != 0.0) : (IntValue != 0); }
        int32  AsInt() const { return (int32)AsInt64(); }
        int64  AsInt64() const { return Kind == EManagedValueKind::Double ? (int64)DoubleValue : IntValue; }
        float  AsFloat() const { return (float)AsDouble(); }
        double AsDouble() const { return Kind == EManagedValueKind::Int ? (double)IntValue : DoubleValue; }
        const FString& AsString() const { return StringValue; }

    private:

        EManagedValueKind Kind = EManagedValueKind::Void;
        int64             IntValue = 0;
        double            DoubleValue = 0.0;
        FString           StringValue;
    };

    class FManagedObject;

    // A resolved managed Type. Construct from a full type name ("Game.HelloScript", "LuminaSharp.FAABB");
    // IsValid() reports whether the lookup hit.
    class FManagedClass
    {
    public:

        FManagedClass() = default;
        RUNTIME_API explicit FManagedClass(FStringView TypeName);
        RUNTIME_API ~FManagedClass();

        FManagedClass(FManagedClass&& Other) noexcept : TypeHandle(Other.TypeHandle) { Other.TypeHandle = nullptr; }
        RUNTIME_API FManagedClass& operator=(FManagedClass&& Other) noexcept;
        FManagedClass(const FManagedClass&) = delete;
        FManagedClass& operator=(const FManagedClass&) = delete;

        bool IsValid() const { return TypeHandle != nullptr; }
        void* GetHandle() const { return TypeHandle; }

        // Default-constructs an instance (parameterless ctor); the result is invalid on failure.
        RUNTIME_API FManagedObject New();

        // Invokes a static method by name; Void on failure or a void return.
        RUNTIME_API FManagedValue InvokeStatic(FStringView Method, std::initializer_list<FManagedValue> Args = {});

    private:

        void* TypeHandle = nullptr; // strong GCHandle to a System.Type
    };

    // A managed object instance, referenced by a strong GCHandle. RAII: the destructor releases the handle.
    class FManagedObject
    {
    public:

        FManagedObject() = default;
        explicit FManagedObject(void* InHandle) : Handle(InHandle) {}
        RUNTIME_API ~FManagedObject();

        FManagedObject(FManagedObject&& Other) noexcept : Handle(Other.Handle) { Other.Handle = nullptr; }
        RUNTIME_API FManagedObject& operator=(FManagedObject&& Other) noexcept;
        FManagedObject(const FManagedObject&) = delete;
        FManagedObject& operator=(const FManagedObject&) = delete;

        bool IsValid() const { return Handle != nullptr; }
        void* GetHandle() const { return Handle; }
        RUNTIME_API void Free();

        // Invokes an instance method by name (matched on name + argument count); Void on failure / void return.
        RUNTIME_API FManagedValue Invoke(FStringView Method, std::initializer_list<FManagedValue> Args = {});

        // Field-or-property access by name. SetField returns false when the member is missing.
        RUNTIME_API FManagedValue GetField(FStringView Name);
        RUNTIME_API bool SetField(FStringView Name, const FManagedValue& Value);

    private:

        void* Handle = nullptr; // strong GCHandle to the managed object
    };
}
