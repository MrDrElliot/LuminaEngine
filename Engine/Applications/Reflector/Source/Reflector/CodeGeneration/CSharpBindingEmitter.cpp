#include "CSharpBindingEmitter.h"

#include <EASTL/algorithm.h>

#include <EASTL/unique_ptr.h>

#include "CodeWriter.h"
#include "Reflector/CodeGeneration/ReflectionNames.h"
#include "Reflector/ReflectionCore/ReflectedHeader.h"
#include "Reflector/ReflectionCore/ReflectedProject.h"
#include "Reflector/ReflectionCore/ReflectionDatabase.h"
#include "Reflector/Types/FieldInfo.h"
#include "Reflector/Types/Functions/ReflectedFunction.h"
#include "Reflector/Types/Properties/ReflectedArrayProperty.h"
#include "Reflector/Types/Properties/ReflectedDelegateProperty.h"
#include "Reflector/Types/PropertyFlags.h"
#include "Reflector/Types/ReflectedType.h"
#include "StringHash.h"

namespace Lumina::Reflection
{
    namespace
    {
        // Excluded from the C# API: explicit opt-out (NoCSharp/) or a manually-registered stub
        // (ManualStub, e.g. the math types) whose real layout the Reflector doesn't see, those are
        // hand-written in LuminaSharp instead.
        bool IsExcludedFromCSharp(const FReflectedType& Type)
        {
            return eastl::any_of(Type.Metadata.begin(), Type.Metadata.end(),
                [](const FMetadataPair& Pair)
                {
                    return Pair.Key == "NoCSharp" || Pair.Key == "ManualStub";
                });
        }

        bool HasMetadata(const FReflectedType& Type, const char* Key)
        {
            return eastl::any_of(Type.Metadata.begin(), Type.Metadata.end(),
                [Key](const FMetadataPair& Pair) { return Pair.Key == Key; });
        }

        // The C# base type to back a generated enum with, matching the native underlying integer's width and
        // signedness. Emitting it explicitly (e.g. `enum X : byte`) keeps a 1/2/8-byte enum the same size as
        // its C++ source so it can live inside a blittable by-value struct mirror.
        const char* CSharpEnumBackingType(const FReflectedEnum& Enum)
        {
            switch (Enum.UnderlyingSize)
            {
            case 1:  return Enum.bUnsignedUnderlying ? "byte"   : "sbyte";
            case 2:  return Enum.bUnsignedUnderlying ? "ushort" : "short";
            case 8:  return Enum.bUnsignedUnderlying ? "ulong"  : "long";
            case 4:
            default: return Enum.bUnsignedUnderlying ? "uint"   : "int";
            }
        }

        // The module a type is defined in ("Runtime", "Editor", "Sandbox", ...). Drives both the
        // native thunk's export macro (RUNTIME_API/EDITOR_API/...) and the C# P/Invoke library key,
        // so any module's reflected types can carry C# bindings, not just Runtime.
        eastl::string ModuleOf(const FReflectedType& Type)
        {
            if (Type.Header != nullptr && Type.Header->Project != nullptr)
            {
                return Type.Header->Project->Name;
            }
            return "Runtime";
        }

        // C++ "Lumina::FVector3" -> C# "Lumina.FVector3".
        eastl::string ToCSharpName(eastl::string Name)
        {
            for (size_t Pos = Name.find("::"); Pos != eastl::string::npos; Pos = Name.find("::"))
            {
                Name.replace(Pos, 2, ".");
            }
            return Name;
        }

        bool IsCSharpKeyword(const eastl::string& Word)
        {
            static const char* Keywords[] = {
                "abstract","as","base","bool","break","byte","case","catch","char","checked","class",
                "const","continue","decimal","default","delegate","do","double","else","enum","event",
                "explicit","extern","false","finally","fixed","float","for","foreach","goto","if",
                "implicit","in","int","interface","internal","is","lock","long","namespace","new","null",
                "object","operator","out","override","params","private","protected","public","readonly",
                "ref","return","sbyte","sealed","short","sizeof","stackalloc","static","string","struct",
                "switch","this","throw","true","try","typeof","uint","ulong","unchecked","unsafe","ushort",
                "using","virtual","void","volatile","while",
            };
            for (const char* K : Keywords)
            {
                if (Word == K)
                {
                    return true;
                }
            }
            return false;
        }

        eastl::string SafeIdentifier(const eastl::string& Name)
        {
            return IsCSharpKeyword(Name) ? ("@" + Name) : Name;
        }

        // Maps a property kind (FReflectedProperty::GetTypeName) to a C# primitive. Returns true with
        // the size/alignment for blittable primitives; false for non-primitive kinds.
        bool PrimitiveCSharp(const eastl::string& Kind, eastl::string& OutCSharp, int& OutSize, int& OutAlign)
        {
            struct FEntry { const char* Kind; const char* CSharp; int Size; int Align; };
            static const FEntry Table[] = {
                { "Int8",   "sbyte",  1, 1 }, { "UInt8",  "byte",   1, 1 }, { "Bool",   "bool",  1, 1 },
                { "Int16",  "short",  2, 2 }, { "UInt16", "ushort", 2, 2 },
                { "Int32",  "int",    4, 4 }, { "UInt32", "uint",   4, 4 }, { "Float",  "float", 4, 4 },
                { "Int64",  "long",   8, 8 }, { "UInt64", "ulong",  8, 8 }, { "Double", "double",8, 8 },
            };
            for (const FEntry& E : Table)
            {
                if (Kind == E.Kind)
                {
                    OutCSharp = E.CSharp;
                    OutSize = E.Size;
                    OutAlign = E.Align;
                    return true;
                }
            }
            return false;
        }

        int AlignUp(int Offset, int Align)
        {
            return (Offset + Align - 1) & ~(Align - 1);
        }

        // Computes the C# Sequential layout (== C++ layout) of a struct if every reflected field is
        // blittable (primitive / int-backed enum / nested blittable struct). Returns false otherwise.
        bool ComputeBlittableLayout(const FReflectedStruct& Struct, const FReflectionDatabase& Db, int& OutSize, int& OutAlign, int Depth)
        {
            if (Depth > 16)
            {
                return false;
            }

            int Offset = 0;
            int MaxAlign = 1;
            for (const auto& Prop : Struct.Props)
            {
                if (Prop->bInner)
                {
                    continue;
                }

                const char* KindPtr = Prop->GetTypeName();
                if (KindPtr == nullptr)
                {
                    return false; // Array / Optional report a null kind -> not blittable
                }
                const eastl::string Kind = KindPtr;
                int Size = 0;
                int Align = 0;
                eastl::string Unused;

                if (PrimitiveCSharp(Kind, Unused, Size, Align))
                {
                    // blittable primitive
                }
                else if (Kind == "Enum")
                {
                    // The generated C# enum carries an explicit backing type matching the native underlying
                    // width (EmitForEnum), so a reflected enum mirrors by value at any size. Use that width as
                    // the field's size/align; the auto-emitted static_assert(sizeof==N) keeps layout honest.
                    const FReflectedEnum* E = Db.GetReflectedType<FReflectedEnum>(FStringHash(Prop->TypeName));
                    if (E == nullptr)
                    {
                        return false;
                    }
                    Size = (int)E->UnderlyingSize;
                    Align = Size;
                }
                else if (Kind == "Struct")
                {
                    const FReflectedStruct* Nested = Db.GetReflectedType<FReflectedStruct>(FStringHash(Prop->TypeName));
                    int NestedSize = 0;
                    int NestedAlign = 0;
                    if (Nested == nullptr || !ComputeBlittableLayout(*Nested, Db, NestedSize, NestedAlign, Depth + 1))
                    {
                        return false;
                    }
                    Size = NestedSize;
                    Align = NestedAlign;
                }
                else
                {
                    return false; // String / Object / Array / etc. -> not blittable
                }

                Offset = AlignUp(Offset, Align) + Size;
                MaxAlign = (Align > MaxAlign) ? Align : MaxAlign;
            }

            OutAlign = MaxAlign;
            OutSize = AlignUp(Offset, MaxAlign);
            return true;
        }
        
        bool IsBlittableValueStruct(const FReflectedStruct& Struct, const FReflectionDatabase& Db, int& OutSize, int& OutAlign)
        {
            if (HasMetadata(Struct, "Component"))
            {
                return false; // write-through component wrapper (see EmitForStruct)
            }
            if (!Struct.Parent.empty())
            {
                return false; // a struct with a reflected base isn't a flat value mirror
            }
            if (Struct.bHasUnreflectedFields)
            {
                return false; // hidden non-PROPERTY state: reflected layout is incomplete, can't blit by value
            }
            // An empty reflected struct computes a zero-size mirror, which can never match C++ sizeof
            // >= 1. Treat it as opaque.
            return ComputeBlittableLayout(Struct, Db, OutSize, OutAlign, 0) && OutSize > 0;
        }

        // An entt::entity / FEntity field (the reflector classifies it as Int32, so it stays a 4-byte
        // blittable mirror field) surfaces as the C# Entity handle, same mapping the function path uses.
        bool IsEntityField(const FReflectedProperty& Prop)
        {
            return Prop.RawTypeName.find("entt::entity") != eastl::string::npos
                || Prop.TypeName.find("entt::entity")    != eastl::string::npos
                || Prop.RawTypeName.find("FEntity")      != eastl::string::npos
                || Prop.TypeName.find("FEntity")         != eastl::string::npos;
        }

        // The C# field type for a blittable struct member.
        eastl::string CSharpFieldType(FReflectedProperty& Prop)
        {
            if (IsEntityField(Prop))
            {
                return "global::LuminaSharp.Entity";
            }
            const eastl::string Kind = Prop.GetTypeName();
            eastl::string CSharp;
            int Size = 0;
            int Align = 0;
            if (PrimitiveCSharp(Kind, CSharp, Size, Align))
            {
                return CSharp;
            }
            if (Kind == "Enum" || Kind == "Struct")
            {
                return ToCSharpName(Prop.TypeName);
            }
            return "int"; // unreachable for blittable structs
        }

        void OpenNamespace(FCodeWriter& Writer, const eastl::string& CppNamespace, bool& bOpened)
        {
            const eastl::string Ns = ToCSharpName(CppNamespace);
            bOpened = !Ns.empty();
            if (bOpened)
            {
                Writer.Linef("namespace %s", Ns.c_str());
                Writer.BeginBlock();
            }
        }

        // C# wrapper read-only, decoupled from the editor flags: ScriptReadOnly forces getter-only even on
        // an editor-editable property; ScriptWritable forces a setter even on an editor ReadOnly/Const one;
        // otherwise the editor ReadOnly/Const flags apply (the historical behavior).
        bool IsReadOnlyProp(const FReflectedProperty& Prop)
        {
            if (EnumHasAnyFlags(Prop.PropertyFlags, EPropertyFlags::ScriptReadOnly))
            {
                return true;
            }
            if (EnumHasAnyFlags(Prop.PropertyFlags, EPropertyFlags::ScriptWritable))
            {
                return false;
            }
            return EnumHasAnyFlags(Prop.PropertyFlags, EPropertyFlags::ReadOnly | EPropertyFlags::Const);
        }

        // A property explicitly hidden from the C# layer (PROPERTY(ScriptHidden) / NotScriptable): no
        // wrapper member is emitted for it at all.
        bool IsScriptHidden(const FReflectedProperty& Prop)
        {
            return EnumHasAnyFlags(Prop.PropertyFlags, EPropertyFlags::ScriptHidden);
        }
        
        void EmitThunkField(FCodeWriter& Writer, const eastl::string& Module, const eastl::string& EntryPoint,
            const eastl::string& Fn, const eastl::string& ParamTypesCsv, const eastl::string& RetType)
        {
            const eastl::string Sig = ParamTypesCsv.empty() ? RetType : (ParamTypesCsv + ", " + RetType);
            Writer.Linef("private static readonly delegate* unmanaged[Cdecl]<%s> %s =", Sig.c_str(), Fn.c_str());
            Writer.Linef("    (delegate* unmanaged[Cdecl]<%s>)global::LuminaSharp.NativeBindings.Resolve(\"%s\", \"%s\");",
                Sig.c_str(), Module.c_str(), EntryPoint.c_str());
        }

        // A reflected type that exists in the database and is allowed in the C# API.
        bool IsExposed(const FReflectionDatabase& Db, const eastl::string& Qualified)
        {
            const FReflectedType* T = Db.GetReflectedType<FReflectedType>(FStringHash(Qualified));
            return T != nullptr && !IsExcludedFromCSharp(*T);
        }

        // "global::" + C# form of a qualified C++ name ("Lumina::CStaticMesh" -> "global::Lumina.CStaticMesh").
        eastl::string GlobalCSharp(const eastl::string& Qualified)
        {
            return "global::" + ToCSharpName(Qualified);
        }

        // A reflected type that becomes an opaque handle wrapper (a C# class deriving NativeObject/
        // NativeStruct), i.e. a class, or a struct that is NOT a blittable value mirror. Such a type
        // is a valid C# *base class* for another wrapper.
        bool IsOpaqueWrapperType(const FReflectionDatabase& Db, const eastl::string& Qualified)
        {
            const FReflectedType* T = Db.GetReflectedType<FReflectedType>(FStringHash(Qualified));
            if (T == nullptr || IsExcludedFromCSharp(*T) || T->Type == FReflectedType::EType::Enum)
            {
                return false;
            }
            if (T->Type == FReflectedType::EType::Class)
            {
                return true; // classes are always opaque wrappers
            }
            const FReflectedStruct* S = static_cast<const FReflectedStruct*>(T);
            int Size = 0;
            int Align = 0;
            if (HasMetadata(*S, "ManualStub"))
            {
                return false; // hand-written value type
            }
            if (IsBlittableValueStruct(*S, Db, Size, Align))
            {
                return false; // blittable value mirror
            }
            return true;
        }

        // The C# base for an opaque wrapper: the reflected parent's wrapper when it's an opaque type,
        // otherwise the given Native* root. Mirrors the C++ single-inheritance chain so e.g. a
        // CStaticMesh wrapper derives the CObject wrapper and inherits its bound members.
        eastl::string CSharpBase(const FReflectedStruct& Type, const FReflectionDatabase& Db, const char* Root)
        {
            const eastl::string& Parent = Type.Parent;
            if (!Parent.empty())
            {
                const bool bQualified = Parent.find("::") != eastl::string::npos;
                const eastl::string Qualified = (bQualified || Type.Namespace.empty())
                    ? Parent
                    : (Type.Namespace + "::" + Parent);
                if (IsOpaqueWrapperType(Db, Qualified))
                {
                    return GlobalCSharp(Qualified);
                }
            }
            return Root;
        }

        // How a property is surfaced to C#. Drives BOTH the managed member and the native thunk, so
        // the two stay in lock-step: classify once, emit on each side from the same result.
        enum class EBind { None, Number, Bool, Enum, Str, Object, StructValue, StructOpaque, Array, Span, Delegate };

        struct FBinding
        {
            EBind         Kind = EBind::None;
            eastl::string CSharp;     // C# property type (for Array: the element's C# type)
            eastl::string Cpp;        // by-value thunk C++ type for Number ("int32", "double", ...)
            eastl::string TargetCpp;  // qualified C++ type for Enum/Object/StructValue ("Lumina::CStaticMesh")
            bool          bIsName = false;   // Str: FName (vs FString)
            bool          bReadOnly = false;
            eastl::unique_ptr<FBinding> Elem; // Array: the element binding (one of the scalar kinds above)
        };

        bool NumericCpp(const eastl::string& Kind, eastl::string& OutCSharp, eastl::string& OutCpp)
        {
            struct FEntry { const char* Kind; const char* CSharp; const char* Cpp; };
            static const FEntry Table[] = {
                { "Int8","sbyte","int8" }, { "Int16","short","int16" }, { "Int32","int","int32" }, { "Int64","long","int64" },
                { "UInt8","byte","uint8" }, { "UInt16","ushort","uint16" }, { "UInt32","uint","uint32" }, { "UInt64","ulong","uint64" },
                { "Float","float","float" }, { "Double","double","double" },
            };
            for (const FEntry& E : Table)
            {
                if (Kind == E.Kind) { OutCSharp = E.CSharp; OutCpp = E.Cpp; return true; }
            }
            return false;
        }

        // Classifies a TVector element by its C++ type spelling (e.g. "int32", "FString", "FNavTileData").
        // Covers the same scalar kinds as Classify minus object elements (can't tell TObjectPtr<T> from a
        // by-value T from the name alone, and a wrong guess would not compile). Unknowns return false so
        // the whole array is skipped, never a broken binding.
        bool ClassifyElement(const eastl::string& RawElem, const eastl::string& Ns, const FReflectionDatabase& Db, FBinding& B)
        {
            eastl::string Bare = RawElem;
            if (Bare.find("Lumina::") == 0)
            {
                Bare = Bare.substr(8);
            }

            struct FEntry { const char* Name; const char* CSharp; const char* Cpp; };
            static const FEntry Numeric[] = {
                { "int8","sbyte","int8" }, { "int16","short","int16" }, { "int32","int","int32" }, { "int64","long","int64" },
                { "uint8","byte","uint8" }, { "uint16","ushort","uint16" }, { "uint32","uint","uint32" }, { "uint64","ulong","uint64" },
                { "float","float","float" }, { "double","double","double" },
            };
            for (const FEntry& E : Numeric)
            {
                if (Bare == E.Name) { B.Kind = EBind::Number; B.CSharp = E.CSharp; B.Cpp = E.Cpp; return true; }
            }
            if (Bare == "bool") { B.Kind = EBind::Bool; B.CSharp = "bool"; return true; }
            if (Bare == "FString" || Bare == "FName") { B.Kind = EBind::Str; B.CSharp = "string"; B.bIsName = (Bare == "FName"); return true; }

            eastl::string Q = RawElem;
            const FReflectedType* T = Db.GetReflectedType<FReflectedType>(FStringHash(Q));
            if (T == nullptr && Q.find("::") == eastl::string::npos && !Ns.empty())
            {
                Q = Ns + "::" + RawElem;
                T = Db.GetReflectedType<FReflectedType>(FStringHash(Q));
            }
            if (T == nullptr)
            {
                return false;
            }
            if (T->Type == FReflectedType::EType::Enum)
            {
                if (IsExcludedFromCSharp(*T))
                {
                    return false;
                }
                B.Kind = EBind::Enum;
                B.CSharp = GlobalCSharp(Q);
                B.TargetCpp = Q;
                return true;
            }
            if (T->Type == FReflectedType::EType::Structure)
            {
                const FReflectedStruct* S = static_cast<const FReflectedStruct*>(T);
                int Size = 0;
                int Align = 0;
                if (HasMetadata(*S, "ManualStub") || IsBlittableValueStruct(*S, Db, Size, Align))
                {
                    B.Kind = EBind::StructValue;
                    B.CSharp = GlobalCSharp(Q);
                    B.TargetCpp = Q;
                    return true;
                }
                if (!IsExcludedFromCSharp(*S))
                {
                    B.Kind = EBind::StructOpaque;
                    B.CSharp = GlobalCSharp(Q);
                    B.TargetCpp = Q;
                    return true;
                }
            }
            return false; // classes (object elements) and anything else: skipped
        }

        // Classifies a single (non-inner) property into its C# binding. Returns false for kinds with no
        // binding (custom-accessor props, soft-objects, optionals here, unexposed targets).
        bool Classify(FReflectedProperty& Prop, const eastl::string& OwnerNs, const FReflectionDatabase& Db, FBinding& B)
        {
            if (EnumHasAnyFlags(Prop.PropertyFlags, EPropertyFlags::Private) ||
                EnumHasAnyFlags(Prop.PropertyFlags, EPropertyFlags::Protected))
            {
                return false; // a free-function thunk can't reach a non-public member
            }
            if (!Prop.GetterFunc.empty() || !Prop.SetterFunc.empty())
            {
                return false; // custom accessors -> route through FProperty later
            }

            if (auto* Arr = dynamic_cast<FReflectedArrayProperty*>(&Prop))
            {
                auto Elem = eastl::make_unique<FBinding>();
                if (!ClassifyElement(Arr->ElementTypeName, OwnerNs, Db, *Elem))
                {
                    return false; // element kind we don't bind -> skip the whole array
                }
                B.Kind = EBind::Array;
                B.CSharp = Elem->CSharp;  // element C# type; the property type is NativeReadOnlyList<this>
                B.bReadOnly = true;       // read-only view this pass (no add/remove/set)
                B.Elem = eastl::move(Elem);
                return true;
            }

            if (auto* Del = dynamic_cast<FReflectedDelegateProperty*>(&Prop))
            {
                B.Kind = EBind::Delegate;
                B.bReadOnly = true;
                if (Del->bHasPayload)
                {
                    const FReflectedStruct* S = Db.GetReflectedType<FReflectedStruct>(FStringHash(Prop.TypeName));
                    int Size = 0;
                    int Align = 0;
                    if (S == nullptr || !(HasMetadata(*S, "ManualStub") || IsBlittableValueStruct(*S, Db, Size, Align)))
                    {
                        return false; // skip non-blittable delegate payloads
                    }
                    B.CSharp = GlobalCSharp(Prop.TypeName);
                    B.TargetCpp = Prop.TypeName;
                }
                return true;
            }

            const char* KindPtr = Prop.GetTypeName();
            if (KindPtr == nullptr)
            {
                return false; // Optional (null kind) -> not bound yet
            }
            const eastl::string Kind = KindPtr;
            B.bReadOnly = IsReadOnlyProp(Prop);

            if (NumericCpp(Kind, B.CSharp, B.Cpp))
            {
                B.Kind = EBind::Number;
                return true;
            }
            if (Kind == "Bool")
            {
                B.Kind = EBind::Bool;
                B.CSharp = "bool";
                return true;
            }
            if (Kind == "Enum")
            {
                if (!IsExposed(Db, Prop.TypeName))
                {
                    return false;
                }
                B.Kind = EBind::Enum;
                B.CSharp = GlobalCSharp(Prop.TypeName);
                B.TargetCpp = Prop.TypeName;
                return true;
            }
            if (Kind == "FString" || Kind == "FName")
            {
                B.Kind = EBind::Str;
                B.CSharp = "string";
                B.bIsName = (Kind == "FName");
                return true;
            }
            if (Kind == "Object")
            {
                B.Kind = EBind::Object;
                B.TargetCpp = Prop.TypeName;
                B.CSharp = IsExposed(Db, Prop.TypeName) ? GlobalCSharp(Prop.TypeName) : "global::LuminaSharp.NativeObject";
                // Settable now via the type-erased LuminaSharp_SetObjectPtr helper (the element type need
                // not be complete here); respect only the property's real const/readonly flags.
                B.bReadOnly = IsReadOnlyProp(Prop);
                return true;
            }
            if (Kind == "Struct")
            {
                const FReflectedStruct* S = Db.GetReflectedType<FReflectedStruct>(FStringHash(Prop.TypeName));
                if (S == nullptr)
                {
                    return false;
                }
                B.TargetCpp = Prop.TypeName;
                int Size = 0;
                int Align = 0;
                const bool bValue = HasMetadata(*S, "ManualStub")   // hand-written blittable value type (math)
                    || IsBlittableValueStruct(*S, Db, Size, Align);
                if (bValue)
                {
                    B.Kind = EBind::StructValue;
                    B.CSharp = GlobalCSharp(Prop.TypeName);
                    return true;
                }
                if (!IsExcludedFromCSharp(*S))
                {
                    B.Kind = EBind::StructOpaque;   // wrapper viewing the embedded struct; read-only
                    B.CSharp = GlobalCSharp(Prop.TypeName);
                    B.bReadOnly = true;
                    return true;
                }
                return false;
            }
            return false; // SoftObject and anything else: not bound yet
        }

        // An array element of one of these kinds is bit-for-bit identical in C# (primitive / 1-byte bool /
        // width-matched enum mirror / blittable value-struct mirror), so the whole TVector<T> can be read as
        // a zero-copy ReadOnlySpan<T> directly over the native storage instead of one crossing per element.
        static bool IsBlittableElementKind(EBind Kind)
        {
            return Kind == EBind::Number || Kind == EBind::Bool || Kind == EBind::Enum || Kind == EBind::StructValue;
        }

        // A read-only TVector<T> view. For a blittable element type it is a zero-copy ReadOnlySpan<T> read in
        // place at the property's offset (no boundary crossing, no native thunk). Otherwise (FString/FName or
        // an opaque struct element) it falls back to NativeReadOnlyList<element> backed by count + per-index
        // thunks.
        void EmitCSharpArray(FCodeWriter& Writer, FReflectedProperty& Prop, const FBinding& B,
            const eastl::string& Friendly, const eastl::string& Module, const eastl::string& TypeName)
        {
            const eastl::string& Member = Prop.Name;
            const eastl::string PropName = SafeIdentifier(Member);

            if (IsBlittableElementKind(B.Elem->Kind))
            {
                const eastl::string OffName = "__off_" + Member;
                Writer.Linef("private static readonly nint %s = (nint)global::LuminaSharp.NativeBindings.PropertyOffset(\"%s\", \"%s\");",
                    OffName.c_str(), TypeName.c_str(), Member.c_str());

                if (IsReadOnlyProp(Prop))
                {
                    // Read-only: a zero-copy ReadOnlySpan<T> over the native storage.
                    Writer.Linef("public global::System.ReadOnlySpan<%s> %s =>", B.Elem->CSharp.c_str(), PropName.c_str());
                    Writer.Linef("    global::LuminaSharp.NativeMarshal.ReadVector<%s>(Handle, %s);", B.Elem->CSharp.c_str(), OffName.c_str());
                }
                else
                {
                    // Writable: a NativeList<T> view = (vector pointer, element-type ops table). Reads decode
                    // the header in place (zero crossing); Add/Remove/Clear call the ops fn-ptrs.
                    const eastl::string OpsFn = "__vecopsfn_" + Member;
                    const eastl::string OpsField = "__ops_" + Member;
                    const eastl::string OpsThunk = "LuminaSharp_VecOps_" + Friendly + "_" + Member;
                    Writer.Linef("public global::LuminaSharp.NativeList<%s> %s =>", B.Elem->CSharp.c_str(), PropName.c_str());
                    Writer.Linef("    new global::LuminaSharp.NativeList<%s>((nint)Handle + %s, %s);", B.Elem->CSharp.c_str(), OffName.c_str(), OpsField.c_str());
                    Writer.Linef("private static readonly delegate* unmanaged[Cdecl]<nint> %s =", OpsFn.c_str());
                    Writer.Linef("    (delegate* unmanaged[Cdecl]<nint>)global::LuminaSharp.NativeBindings.Resolve(\"%s\", \"%s\");",
                        Module.c_str(), OpsThunk.c_str());
                    Writer.Linef("private static readonly nint %s = %s();", OpsField.c_str(), OpsFn.c_str());
                }
                return;
            }

            const eastl::string CountFn = "__count_" + Member;
            const eastl::string GetAtFn = "__getat_" + Member;
            const eastl::string ProjFn = "__proj_" + Member;
            const eastl::string CountThunk = "LuminaSharp_Count_" + Friendly + "_" + Member;
            const eastl::string GetAtThunk = "LuminaSharp_GetAt_" + Friendly + "_" + Member;
            const eastl::string& ECS = B.Elem->CSharp;

            // Only non-blittable elements reach here (blittable returned above): an opaque-struct element wraps the
            // element pointer, an FString/FName element decodes via the two-pass buffer thunk. The projector is a
            // static method (managed function pointer) so the property read allocates no closure.
            eastl::string ProjBody;
            bool bStr = false;
            if (B.Elem->Kind == EBind::StructOpaque)
            {
                ProjBody = " => new " + ECS + "(" + GetAtFn + "(__h, __i));";
            }
            else if (B.Elem->Kind == EBind::Str)
            {
                bStr = true;
                ProjBody = " { int __n = " + GetAtFn + "(__h, __i, (byte*)null, 0); if (__n <= 0) { return string.Empty; }"
                    " byte[] __b = new byte[__n]; fixed (byte* __p = __b) { " + GetAtFn + "(__h, __i, __p, __n); }"
                    " return global::System.Text.Encoding.UTF8.GetString(__b); }";
            }
            else
            {
                return;
            }

            Writer.Linef("public global::LuminaSharp.NativeReadOnlyList<%s> %s =>", ECS.c_str(), PropName.c_str());
            Writer.Linef("    new global::LuminaSharp.NativeReadOnlyList<%s>(%s(Handle), (nint)Handle, &%s);", ECS.c_str(), CountFn.c_str(), ProjFn.c_str());
            Writer.Linef("private static %s %s(nint __h, int __i)%s", ECS.c_str(), ProjFn.c_str(), ProjBody.c_str());

            EmitThunkField(Writer, Module, CountThunk, CountFn, "System.IntPtr", "int");
            if (bStr)
            {
                EmitThunkField(Writer, Module, GetAtThunk, GetAtFn, "System.IntPtr, int, byte*, int", "int");
            }
            else
            {
                EmitThunkField(Writer, Module, GetAtThunk, GetAtFn, "System.IntPtr, int", "System.IntPtr");
            }
        }

        // The C++ element type spelling to instantiate GetVectorOps<T> with, for a blittable array element.
        eastl::string VectorElementCpp(const FBinding& Elem)
        {
            switch (Elem.Kind)
            {
                case EBind::Bool:        return "bool";
                case EBind::Number:      return Elem.Cpp;        // "int32", "float", ...
                case EBind::Enum:        return Elem.TargetCpp;  // qualified enum type
                case EBind::StructValue: return Elem.TargetCpp;  // qualified struct type
                default:                 return eastl::string();
            }
        }

        // A writable blittable-element TVector<T> exposes its element-type ops table to C# (NativeList<T>):
        // one nullary export per writable array property returning the shared GetVectorOps<T>() pointer.
        void EmitVectorOpsExport(FCodeWriter& Writer, FReflectedProperty& Prop, const FBinding& B,
            const eastl::string& Friendly, const char* Api)
        {
            const eastl::string Thunk = "LuminaSharp_VecOps_" + Friendly + "_" + Prop.Name;
            const eastl::string ElemCpp = VectorElementCpp(*B.Elem);
            Writer.Linef("extern \"C\" %s const void* %s() { return ::Lumina::GetVectorOps<%s>(); }",
                Api, Thunk.c_str(), ElemCpp.c_str());
        }

        void EmitNativeArray(FCodeWriter& Writer, FReflectedProperty& Prop, const FBinding& B,
            const eastl::string& Friendly, const char* Qualified, const char* Api)
        {
            const eastl::string& Member = Prop.Name;
            const char* M = Member.c_str();
            const eastl::string CountThunk = "LuminaSharp_Count_" + Friendly + "_" + Member;
            const eastl::string GetAtThunk = "LuminaSharp_GetAt_" + Friendly + "_" + Member;
            const char* G = GetAtThunk.c_str();

            Writer.Linef("extern \"C\" %s int %s(%s* Self) { return (int)Self->%s.size(); }",
                Api, CountThunk.c_str(), Qualified, M);

            switch (B.Elem->Kind)
            {
                case EBind::Number:
                    Writer.Linef("extern \"C\" %s %s %s(%s* Self, int Index) { if (Index < 0 || Index >= (int)Self->%s.size()) return 0; return Self->%s[(size_t)Index]; }",
                        Api, B.Elem->Cpp.c_str(), G, Qualified, M, M);
                    break;
                case EBind::Bool:
                    Writer.Linef("extern \"C\" %s unsigned char %s(%s* Self, int Index) { if (Index < 0 || Index >= (int)Self->%s.size()) return 0; return Self->%s[(size_t)Index] ? 1 : 0; }",
                        Api, G, Qualified, M, M);
                    break;
                case EBind::Enum:
                    Writer.Linef("extern \"C\" %s int %s(%s* Self, int Index) { if (Index < 0 || Index >= (int)Self->%s.size()) return 0; return (int)Self->%s[(size_t)Index]; }",
                        Api, G, Qualified, M, M);
                    break;
                case EBind::StructValue:
                    Writer.Linef("extern \"C\" %s %s %s(%s* Self, int Index) { if (Index < 0 || Index >= (int)Self->%s.size()) return %s{}; return Self->%s[(size_t)Index]; }",
                        Api, B.Elem->TargetCpp.c_str(), G, Qualified, M, B.Elem->TargetCpp.c_str(), M);
                    break;
                case EBind::StructOpaque:
                    Writer.Linef("extern \"C\" %s void* %s(%s* Self, int Index) { if (Index < 0 || Index >= (int)Self->%s.size()) return nullptr; return (void*)&Self->%s[(size_t)Index]; }",
                        Api, G, Qualified, M, M);
                    break;
                case EBind::Str:
                    Writer.Linef("extern \"C\" %s int %s(%s* Self, int Index, char* Buffer, int Capacity) "
                        "{ if (Index < 0 || Index >= (int)Self->%s.size()) return 0; const char* S = Self->%s[(size_t)Index].c_str(); "
                        "int L = S ? (int)Self->%s[(size_t)Index].length() : 0; "
                        "if (S && Buffer && Capacity > 0) { int N = L < Capacity ? L : Capacity; for (int i = 0; i < N; ++i) Buffer[i] = S[i]; } return L; }",
                        Api, G, Qualified, M, M, M);
                    break;
                default:
                    break;
            }
        }

        // The blittable-field offset static: resolved ONCE per property from live reflection (no native
        // export per property). C# then reads/writes native memory directly at (Handle + offset).
        void EmitOffsetField(FCodeWriter& Writer, const eastl::string& OffName, const eastl::string& TypeName, const eastl::string& Member)
        {
            Writer.Linef("private static readonly nint %s = (nint)global::LuminaSharp.NativeBindings.PropertyOffset(\"%s\", \"%s\");",
                OffName.c_str(), TypeName.c_str(), Member.c_str());
        }

        // The non-blittable-field token static: the FProperty* resolved ONCE per property, reused by the
        // generic per-type exporters (PropGet/SetString/Name/Object).
        void EmitTokenField(FCodeWriter& Writer, const eastl::string& PropFieldName, const eastl::string& TypeName, const eastl::string& Member)
        {
            Writer.Linef("private static readonly System.IntPtr %s = global::LuminaSharp.NativeBindings.FindProperty(\"%s\", \"%s\");",
                PropFieldName.c_str(), TypeName.c_str(), Member.c_str());
        }

        // The managed property on an opaque wrapper. Blittable kinds (Number/Bool/Enum/StructValue/
        // StructOpaque) read/write native memory directly at the property's runtime-resolved offset (zero
        // boundary crossing, zero per-property export). Non-blittable kinds (Str/Object) route through the
        // fixed generic per-type exporters with the property's FProperty* token resolved once. Array still
        // uses per-property thunks (the follow-up). TypeName is the reflected type's simple name, which the
        // native FindProperty/PropertyOffset resolve via FindObject (mirrors the component-ops lookup).
        void EmitCSharpMember(FCodeWriter& Writer, FReflectedProperty& Prop, const FBinding& B,
            const eastl::string& Friendly, const eastl::string& Module, const eastl::string& TypeName)
        {
            const eastl::string& Member = Prop.Name;
            const eastl::string PropName = SafeIdentifier(Member);
            const eastl::string OffName = "__off_" + Member;
            const eastl::string PropFieldName = "__prop_" + Member;
            const bool bRO = B.bReadOnly;
            const char* CS = B.CSharp.c_str();

            switch (B.Kind)
            {
                case EBind::Number:
                case EBind::Bool:
                case EBind::Enum:
                case EBind::StructValue:
                {
                    // For Bool the C# type is bool (1 byte); for Enum it's the enum mirror (its underlying
                    // width matches native); both are blittable so Unsafe.Read/Write at the offset is exact.
                    const char* T = (B.Kind == EBind::Bool) ? "bool" : CS;
                    Writer.Linef("public %s %s", T, PropName.c_str());
                    Writer.BeginBlock();
                    Writer.Linef("get => global::System.Runtime.CompilerServices.Unsafe.ReadUnaligned<%s>((void*)((nint)Handle + %s));",
                        T, OffName.c_str());
                    if (!bRO)
                    {
                        Writer.Linef("set => global::System.Runtime.CompilerServices.Unsafe.WriteUnaligned((void*)((nint)Handle + %s), value);",
                            OffName.c_str());
                    }
                    Writer.EndBlock();
                    EmitOffsetField(Writer, OffName, TypeName, Member);
                    break;
                }
                case EBind::Str:
                {
                    Writer.Linef("public string %s", PropName.c_str());
                    Writer.BeginBlock();
                    if (B.bIsName)
                    {
                        // FName is an interned id, not an eastl::string, so resolving it to text still goes
                        // through the name table on the native side (one crossing each way).
                        Writer.Linef("get => global::LuminaSharp.Native.PropGetName(Handle, %s);", PropFieldName.c_str());
                        if (!bRO)
                        {
                            Writer.Linef("set => global::LuminaSharp.Native.PropSetName(Handle, %s, value);", PropFieldName.c_str());
                        }
                        Writer.EndBlock();
                        EmitTokenField(Writer, PropFieldName, TypeName, Member);
                    }
                    else
                    {
                        // FString: read the eastl::basic_string in place (no crossing). The set still assigns
                        // through the native allocator via the generic exporter.
                        Writer.Linef("get => global::LuminaSharp.NativeMarshal.ReadString((nint)Handle + %s);", OffName.c_str());
                        if (!bRO)
                        {
                            Writer.Linef("set => global::LuminaSharp.Native.PropSetString(Handle, %s, value);", PropFieldName.c_str());
                        }
                        Writer.EndBlock();
                        EmitOffsetField(Writer, OffName, TypeName, Member);
                        if (!bRO)
                        {
                            EmitTokenField(Writer, PropFieldName, TypeName, Member);
                        }
                    }
                    break;
                }
                case EBind::Object:
                {
                    Writer.Linef("public %s %s", CS, PropName.c_str());
                    Writer.BeginBlock();
                    Writer.Line("get");
                    Writer.BeginBlock();
                    Writer.Linef("System.IntPtr __h = global::LuminaSharp.Native.PropGetObject(Handle, %s);", PropFieldName.c_str());
                    Writer.Linef("return __h == System.IntPtr.Zero ? null : new %s(__h);", CS);
                    Writer.EndBlock();
                    if (!bRO)
                    {
                        Writer.Linef("set => global::LuminaSharp.Native.PropSetObject(Handle, %s, value is null ? System.IntPtr.Zero : value.Handle);",
                            PropFieldName.c_str());
                    }
                    Writer.EndBlock();
                    EmitTokenField(Writer, PropFieldName, TypeName, Member);
                    break;
                }
                case EBind::StructOpaque:
                {
                    // A wrapper viewing the embedded struct in place at the offset (read-only). No export.
                    Writer.Linef("public %s %s => new %s((nint)Handle + %s);", CS, PropName.c_str(), CS, OffName.c_str());
                    EmitOffsetField(Writer, OffName, TypeName, Member);
                    break;
                }
                case EBind::Array:
                    EmitCSharpArray(Writer, Prop, B, Friendly, Module, TypeName);
                    break;
                case EBind::Delegate:
                {
                    if (B.CSharp.empty())
                    {
                        Writer.Linef("public global::LuminaSharp.ScriptDelegate %s => new global::LuminaSharp.ScriptDelegate((void*)((nint)Handle + %s));",
                            PropName.c_str(), OffName.c_str());
                    }
                    else
                    {
                        Writer.Linef("public global::LuminaSharp.ScriptDelegate<%s> %s => new global::LuminaSharp.ScriptDelegate<%s>((void*)((nint)Handle + %s));",
                            CS, PropName.c_str(), CS, OffName.c_str());
                    }
                    EmitOffsetField(Writer, OffName, TypeName, Member);
                    break;
                }
                case EBind::Span:
                case EBind::None:
                    break;
            }
        }

        // The native `extern "C" <API>` thunk(s) backing one bound property. Only an array with a
        // non-blittable element (FString/FName or an opaque struct) still needs them: blittable scalars and
        // blittable-element arrays are read directly at the offset from C#, and the non-blittable scalar
        // kinds (Str/Object) route through the fixed generic per-type exporters in DotNetProperty.cpp. So the
        // per-property native-export count is now O(non-blittable-element array properties), not O(properties).
        void EmitNativeThunk(FCodeWriter& Writer, FReflectedProperty& Prop, const FBinding& B,
            const eastl::string& Friendly, const char* Qualified, const char* Api)
        {
            if (B.Kind != EBind::Array)
            {
                return;
            }
            if (!IsBlittableElementKind(B.Elem->Kind))
            {
                // Non-blittable element (FString/FName/opaque struct): per-index count/getat thunks.
                EmitNativeArray(Writer, Prop, B, Friendly, Qualified, Api);
            }
            else if (!IsReadOnlyProp(Prop))
            {
                // Writable blittable element: the element-type ops table for NativeList<T>. Read-only
                // blittable arrays need no export (C# reads the header directly as a ReadOnlySpan).
                EmitVectorOpsExport(Writer, Prop, B, Friendly, Api);
            }
        }

        // Emits the managed members for every bindable property on an opaque wrapper.
        void EmitProperties(FCodeWriter& Writer, const FReflectedStruct& Type, const FReflectionDatabase& Db)
        {
            const eastl::string Friendly = Names::FriendlyFromQualified(Type.QualifiedName);
            const eastl::string Module = ModuleOf(Type);
            for (const auto& Prop : Type.Props)
            {
                if (Prop->bInner || IsScriptHidden(*Prop))
                {
                    continue;
                }
                FBinding B;
                if (Classify(*Prop, Type.Namespace, Db, B))
                {
                    EmitCSharpMember(Writer, *Prop, B, Friendly, Module, Type.DisplayName);
                }
            }
        }

        // "a" / 3 -> "a3". Used to name generated function parameters.
        eastl::string ArgIndexName(char Prefix, size_t I)
        {
            eastl::string Out;
            Out += Prefix;
            char Buf[12];
            int P = 11;
            Buf[P] = 0;
            if (I == 0)
            {
                Out += '0';
                return Out;
            }
            while (I > 0)
            {
                Buf[--P] = static_cast<char>('0' + (I % 10));
                I /= 10;
            }
            Out += (Buf + P);
            return Out;
        }

        // One function parameter / return classification (by-value-safe kinds only).
        struct FArg
        {
            EBind         Kind = EBind::None;
            eastl::string CSharp;
            eastl::string Cpp;
            eastl::string TargetCpp;
            bool          bEntity = false; // entt::entity: ABI-marshalled as uint32, surfaced as C# Entity.
            bool          bIsName = false; // EBind::Str arg: FName (true) vs FString (false), for the native ctor.
            // EBind::Span: a (T* ptr, int32 count) C++ pair -> one C# Span<T>/ReadOnlySpan<T>. SpanElemCpp is the
            // C++ element ("uint32"); CSharp is the full "System.Span<uint>"; bReadOnlySpan = const T*.
            eastl::string SpanElemCpp;
            bool          bReadOnlySpan = false;
        };

        // Maps a bare C++ numeric spelling ("uint32") to its C# type ("uint"); false if not a numeric.
        bool NumericCSharp(const eastl::string& Bare, eastl::string& OutCSharp)
        {
            struct FE { const char* Cpp; const char* CSharp; };
            static const FE Table[] = {
                {"int8","sbyte"},  {"int16","short"},  {"int32","int"},   {"int64","long"},
                {"uint8","byte"},  {"uint16","ushort"},{"uint32","uint"}, {"uint64","ulong"},
                {"float","float"}, {"double","double"},
            };
            for (const FE& E : Table)
            {
                if (Bare == E.Cpp) { OutCSharp = E.CSharp; return true; }
            }
            return false;
        }

        // Type spelling with const / & / * / whitespace removed: "const FName &" -> "FName".
        eastl::string StripQualifiers(const eastl::string& T)
        {
            eastl::string Out;
            for (char C : T)
            {
                if (C != ' ' && C != '\t' && C != '&' && C != '*')
                {
                    Out += C;
                }
            }
            for (size_t P; (P = Out.find("const")) != eastl::string::npos; )
            {
                Out.erase(P, 5);
            }
            return Out;
        }

        // Classifies a function parameter / return by its reflected type flag. Conservative: only
        // by-value-safe kinds (numeric / bool / enum / blittable value struct). Pointers are rejected
        // (ambiguous marshaling); a mutable-reference *arg* is rejected (a by-value can't bind it),
        // but a const-ref arg and a (copied) ref return are fine. For primitive kinds the raw spelling
        // must actually BE that primitive, so strong types that classify as an int (entt::entity, scoped
        // enums typedef'd to ints) are skipped rather than passed an incompatible int.
        bool ClassifyField(const FFieldInfo& F, const FReflectionDatabase& Db, bool bIsArg, FArg& B)
        {
            // A pointer to a reflected CObject class or opaque (non-blittable) struct marshals as the engine
            // handle: void* at the thunk, the NativeObject/NativeStruct wrapper in C# (the generator wraps a
            // returned handle / passes an arg's Handle). Handled before the generic pointer rejection since
            // the pointer spelling IS the marshaling here. A blittable value struct by pointer is rejected.
            if (F.RawFieldType.find('*') != eastl::string::npos)
            {
                if ((F.Flags == EPropertyTypeFlags::Object || F.Flags == EPropertyTypeFlags::Struct)
                    && IsOpaqueWrapperType(Db, F.TypeName))
                {
                    B.Kind = EBind::Object;
                    B.TargetCpp = F.TypeName;
                    B.CSharp = GlobalCSharp(F.TypeName);
                    return true;
                }
                return false; // any other pointer in/out -> ambiguous marshaling
            }
            if (bIsArg && F.RawFieldType.find('&') != eastl::string::npos
                && F.RawFieldType.find("const") == eastl::string::npos)
            {
                return false; // mutable-reference arg can't take a by-value
            }

            const eastl::string Bare = StripQualifiers(F.RawFieldType);

            // entt::entity (a scoped enum over uint32) marshals as the C# Entity handle. Checked before the
            // numeric/enum classification below, where it would otherwise be skipped (it classifies as an
            // int but spells "entt::entity"). The thunk passes it as a raw uint32; bEntity drives the C#
            // Entity wrapper + the native static_cast back to entt::entity.
            if (Bare == "entt::entity")
            {
                B.Kind = EBind::Number;
                B.CSharp = "global::LuminaSharp.Entity";
                B.Cpp = "uint32";
                B.bEntity = true;
                return true;
            }

            struct FNum { EPropertyTypeFlags Flag; const char* CSharp; const char* Cpp; };
            static const FNum Numerics[] = {
                { EPropertyTypeFlags::Int8,   "sbyte",  "int8" },   { EPropertyTypeFlags::Int16,  "short",  "int16" },
                { EPropertyTypeFlags::Int32,  "int",    "int32" },  { EPropertyTypeFlags::Int64,  "long",   "int64" },
                { EPropertyTypeFlags::UInt8,  "byte",   "uint8" },  { EPropertyTypeFlags::UInt16, "ushort", "uint16" },
                { EPropertyTypeFlags::UInt32, "uint",   "uint32" }, { EPropertyTypeFlags::UInt64, "ulong",  "uint64" },
                { EPropertyTypeFlags::Float,  "float",  "float" },  { EPropertyTypeFlags::Double, "double", "double" },
            };
            for (const FNum& N : Numerics)
            {
                if (F.Flags == N.Flag)
                {
                    // Accept either the canonical spelling (Bare, from RawFieldType) or the engine alias
                    // (TypeName), clang canonicalizes int32 -> int, so the alias is what the source wrote.
                    if (Bare != N.Cpp && StripQualifiers(F.TypeName) != N.Cpp)
                    {
                        return false; // strong type spelled differently than its int -> skip
                    }
                    B.Kind = EBind::Number; B.CSharp = N.CSharp; B.Cpp = N.Cpp;
                    return true;
                }
            }

            switch (F.Flags)
            {
                case EPropertyTypeFlags::Bool:
                    if (Bare != "bool" && Bare != "_Bool")
                    {
                        return false;
                    }
                    B.Kind = EBind::Bool; B.CSharp = "bool"; return true;
                case EPropertyTypeFlags::Enum:
                    if (!IsExposed(Db, F.TypeName))
                    {
                        return false;
                    }
                    B.Kind = EBind::Enum; B.CSharp = GlobalCSharp(F.TypeName); B.TargetCpp = F.TypeName; return true;
                case EPropertyTypeFlags::Struct:
                {
                    const FReflectedStruct* S = Db.GetReflectedType<FReflectedStruct>(FStringHash(F.TypeName));
                    if (S == nullptr)
                    {
                        return false;
                    }
                    int Size = 0;
                    int Align = 0;
                    if (HasMetadata(*S, "ManualStub") || IsBlittableValueStruct(*S, Db, Size, Align))
                    {
                        B.Kind = EBind::StructValue; B.CSharp = GlobalCSharp(F.TypeName); B.TargetCpp = F.TypeName; return true;
                    }
                    return false; // opaque struct by value isn't supported as a function param/return yet
                }
                case EPropertyTypeFlags::Name:
                case EPropertyTypeFlags::String:
                    // FName/FString marshal as a C# string: an ARG as (UTF-8 byte*, len) at the thunk; a
                    // RETURN via the engine two-pass caller-buffer protocol (the [NativeCall] generator
                    // emits the C# two-pass, EmitNativeFunction the trailing (char*,int)->int thunk).
                    B.Kind = EBind::Str; B.CSharp = "string"; B.bIsName = (F.Flags == EPropertyTypeFlags::Name);
                    return true;
                default:
                    return false; // Object / Vector / SoftObject / Optional / etc.: not in functions yet
            }
        }

        struct FFnBinding
        {
            bool                bVoid = true;
            FArg                Ret;
            eastl::vector<FArg> Args;
        };

        // Binds a reflected function only when the name is unique in the type (no overloads -> no C#
        // method-name collision) and every arg + the return is a by-value-safe kind.
        bool ClassifyFunction(const FReflectedFunction& Fn, const FReflectedStruct& Type, const FReflectionDatabase& Db, FFnBinding& Out)
        {
            int NameCount = 0;
            for (const auto& Other : Type.Functions)
            {
                if (Other->Name == Fn.Name)
                {
                    ++NameCount;
                }
            }
            if (NameCount != 1)
            {
                return false; // overloaded -> skip
            }
            if (Fn.bHasOmittedArgs)
            {
                return false; // an unsupported arg was dropped (LRT1005); the real signature has more args
            }

            if (Fn.Return.has_value())
            {
                if (!ClassifyField(*Fn.Return, Db, false, Out.Ret))
                {
                    return false;
                }
                Out.bVoid = false;
            }
            for (const FFieldInfo& Arg : Fn.Arguments)
            {
                FArg A;
                if (!ClassifyField(Arg, Db, true, A))
                {
                    return false;
                }
                Out.Args.push_back(A);
            }
            return true;
        }
        
        void EmitCSharpFunction(FCodeWriter& Writer, const FReflectedFunction& Fn, const FFnBinding& FB,
            const eastl::string& Friendly, const eastl::string& Module, bool bSuppressGCTransition)
        {
            const eastl::string& Name = Fn.Name;
            const eastl::string CallThunk = "LuminaSharp_Call_" + Friendly + "_" + Name;

            eastl::string SigParams;
            for (size_t i = 0; i < FB.Args.size(); ++i)
            {
                if (i != 0) { SigParams += ", "; }
                SigParams += FB.Args[i].CSharp + " " + ArgIndexName('a', i);
            }

            const eastl::string RetCS = FB.bVoid ? eastl::string("void") : FB.Ret.CSharp;
            
            if (bSuppressGCTransition)
            {
                Writer.Linef("[global::LuminaSharp.NativeCall(Module = \"%s\", EntryPoint = \"%s\", SuppressGCTransition = true)]",
                    Module.c_str(), CallThunk.c_str());
            }
            else
            {
                Writer.Linef("[global::LuminaSharp.NativeCall(Module = \"%s\", EntryPoint = \"%s\")]",
                    Module.c_str(), CallThunk.c_str());
            }
            Writer.Linef("public partial %s %s(%s);", RetCS.c_str(), Name.c_str(), SigParams.c_str());
        }

        // The native `extern "C"` thunk that performs the instance call.
        void EmitNativeFunction(FCodeWriter& Writer, const FReflectedFunction& Fn, const FFnBinding& FB,
            const eastl::string& Friendly, const char* Qualified, const char* Api)
        {
            const eastl::string& Name = Fn.Name;
            const eastl::string CallThunk = "LuminaSharp_Call_" + Friendly + "_" + Name;

            eastl::string Params;
            eastl::string CallArgs;
            for (size_t i = 0; i < FB.Args.size(); ++i)
            {
                const FArg& A = FB.Args[i];
                const eastl::string An = ArgIndexName('A', i);
                if (i != 0)
                {
                    Params += ", "; CallArgs += ", ";
                }
                
                if (A.bEntity)
                {
                    Params += "uint32 " + An; CallArgs += "static_cast<entt::entity>(" + An + ")";
                }
                else if (A.Kind == EBind::Str)
                {
                    // (const char* utf8, int len) -> a temporary FName/FString bound to the (const ref) param.
                    Params += "const char* " + An + ", int " + An + "Len";
                    const eastl::string Ctor = A.bIsName ? eastl::string("Lumina::FName") : eastl::string("Lumina::FString");
                    CallArgs += "((" + An + "Len > 0) ? " + Ctor + "(" + An + ", (size_t)" + An + "Len) : " + Ctor + "())";
                }
                else
                {
                    switch (A.Kind)
                    {
                    case EBind::Number:      Params += A.Cpp + " " + An;            CallArgs += An;                            break;
                    case EBind::Bool:        Params += "unsigned char " + An;       CallArgs += "(" + An + " != 0)";           break;
                    case EBind::Enum:        Params += "int " + An;                 CallArgs += "(" + A.TargetCpp + ")" + An;  break;
                    case EBind::StructValue: Params += A.TargetCpp + " " + An;      CallArgs += An;                            break;
                    case EBind::Object:      Params += "void* " + An;               CallArgs += "static_cast<" + A.TargetCpp + "*>(" + An + ")"; break;
                    default: break;
                    }
                }
            }

            const eastl::string CallExpr = "Self->" + Name + "(" + CallArgs + ")";
            eastl::string RetCpp = "void";
            eastl::string Body = CallExpr + ";";
            bool bStringReturn = false;
            if (!FB.bVoid)
            {
                if (FB.Ret.bEntity)
                {
                    RetCpp = "uint32"; Body = "return (uint32)(" + CallExpr + ");";
                }
                else
                {
                    switch (FB.Ret.Kind)
                    {
                    case EBind::Number:      RetCpp = FB.Ret.Cpp;       Body = "return " + CallExpr + ";";          break;
                    case EBind::Bool:        RetCpp = "unsigned char";  Body = "return " + CallExpr + " ? 1 : 0;";  break;
                    case EBind::Enum:        RetCpp = "int";            Body = "return (int)" + CallExpr + ";";      break;
                    case EBind::StructValue: RetCpp = FB.Ret.TargetCpp; Body = "return " + CallExpr + ";";          break;
                    case EBind::Object:      RetCpp = "void*";          Body = "return (void*)(" + CallExpr + ");"; break;
                    case EBind::Str:
                        // Two-pass caller-buffer protocol: copy the returned FString/FName into (Buffer,
                        // Capacity) when given, and always return the full byte length so the C# side can
                        // size an exact buffer on its first (null, 0) call. c_str() is null-terminated for
                        // both, so a manual strlen avoids needing FName::length().
                        bStringReturn = true;
                        RetCpp = "int";
                        Body = "auto __r = " + CallExpr + "; const char* __s = __r.c_str(); int __l = 0; if (__s) { while (__s[__l]) { ++__l; } } "
                               "if (__s && Buffer && Capacity > 0) { const int __n = __l < Capacity ? __l : Capacity; for (int __i = 0; __i < __n; ++__i) { Buffer[__i] = __s[__i]; } } return __l;";
                        break;
                    default: break;
                    }
                }
            }

            if (bStringReturn)
            {
                Params += Params.empty() ? "char* Buffer, int Capacity" : ", char* Buffer, int Capacity";
            }

            const eastl::string ParamSig = Params.empty() ? eastl::string() : (", " + Params);
            Writer.Linef("extern \"C\" %s %s %s(%s* Self%s) { %s }",
                Api, RetCpp.c_str(), CallThunk.c_str(), Qualified, ParamSig.c_str(), Body.c_str());
        }
        
        bool FunctionHasMetadata(const FReflectedFunction& Fn, const char* Key)
        {
            for (const FMetadataPair& Meta : Fn.Metadata)
            {
                if (Meta.Key == Key)
                {
                    return true;
                }
            }
            return false;
        }

        bool IsScriptEvent(const FReflectedFunction& Fn); // defined below; used to skip ScriptEvents here

        void EmitFunctions(FCodeWriter& Writer, const FReflectedStruct& Type, const FReflectionDatabase& Db)
        {
            const eastl::string Friendly = Names::FriendlyFromQualified(Type.QualifiedName);
            const eastl::string Module = ModuleOf(Type);
            const bool bStructFastCalls = Type.HasMetadata("ScriptFastCalls");
            for (const auto& Fn : Type.Functions)
            {
                if (IsScriptEvent(*Fn)) { continue; } // ScriptEvents are emitted specially in EmitForClass
                FFnBinding FB;
                if (ClassifyFunction(*Fn, Type, Db, FB))
                {
                    // Opt in per-function or struct-wide (ScriptFastCalls); NoSuppressGCTransition opts back out.
                    const bool bSuppressGC =
                        (bStructFastCalls || FunctionHasMetadata(*Fn, "SuppressGCTransition"))
                        && !FunctionHasMetadata(*Fn, "NoSuppressGCTransition");
                    EmitCSharpFunction(Writer, *Fn, FB, Friendly, Module, bSuppressGC);
                }
            }
        }

        void EmitNativeFunctions(FCodeWriter& Writer, const FReflectedStruct& Type, const char* Qualified, const char* Api, const FReflectionDatabase& Db)
        {
            const eastl::string Friendly = Names::FriendlyFromQualified(Type.QualifiedName);
            for (const auto& Fn : Type.Functions)
            {
                if (IsScriptEvent(*Fn)) { continue; } // ScriptEvent base/dispatch thunks come from EmitScriptableNative
                FFnBinding FB;
                if (ClassifyFunction(*Fn, Type, Db, FB))
                {
                    EmitNativeFunction(Writer, *Fn, FB, Friendly, Qualified, Api);
                }
            }
        }

        // ---- Scriptable: C# subclassing of REFLECT(Scriptable) CObjects via per-method FUNCTION(ScriptEvent) ----
        //
        // A ScriptEvent is a native virtual a C# subclass may override. The Reflector emits: a C# `virtual`
        // whose default body calls a native base thunk (runs the C++ impl); a reverse [UnmanagedCallersOnly]
        // dispatcher native calls when a subclass overrides it; and a native shim subclass whose overrides route
        // to managed (when overridden) or fall through to the base. Arg/return marshalling mirrors the forward
        // function thunks (ClassifyField) in the opposite direction.

        bool IsScriptEvent(const FReflectedFunction& Fn) { return FunctionHasMetadata(Fn, "ScriptEvent"); }

        // Reverse-marshalable kinds. Objects (CObject pointers) marshal as the engine handle (void* native /
        // IntPtr managed), exactly like the forward path. Strings are still deferred (their return needs the
        // two-pass caller-buffer protocol, which the reverse dispatcher doesn't implement yet).
        bool ScriptEventArgSupported(const FArg& A)
        {
            if (A.bEntity) { return true; }
            switch (A.Kind)
            {
            case EBind::Number: case EBind::Bool: case EBind::Enum: case EBind::StructValue: case EBind::Object: return true;
            default: return false;
            }
        }

        // A ScriptEvent must be non-overloaded and have only v1 reverse-marshalable args + return. It must also
        // be `virtual` for the shim's `override` to bind, but that's enforced at C++ compile time on the generated
        // shim rather than here (libclang's isVirtual is unreliable for inline-defined methods in this parse).
        bool ClassifyScriptEvent(const FReflectedFunction& Fn, const FReflectedStruct& Type, const FReflectionDatabase& Db, FFnBinding& Out)
        {
            if (!ClassifyFunction(Fn, Type, Db, Out)) { return false; }
            if (!Out.bVoid && !ScriptEventArgSupported(Out.Ret)) { return false; }
            for (const FArg& A : Out.Args) { if (!ScriptEventArgSupported(A)) { return false; } }
            return true;
        }

        struct FScriptEvent { const FReflectedFunction* Fn; FFnBinding FB; int Index; };

        // The classifiable ScriptEvents of a type, each tagged with its stable bit index (the index counts EVERY
        // ScriptEvent in declaration order, so the native shim and C# [ScriptEvent(index)] tags align by construction).
        void CollectScriptEvents(const FReflectedStruct& Type, const FReflectionDatabase& Db, eastl::vector<FScriptEvent>& Out)
        {
            int Index = 0;
            for (const auto& Fn : Type.Functions)
            {
                if (!IsScriptEvent(*Fn)) { continue; }
                FFnBinding FB;
                if (ClassifyScriptEvent(*Fn, Type, Db, FB)) { Out.push_back({ Fn.get(), eastl::move(FB), Index }); }
                ++Index;
            }
        }

        //~ C# reverse-dispatcher ABI mapping (native ABI <-> C# call value).
        eastl::string SeArgAbiCS(const FArg& A)
        {
            if (A.bEntity) { return "uint"; }
            switch (A.Kind) { case EBind::Bool: return "byte"; case EBind::Enum: return "int"; case EBind::Object: return "global::System.IntPtr"; default: return A.CSharp; }
        }
        eastl::string SeArgFromAbiCS(const FArg& A, const eastl::string& N)
        {
            if (A.bEntity) { return "new global::LuminaSharp.Entity(" + N + ")"; }
            switch (A.Kind)
            {
            case EBind::Bool:   return "(" + N + " != 0)";
            case EBind::Enum:   return "(" + A.CSharp + ")" + N;
            case EBind::Object: return "(" + N + " == global::System.IntPtr.Zero ? null : new " + A.CSharp + "(" + N + "))";
            default:            return N;
            }
        }
        eastl::string SeRetAbiCS(const FFnBinding& FB)
        {
            if (FB.bVoid) { return "void"; }
            if (FB.Ret.bEntity) { return "uint"; }
            switch (FB.Ret.Kind) { case EBind::Bool: return "byte"; case EBind::Enum: return "int"; case EBind::Object: return "global::System.IntPtr"; default: return FB.Ret.CSharp; }
        }
        eastl::string SeRetToAbiCS(const FFnBinding& FB, const eastl::string& Expr)
        {
            if (FB.Ret.bEntity) { return "(" + Expr + ").Id"; }
            switch (FB.Ret.Kind)
            {
            case EBind::Bool:   return "(byte)((" + Expr + ") ? 1 : 0)";
            case EBind::Enum:   return "(int)(" + Expr + ")";
            case EBind::Object: return "((" + Expr + ") is null ? global::System.IntPtr.Zero : (" + Expr + ").Handle)";
            default:            return Expr;
            }
        }

        //~ Native shim ABI mapping (real C++ type <-> reverse-thunk ABI type).
        eastl::string SeArgCppParam(const FArg& A)
        {
            if (A.bEntity) { return "entt::entity"; }
            switch (A.Kind) { case EBind::Bool: return "bool"; case EBind::Enum: return A.TargetCpp; case EBind::StructValue: return A.TargetCpp; case EBind::Object: return A.TargetCpp + "*"; default: return A.Cpp; }
        }
        eastl::string SeArgAbiCpp(const FArg& A)
        {
            if (A.bEntity) { return "uint32"; }
            switch (A.Kind) { case EBind::Bool: return "unsigned char"; case EBind::Enum: return "int"; case EBind::StructValue: return A.TargetCpp; case EBind::Object: return "void*"; default: return A.Cpp; }
        }
        eastl::string SeArgCppToAbi(const FArg& A, const eastl::string& N)
        {
            if (A.bEntity) { return "(uint32)" + N; }
            switch (A.Kind) { case EBind::Bool: return "(unsigned char)(" + N + " ? 1 : 0)"; case EBind::Enum: return "(int)" + N; case EBind::Object: return "(void*)" + N; default: return N; }
        }
        eastl::string SeRetCpp(const FFnBinding& FB)
        {
            if (FB.bVoid) { return "void"; }
            if (FB.Ret.bEntity) { return "entt::entity"; }
            switch (FB.Ret.Kind) { case EBind::Bool: return "bool"; case EBind::Enum: return FB.Ret.TargetCpp; case EBind::StructValue: return FB.Ret.TargetCpp; case EBind::Object: return FB.Ret.TargetCpp + "*"; default: return FB.Ret.Cpp; }
        }
        eastl::string SeRetAbiCpp(const FFnBinding& FB)
        {
            if (FB.bVoid) { return "void"; }
            if (FB.Ret.bEntity) { return "uint32"; }
            switch (FB.Ret.Kind) { case EBind::Bool: return "unsigned char"; case EBind::Enum: return "int"; case EBind::StructValue: return FB.Ret.TargetCpp; case EBind::Object: return "void*"; default: return FB.Ret.Cpp; }
        }
        eastl::string SeRetAbiToCpp(const FFnBinding& FB, const eastl::string& Expr)
        {
            if (FB.Ret.bEntity) { return "static_cast<entt::entity>(" + Expr + ")"; }
            switch (FB.Ret.Kind) { case EBind::Bool: return "(" + Expr + " != 0)"; case EBind::Enum: return "(" + FB.Ret.TargetCpp + ")(" + Expr + ")"; case EBind::Object: return "static_cast<" + FB.Ret.TargetCpp + "*>(" + Expr + ")"; default: return Expr; }
        }

        // Emits the C# side of one ScriptEvent into the wrapper class: a private [NativeCall] base partial (runs
        // the C++ default), the overridable `virtual` (default body calls it), and the reverse dispatcher.
        void EmitScriptEventCSharp(FCodeWriter& Writer, const FScriptEvent& E, const eastl::string& Friendly,
            const eastl::string& Module, const eastl::string& ClassName)
        {
            const eastl::string& Name = E.Fn->Name;
            const FFnBinding& FB = E.FB;
            const eastl::string BaseThunk = "LuminaSharp_Base_" + Friendly + "_" + Name;
            const eastl::string Dispatch  = "__ScriptEvent_" + Friendly + "_" + Name;

            eastl::string SigParams, ArgNames;
            for (size_t i = 0; i < FB.Args.size(); ++i)
            {
                if (i) { SigParams += ", "; ArgNames += ", "; }
                SigParams += FB.Args[i].CSharp + " " + ArgIndexName('a', i);
                ArgNames  += ArgIndexName('a', i);
            }
            const eastl::string RetCS = FB.bVoid ? eastl::string("void") : FB.Ret.CSharp;

            Writer.Linef("[global::LuminaSharp.NativeCall(Module = \"%s\", EntryPoint = \"%s\")]", Module.c_str(), BaseThunk.c_str());
            Writer.Linef("private partial %s __base_%s(%s);", RetCS.c_str(), Name.c_str(), SigParams.c_str());

            Writer.Linef("[global::LuminaSharp.ScriptEvent(%d)]", E.Index);
            Writer.Linef("public virtual %s %s(%s)", RetCS.c_str(), Name.c_str(), SigParams.c_str());
            Writer.BeginBlock();
            Writer.Linef("%s__base_%s(%s);", FB.bVoid ? "" : "return ", Name.c_str(), ArgNames.c_str());
            Writer.EndBlock();

            eastl::string AbiParams, CallArgs;
            for (size_t i = 0; i < FB.Args.size(); ++i)
            {
                AbiParams += ", " + SeArgAbiCS(FB.Args[i]) + " " + ArgIndexName('a', i);
                if (i) { CallArgs += ", "; }
                CallArgs += SeArgFromAbiCS(FB.Args[i], ArgIndexName('a', i));
            }
            Writer.Line("[global::LuminaSharp.ManagedExport]");
            Writer.Line("[global::System.Runtime.InteropServices.UnmanagedCallersOnly(CallConvs = new[] { typeof(global::System.Runtime.CompilerServices.CallConvStdcall) })]");
            Writer.Linef("internal static %s %s(global::System.IntPtr __handle%s)", SeRetAbiCS(FB).c_str(), Dispatch.c_str(), AbiParams.c_str());
            Writer.BeginBlock();
            Writer.Line("try");
            Writer.BeginBlock();
            Writer.Linef("if (global::System.Runtime.InteropServices.GCHandle.FromIntPtr(__handle).Target is %s __o)", ClassName.c_str());
            Writer.BeginBlock();
            if (FB.bVoid)
            {
                Writer.Linef("__o.%s(%s);", Name.c_str(), CallArgs.c_str());
            }
            else
            {
                // Store the result first: the object-return conversion references it twice (null-check + .Handle),
                // and we must not invoke the override twice.
                Writer.Linef("var __r = __o.%s(%s);", Name.c_str(), CallArgs.c_str());
                Writer.Linef("return %s;", SeRetToAbiCS(FB, "__r").c_str());
            }
            Writer.EndBlock();
            Writer.EndBlock();
            Writer.Line("catch (global::System.Exception __ex) { global::LuminaSharp.Interop.LogException(__ex); }");
            if (!FB.bVoid) { Writer.Line("return default;"); }
            Writer.EndBlock();
        }

        // Emits the native side for a Scriptable class: per-event base thunks, a forwarding shim subclass, and a
        // static registration so the host can mint a CClass(super = this class) using the shim.
        void EmitScriptableNative(FCodeWriter& Writer, const FReflectedStruct& Type, const FReflectionDatabase& Db, const char* Qualified, const char* Api)
        {
            const eastl::string Friendly = Names::FriendlyFromQualified(Type.QualifiedName);
            const eastl::string Shim = Friendly + "__Script";

            eastl::vector<FScriptEvent> Events;
            CollectScriptEvents(Type, Db, Events);

            // Base-call thunks (qualified, non-virtual) backing each event's C# default body.
            for (const FScriptEvent& E : Events)
            {
                const FFnBinding& FB = E.FB;
                eastl::string Params, CallArgs;
                for (size_t i = 0; i < FB.Args.size(); ++i)
                {
                    if (i) { Params += ", "; CallArgs += ", "; }
                    Params += SeArgAbiCpp(FB.Args[i]) + " " + ArgIndexName('A', i);
                    // ABI param -> the C++ base call argument (inverse of SeArgCppToAbi).
                    const FArg& A = FB.Args[i];
                    const eastl::string An = ArgIndexName('A', i);
                    if (A.bEntity)                   { CallArgs += "static_cast<entt::entity>(" + An + ")"; }
                    else if (A.Kind == EBind::Bool)  { CallArgs += "(" + An + " != 0)"; }
                    else if (A.Kind == EBind::Enum)  { CallArgs += "(" + A.TargetCpp + ")" + An; }
                    else if (A.Kind == EBind::Object){ CallArgs += "static_cast<" + A.TargetCpp + "*>(" + An + ")"; }
                    else                             { CallArgs += An; }
                }
                const eastl::string Call = eastl::string("Self->") + Qualified + "::" + E.Fn->Name + "(" + CallArgs + ")";
                const eastl::string ParamSig = Params.empty() ? eastl::string() : (", " + Params);
                if (FB.bVoid)
                {
                    Writer.Linef("extern \"C\" %s void LuminaSharp_Base_%s_%s(%s* Self%s) { %s; }",
                        Api, Friendly.c_str(), E.Fn->Name.c_str(), Qualified, ParamSig.c_str(), Call.c_str());
                }
                else
                {
                    // Return the ABI form (inverse of SeRetAbiToCpp): bool->0/1, enum->int, entity->uint32, else by value.
                    eastl::string RetExpr;
                    if (FB.Ret.bEntity)                    { RetExpr = "return (uint32)(" + Call + ");"; }
                    else if (FB.Ret.Kind == EBind::Bool)   { RetExpr = "return (" + Call + ") ? 1 : 0;"; }
                    else if (FB.Ret.Kind == EBind::Enum)   { RetExpr = "return (int)(" + Call + ");"; }
                    else if (FB.Ret.Kind == EBind::Object) { RetExpr = "return (void*)(" + Call + ");"; }
                    else                                   { RetExpr = "return " + Call + ";"; }
                    Writer.Linef("extern \"C\" %s %s LuminaSharp_Base_%s_%s(%s* Self%s) { %s }",
                        Api, SeRetAbiCpp(FB).c_str(), Friendly.c_str(), E.Fn->Name.c_str(), Qualified, ParamSig.c_str(), RetExpr.c_str());
                }
            }

            // The forwarding shim + its registration (anonymous namespace: one per Scriptable class, TU-local).
            Writer.Line("namespace");
            Writer.Line("{");
            Writer.Linef("    class %s final : public %s", Shim.c_str(), Qualified);
            Writer.Line("    {");
            Writer.Line("    public:");
            Writer.Line("        Lumina::FScriptableBridge __Bridge;");
            Writer.Linef("        virtual void PostInitProperties() override { %s::PostInitProperties(); __Bridge.Attach(this); }", Qualified);
            Writer.Linef("        virtual ~%s() override { __Bridge.Destroy(); }", Shim.c_str());
            for (const FScriptEvent& E : Events)
            {
                const FFnBinding& FB = E.FB;
                const eastl::string& Name = E.Fn->Name;
                eastl::string CppParams, BaseArgs, AbiTypes, AbiArgs;
                for (size_t i = 0; i < FB.Args.size(); ++i)
                {
                    const eastl::string An = ArgIndexName('A', i);
                    if (i) { CppParams += ", "; BaseArgs += ", "; }
                    CppParams += SeArgCppParam(FB.Args[i]) + " " + An;
                    BaseArgs  += An;
                    AbiTypes  += ", " + SeArgAbiCpp(FB.Args[i]);
                    AbiArgs   += ", " + SeArgCppToAbi(FB.Args[i], An);
                }
                const eastl::string BaseCall = eastl::string(Qualified) + "::" + Name + "(" + BaseArgs + ")";
                Writer.Linef("        virtual %s %s(%s) override", SeRetCpp(FB).c_str(), Name.c_str(), CppParams.c_str());
                Writer.Line("        {");
                Writer.Linef("            if (__Bridge.ShouldDispatch(%d))", E.Index);
                Writer.Line("            {");
                Writer.Linef("                typedef %s (*FThunk)(void*%s);", SeRetAbiCpp(FB).c_str(), AbiTypes.c_str());
                Writer.Linef("                static FThunk __t = (FThunk)Lumina::DotNet::ResolveManagedExport(\"__ScriptEvent_%s_%s\");", Friendly.c_str(), Name.c_str());
                if (FB.bVoid)
                {
                    Writer.Linef("                if (__t) { __t(__Bridge.Handle%s); return; }", AbiArgs.c_str());
                }
                else
                {
                    const eastl::string ThunkCall = eastl::string("__t(__Bridge.Handle") + AbiArgs + ")";
                    Writer.Linef("                if (__t) { return %s; }", SeRetAbiToCpp(FB, ThunkCall).c_str());
                }
                Writer.Line("            }");
                Writer.Linef("            %s%s;", FB.bVoid ? "" : "return ", BaseCall.c_str());
                Writer.Line("        }");
            }
            Writer.Line("    };");
            Writer.Linef("    struct __ScriptableReg_%s", Friendly.c_str());
            Writer.Line("    {");
            Writer.Linef("        __ScriptableReg_%s()", Friendly.c_str());
            Writer.Line("        {");
            Writer.Line("            Lumina::FScriptableNativeInfo __I;");
            Writer.Linef("            __I.GetBaseClass = []() -> Lumina::CClass* { return %s::StaticClass(); };", Qualified);
            Writer.Linef("            __I.Factory = [](void* __m) -> Lumina::CObject* { return new (__m) %s(); };", Shim.c_str());
            Writer.Linef("            __I.ShimSize = (uint32)sizeof(%s);", Shim.c_str());
            Writer.Linef("            __I.ShimAlign = (uint32)alignof(%s);", Shim.c_str());
            Writer.Linef("            Lumina::FScriptableRegistry::RegisterNative(\"%s\", __I);", Type.DisplayName.c_str());
            Writer.Line("        }");
            Writer.Line("    };");
            Writer.Linef("    static __ScriptableReg_%s __scriptableReg_%s;", Friendly.c_str(), Friendly.c_str());
            Writer.Line("}");
        }

        // ---- SCRIPT_EXPORT free functions (no owning type) ----

        // Splits a C# target "Lumina.Native" into namespace "Lumina" + class "Native". An empty target
        // defaults to namespace "Lumina", class "ScriptExports".
        void SplitTarget(const eastl::string& Target, eastl::string& OutNs, eastl::string& OutClass)
        {
            const eastl::string T = Target.empty() ? eastl::string("Lumina.ScriptExports") : Target;
            const size_t Dot = T.find_last_of('.');
            if (Dot == eastl::string::npos)
            {
                OutNs.clear();
                OutClass = T;
            }
            else
            {
                OutNs = T.substr(0, Dot);
                OutClass = T.substr(Dot + 1);
            }
        }

        // The exported symbol name for a free-function thunk: unique across the module via the qualified name.
        eastl::string FreeThunkName(const FReflectedFunction& Fn)
        {
            return "LuminaSharp_Export_" + Names::FriendlyFromQualified(Fn.QualifiedName);
        }

        // A free function binds when it has no dropped args and every arg + the return is a by-value-safe kind.
        bool ClassifyFreeFunction(const FReflectedFunction& Fn, const FReflectionDatabase& Db, FFnBinding& Out)
        {
            if (Fn.bHasOmittedArgs)
            {
                return false;
            }
            if (Fn.Return.has_value())
            {
                if (!ClassifyField(*Fn.Return, Db, false, Out.Ret))
                {
                    return false;
                }
                Out.bVoid = false;
            }
            for (size_t i = 0; i < Fn.Arguments.size(); ++i)
            {
                const FFieldInfo& Arg = Fn.Arguments[i];

                // Span<T>: a primitive pointer (T*) immediately followed by an int32 count maps to ONE C#
                // Span<T> / ReadOnlySpan<T>. The C# generator marshals it to (T* pinned, int Length); the
                // native thunk passes that pair straight to the C++ function (which already takes T*, int).
                if (Arg.RawFieldType.find('*') != eastl::string::npos && (i + 1) < Fn.Arguments.size())
                {
                    const eastl::string Elem = StripQualifiers(Arg.RawFieldType);
                    const eastl::string NextBare = StripQualifiers(Fn.Arguments[i + 1].RawFieldType);
                    eastl::string ElemCS;
                    if ((NextBare == "int32" || NextBare == "int") && NumericCSharp(Elem, ElemCS))
                    {
                        FArg A;
                        A.Kind = EBind::Span;
                        A.SpanElemCpp = Elem;
                        A.bReadOnlySpan = Arg.RawFieldType.find("const") != eastl::string::npos;
                        A.CSharp = (A.bReadOnlySpan ? eastl::string("global::System.ReadOnlySpan<")
                                                    : eastl::string("global::System.Span<")) + ElemCS + ">";
                        Out.Args.push_back(A);
                        ++i; // consume the count parameter
                        continue;
                    }
                }

                FArg A;
                if (!ClassifyField(Arg, Db, true, A))
                {
                    return false;
                }
                Out.Args.push_back(A);
            }
            return true;
        }

        void EmitCSharpFreeFunction(FCodeWriter& Writer, const FReflectedFunction& Fn, const FFnBinding& FB,
            const eastl::string& Module, bool bSuppressGCTransition)
        {
            const eastl::string Thunk = FreeThunkName(Fn);
            eastl::string SigParams;
            for (size_t i = 0; i < FB.Args.size(); ++i)
            {
                if (i != 0) { SigParams += ", "; }
                SigParams += FB.Args[i].CSharp + " " + ArgIndexName('a', i);
            }
            const eastl::string RetCS = FB.bVoid ? eastl::string("void") : FB.Ret.CSharp;
            if (bSuppressGCTransition)
            {
                Writer.Linef("[global::LuminaSharp.NativeCall(Module = \"%s\", EntryPoint = \"%s\", SuppressGCTransition = true)]",
                    Module.c_str(), Thunk.c_str());
            }
            else
            {
                Writer.Linef("[global::LuminaSharp.NativeCall(Module = \"%s\", EntryPoint = \"%s\")]",
                    Module.c_str(), Thunk.c_str());
            }
            Writer.Linef("public static partial %s %s(%s);", RetCS.c_str(), Fn.Name.c_str(), SigParams.c_str());
        }

        // The native extern "C" thunk that calls the free function by its qualified name (no instance).
        void EmitNativeFreeFunction(FCodeWriter& Writer, const FReflectedFunction& Fn, const FFnBinding& FB, const char* Api)
        {
            const eastl::string Thunk = FreeThunkName(Fn);
            eastl::string Params;
            eastl::string CallArgs;
            for (size_t i = 0; i < FB.Args.size(); ++i)
            {
                const FArg& A = FB.Args[i];
                const eastl::string An = ArgIndexName('A', i);
                if (i != 0)
                {
                    Params += ", "; CallArgs += ", ";
                }

                if (A.bEntity)
                {
                    Params += "uint32 " + An; CallArgs += "static_cast<entt::entity>(" + An + ")";
                }
                else if (A.Kind == EBind::Str)
                {
                    Params += "const char* " + An + ", int " + An + "Len";
                    const eastl::string Ctor = A.bIsName ? eastl::string("Lumina::FName") : eastl::string("Lumina::FString");
                    CallArgs += "((" + An + "Len > 0) ? " + Ctor + "(" + An + ", (size_t)" + An + "Len) : " + Ctor + "())";
                }
                else if (A.Kind == EBind::Span)
                {
                    // C# Span<T> -> (T* pinned, int Length); pass the pair straight to the C++ (T*, int).
                    const eastl::string Ptr = A.bReadOnlySpan ? ("const " + A.SpanElemCpp + "* ") : (A.SpanElemCpp + "* ");
                    Params += Ptr + An + ", int " + An + "Count";
                    CallArgs += An + ", " + An + "Count";
                }
                else
                {
                    switch (A.Kind)
                    {
                    case EBind::Number:      Params += A.Cpp + " " + An;        CallArgs += An;                            break;
                    case EBind::Bool:        Params += "unsigned char " + An;   CallArgs += "(" + An + " != 0)";           break;
                    case EBind::Enum:        Params += "int " + An;             CallArgs += "(" + A.TargetCpp + ")" + An;  break;
                    case EBind::StructValue: Params += A.TargetCpp + " " + An;  CallArgs += An;                            break;
                    case EBind::Object:      Params += "void* " + An;           CallArgs += "static_cast<" + A.TargetCpp + "*>(" + An + ")"; break;
                    default: break;
                    }
                }
            }

            const eastl::string CallExpr = Fn.QualifiedName + "(" + CallArgs + ")";
            eastl::string RetCpp = "void";
            eastl::string Body = CallExpr + ";";
            bool bStringReturn = false;
            if (!FB.bVoid)
            {
                if (FB.Ret.bEntity)
                {
                    RetCpp = "uint32"; Body = "return (uint32)(" + CallExpr + ");";
                }
                else
                {
                    switch (FB.Ret.Kind)
                    {
                    case EBind::Number:      RetCpp = FB.Ret.Cpp;        Body = "return " + CallExpr + ";";          break;
                    case EBind::Bool:        RetCpp = "unsigned char";   Body = "return " + CallExpr + " ? 1 : 0;";  break;
                    case EBind::Enum:        RetCpp = "int";             Body = "return (int)" + CallExpr + ";";      break;
                    case EBind::StructValue: RetCpp = FB.Ret.TargetCpp;  Body = "return " + CallExpr + ";";          break;
                    case EBind::Object:      RetCpp = "void*";           Body = "return (void*)(" + CallExpr + ");"; break;
                    case EBind::Str:
                        bStringReturn = true;
                        RetCpp = "int";
                        Body = "auto __r = " + CallExpr + "; const char* __s = __r.c_str(); int __l = 0; if (__s) { while (__s[__l]) { ++__l; } } "
                               "if (__s && Buffer && Capacity > 0) { const int __n = __l < Capacity ? __l : Capacity; for (int __i = 0; __i < __n; ++__i) { Buffer[__i] = __s[__i]; } } return __l;";
                        break;
                    default: break;
                    }
                }
            }
            if (bStringReturn)
            {
                Params += Params.empty() ? "char* Buffer, int Capacity" : ", char* Buffer, int Capacity";
            }

            Writer.Linef("extern \"C\" %s %s %s(%s) { %s }",
                Api, RetCpp.c_str(), Thunk.c_str(), Params.c_str(), Body.c_str());
        }
    }

    bool CSharpBindingEmitter::EmitForEnum(FCodeWriter& Writer, const FReflectedEnum& Enum)
    {
        if (IsExcludedFromCSharp(Enum))
        {
            return false;
        }

        bool bNs = false;
        OpenNamespace(Writer, Enum.Namespace, bNs);

        Writer.Linef("public enum %s : %s", Enum.DisplayName.c_str(), CSharpEnumBackingType(Enum));
        Writer.BeginBlock();
        for (const FReflectedEnum::FConstant& Constant : Enum.Constants)
        {
            Writer.Linef("%s = %u,", SafeIdentifier(Constant.Label).c_str(), Constant.Value);
        }
        Writer.EndBlock();

        if (bNs)
        {
            Writer.EndBlock();
        }
        Writer.Line();
        return true;
    }

    bool CSharpBindingEmitter::EmitFreeFunctions(FCodeWriter& Writer, FReflectedHeader* Header, const FReflectionDatabase& Database)
    {
        const auto It = Database.FreeFunctions.find(Header);
        if (It == Database.FreeFunctions.end())
        {
            return false;
        }

        const eastl::string Module = (Header->Project != nullptr) ? Header->Project->Name : eastl::string("Runtime");

        // One static partial-class block per function; C# merges partials targeting the same class.
        bool bEmittedAny = false;
        for (const auto& Fn : It->second)
        {
            FFnBinding FB;
            if (!ClassifyFreeFunction(*Fn, Database, FB))
            {
                continue;
            }

            eastl::string Ns;
            eastl::string Cls;
            SplitTarget(Fn->CSharpTarget, Ns, Cls);

            if (!Ns.empty())
            {
                Writer.Linef("namespace %s", Ns.c_str());
                Writer.BeginBlock();
            }
            Writer.Linef("public static unsafe partial class %s", Cls.c_str());
            Writer.BeginBlock();
            EmitCSharpFreeFunction(Writer, *Fn, FB, Module, FunctionHasMetadata(*Fn, "SuppressGCTransition"));
            Writer.EndBlock();
            if (!Ns.empty())
            {
                Writer.EndBlock();
            }
            bEmittedAny = true;
        }

        if (bEmittedAny)
        {
            Writer.Line();
        }
        return bEmittedAny;
    }

    void CSharpBindingEmitter::EmitNativeFreeFunctions(FCodeWriter& Writer, FReflectedHeader* Header, const FReflectionDatabase& Database)
    {
        const auto It = Database.FreeFunctions.find(Header);
        if (It == Database.FreeFunctions.end())
        {
            return;
        }

        // Interop thunks are resolved by name at runtime from whatever binary they land in (their module DLL
        // in modular builds, the exe in monolithic), so they always use the always-dllexport LUMINA_SCRIPT_API
        // rather than the per-module API macro (which is empty under LUMINA_MONOLITHIC). See ModuleAPI.h.
        const char* Api = "LUMINA_SCRIPT_API";
        for (const auto& Fn : It->second)
        {
            FFnBinding FB;
            if (ClassifyFreeFunction(*Fn, Database, FB))
            {
                EmitNativeFreeFunction(Writer, *Fn, FB, Api);
            }
        }
    }

    bool CSharpBindingEmitter::EmitForStruct(FCodeWriter& Writer, const FReflectedStruct& Struct, const FReflectionDatabase& Database)
    {
        if (IsExcludedFromCSharp(Struct))
        {
            return false;
        }

        // A reflected struct becomes a blittable C# VALUE mirror (a [StructLayout(Sequential)] struct passed
        // by value) automatically when every field is blittable and it isn't a live ECS component (see
        // IsBlittableValueStruct); a native size assert validates the layout. Otherwise it's an opaque
        // handle, most reflected structs hold FString/containers/smart-ptrs and would corrupt if mirrored
        // by value, and the Reflector can't see non-PROPERTY fields.
        int Size = 0;
        int Align = 0;
        const bool bBlittable = IsBlittableValueStruct(Struct, Database, Size, Align);

        bool bNs = false;
        OpenNamespace(Writer, Struct.Namespace, bNs);

        if (bBlittable)
        {
            Writer.Linef("[global::LuminaSharp.NativeType(\"%s\")]", Struct.DisplayName.c_str());
            Writer.Line("[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]");
            Writer.Linef("public struct %s", Struct.DisplayName.c_str());
            Writer.BeginBlock();
            for (const auto& Prop : Struct.Props)
            {
                if (Prop->bInner)
                {
                    continue;
                }
                Writer.Linef("public %s %s;", CSharpFieldType(*Prop).c_str(), SafeIdentifier(Prop->Name).c_str());
            }
            Writer.EndBlock();
        }
        else
        {
            // Non-blittable (FString/containers/smart-ptrs): an opaque handle, not a value mirror.
            const eastl::string Base = CSharpBase(Struct, Database, "global::LuminaSharp.NativeStruct");
            Writer.Linef("[global::LuminaSharp.NativeType(\"%s\")]", Struct.DisplayName.c_str());
            Writer.Linef("public unsafe partial class %s : %s", Struct.DisplayName.c_str(), Base.c_str());
            Writer.BeginBlock();
            Writer.Linef("internal %s(System.IntPtr handle) : base(handle) { }", Struct.DisplayName.c_str());
            EmitProperties(Writer, Struct, Database);
            EmitFunctions(Writer, Struct, Database);
            Writer.EndBlock();
        }

        if (bNs)
        {
            Writer.EndBlock();
        }
        Writer.Line();
        return true;
    }

    bool CSharpBindingEmitter::EmitForClass(FCodeWriter& Writer, const FReflectedClass& Class, const FReflectionDatabase& Database)
    {
        if (IsExcludedFromCSharp(Class))
        {
            return false;
        }

        bool bNs = false;
        OpenNamespace(Writer, Class.Namespace, bNs);

        // Opaque handle wrapper: derives its reflected base's wrapper (inheriting its members) and
        // adds its own bound properties.
        const eastl::string Base = CSharpBase(Class, Database, "global::LuminaSharp.NativeObject");
        const bool bScriptable = Class.HasMetadata("Scriptable");
        if (bScriptable)
        {
            // Marks the wrapper as a user-subclassable base; the runtime discovers Scriptable subclasses by it.
            Writer.Line("[global::LuminaSharp.ScriptableType]");
        }
        Writer.Linef("[global::LuminaSharp.NativeType(\"%s\")]", Class.DisplayName.c_str());
        Writer.Linef("public unsafe partial class %s : %s", Class.DisplayName.c_str(), Base.c_str());
        Writer.BeginBlock();
        Writer.Linef("internal %s(System.IntPtr handle) : base(handle) { }", Class.DisplayName.c_str());
        // Managed-first ctor: a user subclass (or the Scriptable host) Activator-creates the instance, then the
        // native object is bound via BindNativeHandle. Chains to the base's parameterless ctor up to NativeObject().
        Writer.Linef("protected %s() : base() { }", Class.DisplayName.c_str());
        if (bScriptable)
        {
            const eastl::string Module = ModuleOf(Class);
            const eastl::string Friendly = Names::FriendlyFromQualified(Class.QualifiedName);
            eastl::vector<FScriptEvent> Events;
            CollectScriptEvents(Class, Database, Events);
            for (const FScriptEvent& E : Events)
            {
                EmitScriptEventCSharp(Writer, E, Friendly, Module, Class.DisplayName);
            }
        }
        EmitProperties(Writer, Class, Database);
        EmitFunctions(Writer, Class, Database);
        Writer.EndBlock();

        if (bNs)
        {
            Writer.EndBlock();
        }
        Writer.Line();
        return true;
    }

    void CSharpBindingEmitter::EmitNativeLayoutCheck(FCodeWriter& Writer, const FReflectedStruct& Struct, const FReflectionDatabase& Database)
    {
        if (IsExcludedFromCSharp(Struct))
        {
            return;
        }

        int Size = 0;
        int Align = 0;
        if (!IsBlittableValueStruct(Struct, Database, Size, Align))
        {
            return; // only blittable value mirrors have a flat layout to validate
        }

        Writer.Linef("static_assert(sizeof(%s) == %d, \"LuminaSharp: blittable C# mirror of %s has a size mismatch (likely a non-reflected field).\");",
            Struct.QualifiedName.c_str(), Size, Struct.DisplayName.c_str());
    }

    void CSharpBindingEmitter::EmitNativeThunks(FCodeWriter& Writer, const FReflectedStruct& Type, const FReflectionDatabase& Database)
    {
        if (IsExcludedFromCSharp(Type))
        {
            return;
        }
        // A blittable value-mirror struct exposes its fields directly -> no thunks.
        if (Type.Type == FReflectedType::EType::Structure)
        {
            int Size = 0;
            int Align = 0;
            if (IsBlittableValueStruct(Type, Database, Size, Align))
            {
                return;
            }
        }

        const eastl::string Friendly = Names::FriendlyFromQualified(Type.QualifiedName);
        const char* Qualified = Type.QualifiedName.c_str();
        // Interop thunks are resolved by name at runtime from whatever binary they land in (their module
        // DLL in modular builds, the exe in monolithic), so they use the always-dllexport LUMINA_SCRIPT_API
        // rather than the per-module API macro (empty under LUMINA_MONOLITHIC). See ModuleAPI.h.
        const char* ApiMacro = "LUMINA_SCRIPT_API";

        for (const auto& Prop : Type.Props)
        {
            if (Prop->bInner || IsScriptHidden(*Prop))
            {
                continue;
            }
            FBinding B;
            if (Classify(*Prop, Type.Namespace, Database, B))
            {
                EmitNativeThunk(Writer, *Prop, B, Friendly, Qualified, ApiMacro);
            }
        }

        EmitNativeFunctions(Writer, Type, Qualified, ApiMacro, Database);

        // Scriptable class: emit the forwarding shim subclass + base thunks + the CClass-minting registration.
        if (Type.Type == FReflectedType::EType::Class && Type.HasMetadata("Scriptable"))
        {
            EmitScriptableNative(Writer, Type, Database, Qualified, ApiMacro);
        }
    }
}
