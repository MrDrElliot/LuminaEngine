#include "CSharpBindingEmitter.h"

#include <algorithm>
#include <memory>
#include <set>

#include "CodeWriter.h"
#include "Reflector/CodeGeneration/ReflectionNames.h"
#include "Reflector/ReflectionCore/ReflectedHeader.h"
#include "Reflector/ReflectionCore/ReflectedProject.h"
#include "Reflector/ReflectionCore/ReflectionDatabase.h"
#include "Reflector/Types/FieldInfo.h"
#include "Reflector/Types/Functions/ReflectedFunction.h"
#include "Reflector/Types/Properties/ReflectedArrayProperty.h"
#include "Reflector/Types/Properties/ReflectedDelegateProperty.h"
#include "Reflector/Types/Properties/ReflectedMapProperty.h"
#include "Reflector/Types/Properties/ReflectedOptionalProperty.h"
#include "Reflector/Types/PropertyFlags.h"
#include "Reflector/Types/ReflectedType.h"
#include "StringHash.h"

namespace Lumina::Reflection
{
    namespace
    {
        // The C++ comment as a one-line summary, so IntelliSense shows it on the binding.
        void EmitDocSummary(FCodeWriter& Writer, std::string_view Text)
        {
            std::string Escaped;
            for (const char Char : Text)
            {
                switch (Char)
                {
                    case '&': Escaped += "&amp;"; break;
                    case '<': Escaped += "&lt;"; break;
                    case '>': Escaped += "&gt;"; break;
                    case '\r': break;
                    case '\n': Escaped += ' '; break;
                    default: Escaped += Char; break;
                }
            }
            const size_t First = Escaped.find_first_not_of(' ');
            if (First == std::string::npos)
            {
                return;
            }
            Writer.Linef("/// <summary>%s</summary>", Escaped.substr(First, Escaped.find_last_not_of(' ') - First + 1).c_str());
        }

        // A ToolTip is stored escaped for the generated C++ literal, and a hand-written one precedes the comment.
        void EmitDocSummary(FCodeWriter& Writer, const std::vector<FMetadataPair>& Metadata)
        {
            const std::string* ToolTip = FindMetadata(Metadata, "ToolTip");
            if (ToolTip == nullptr)
            {
                return;
            }
            std::string Text;
            for (size_t Index = 0; Index < ToolTip->size(); ++Index)
            {
                const bool bEscape = (*ToolTip)[Index] == '\\' && Index + 1 < ToolTip->size()
                    && ((*ToolTip)[Index + 1] == '\\' || (*ToolTip)[Index + 1] == '"');
                Text += (*ToolTip)[bEscape ? ++Index : Index];
            }
            EmitDocSummary(Writer, Text);
        }

        // Either an explicit opt-out or a mirror LuminaSharp already hand-writes.
        bool IsExcludedFromCSharp(const FReflectedType& Type)
        {
            return Type.HasMetadata("NoCSharp") || Type.HasMetadata("CSharpValueMirror");
        }

        // An explicit backing type keeps a 1, 2 or 8-byte enum the same size as its C++ source.
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

        // Drives both the native thunk's export macro and the C# P/Invoke library key, not just Runtime.
        std::string ModuleOf(const FReflectedType& Type)
        {
            if (Type.Header != nullptr && Type.Header->Project != nullptr)
            {
                return Type.Header->Project->Name;
            }
            return "Runtime";
        }

        // C++ "Lumina::FVector3" -> C# "Lumina.FVector3".
        std::string ToCSharpName(std::string Name)
        {
            for (size_t Pos = Name.find("::"); Pos != std::string::npos; Pos = Name.find("::"))
            {
                Name.replace(Pos, 2, ".");
            }
            return Name;
        }

        bool IsCSharpKeyword(const std::string& Word)
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

        std::string SafeIdentifier(const std::string& Name)
        {
            return IsCSharpKeyword(Name) ? ("@" + Name) : Name;
        }

        // A struct handle spelled as a nullable, back to the handle type a generic argument needs.
        std::string StripNullable(const std::string& CSharp)
        {
            return !CSharp.empty() && CSharp.back() == '?' ? CSharp.substr(0, CSharp.size() - 1) : CSharp;
        }

        // Returns true with the size and alignment for blittable primitives, false for every other kind.
        bool PrimitiveCSharp(const std::string& Kind, std::string& OutCSharp, int& OutSize, int& OutAlign)
        {
            struct FEntry { const char* Kind; const char* CSharp; int Size; int Align; };
            static const FEntry Table[] = {
                { "Int8",   "sbyte",  1, 1 }, { "UInt8",  "byte",   1, 1 }, { "Bool",   "bool",  1, 1 },
                { "Int16",  "short",  2, 2 }, { "UInt16", "ushort", 2, 2 },
                { "Int32",  "int",    4, 4 }, { "UInt32", "uint",   4, 4 }, { "Float",  "float", 4, 4 },
                { "Int64",  "long",   8, 8 }, { "UInt64", "ulong",  8, 8 }, { "Double", "double",8, 8 },
                { "Entity", "global::LuminaSharp.FEntity", 4, 4 },
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

        // Returns false unless every reflected field is a primitive, int-backed enum or blittable struct.
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
                const std::string Kind = KindPtr;
                int Size = 0;
                int Align = 0;
                std::string Unused;

                if (PrimitiveCSharp(Kind, Unused, Size, Align))
                {
                    // blittable primitive
                }
                else if (Kind == "Enum")
                {
                    // The generated enum's explicit backing type matches native width, so use it for size and align.
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
            if (Struct.HasMetadata("Component"))
            {
                return false; // write-through component wrapper (see EmitForStruct)
            }
            if (!Struct.Parent.empty())
            {
                return false; // a struct with a reflected base isn't a flat value mirror
            }
            if (Struct.bHasUnreflectedFields)
            {
                return false; // hidden non-PROPERTY state, so the reflected layout is incomplete and cannot blit by value
            }
            // An empty reflected struct computes a zero-size mirror, which can never match a C++ sizeof.
            return ComputeBlittableLayout(Struct, Db, OutSize, OutAlign, 0) && OutSize > 0;
        }

        std::string StripQualifiers(const std::string& T);

        // Exact spellings only, so FEntityRegistry and friends are not swept up by a substring match.
        bool IsEntitySpelling(const std::string& Type)
        {
            const std::string Bare = StripQualifiers(Type);
            return Bare == "FEntity" || Bare == "Lumina::FEntity"
                || Bare == "ECS::FEntity" || Bare == "Lumina::ECS::FEntity";
        }

        // An ECS::FEntity field is its own Entity kind, blittable as its packed id, and surfaces as the C# FEntity.
        bool IsEntityField(const FReflectedProperty& Prop)
        {
            return IsEntitySpelling(Prop.RawTypeName) || IsEntitySpelling(Prop.TypeName);
        }

        // The C# field type for a blittable struct member.
        std::string CSharpFieldType(FReflectedProperty& Prop)
        {
            if (IsEntityField(Prop))
            {
                return "global::LuminaSharp.FEntity";
            }
            const std::string Kind = Prop.GetTypeName();
            std::string CSharp;
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

        void OpenNamespace(FCodeWriter& Writer, const std::string& CppNamespace, bool& bOpened)
        {
            const std::string Ns = ToCSharpName(CppNamespace);
            bOpened = !Ns.empty();
            if (bOpened)
            {
                Writer.Linef("namespace %s", Ns.c_str());
                Writer.BeginBlock();
            }
        }

        // ScriptReadOnly forces getter-only and ScriptWritable forces a setter, else the editor flags apply.
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

        // PROPERTY(ScriptHidden) emits no wrapper member for the property at all.
        bool IsScriptHidden(const FReflectedProperty& Prop)
        {
            return EnumHasAnyFlags(Prop.PropertyFlags, EPropertyFlags::ScriptHidden);
        }
        
        [[maybe_unused]] void EmitThunkField(FCodeWriter& Writer, const std::string& Module, const std::string& EntryPoint,
            const std::string& Fn, const std::string& ParamTypesCsv, const std::string& RetType)
        {
            const std::string Sig = ParamTypesCsv.empty() ? RetType : (ParamTypesCsv + ", " + RetType);
            Writer.Linef("private static readonly delegate* unmanaged[Cdecl]<%s> %s =", Sig.c_str(), Fn.c_str());
            Writer.Linef("    (delegate* unmanaged[Cdecl]<%s>)global::LuminaSharp.NativeBindings.Resolve(\"%s\", \"%s\");",
                Sig.c_str(), Module.c_str(), EntryPoint.c_str());
        }

        // A reflected type that exists in the database and is allowed in the C# API.
        bool IsExposed(const FReflectionDatabase& Db, const std::string& Qualified)
        {
            const FReflectedType* T = Db.GetReflectedType<FReflectedType>(FStringHash(Qualified));
            return T != nullptr && !IsExcludedFromCSharp(*T);
        }

        // "global::" + C# form of a qualified C++ name ("Lumina::CStaticMesh" -> "global::Lumina.CStaticMesh").
        std::string GlobalCSharp(const std::string& Qualified)
        {
            return "global::" + ToCSharpName(Qualified);
        }

        // A class, or a struct that is not a blittable value mirror, becomes an opaque handle wrapper.
        std::vector<const FReflectedStruct*> ReflectedAncestors(const FReflectedStruct& Type, const FReflectionDatabase& Db);

        // A component, or a struct deriving one, which C# only ever reaches in place.
        bool IsComponentStruct(const FReflectedStruct& Struct, const FReflectionDatabase& Db)
        {
            if (Struct.HasMetadata("Component"))
            {
                return true;
            }
            const std::vector<const FReflectedStruct*> Ancestors = ReflectedAncestors(Struct, Db);
            return std::any_of(Ancestors.begin(), Ancestors.end(), [](const FReflectedStruct* Ancestor) { return Ancestor->HasMetadata("Component"); });
        }

        bool IsComponentType(const FReflectionDatabase& Db, const std::string& Qualified)
        {
            const FReflectedType* T = Db.GetReflectedType<FReflectedType>(FStringHash(Qualified));
            return T != nullptr && T->Type == FReflectedType::EType::Structure && IsComponentStruct(*static_cast<const FReflectedStruct*>(T), Db);
        }

        bool IsOpaqueWrapperType(const FReflectionDatabase& Db, const std::string& Qualified)
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
            if (S->HasMetadata("CSharpValueMirror"))
            {
                return false; // hand-written value type
            }
            if (IsBlittableValueStruct(*S, Db, Size, Align))
            {
                return false; // blittable value mirror
            }
            return true;
        }

        // The CObject root is hand-declared, so it has no database entry and models as NativeObject.
        bool IsObjectRootType(const std::string& Qualified)
        {
            return Qualified == "Lumina::CObject" || Qualified == "CObject";
        }

        // Mirrors the C++ single-inheritance chain so a wrapper inherits its parent's bound members.
        std::string CSharpBase(const FReflectedStruct& Type, const FReflectionDatabase& Db, const char* Root)
        {
            const std::string& Parent = Type.Parent;
            if (!Parent.empty())
            {
                const bool bQualified = Parent.find("::") != std::string::npos;
                const std::string Qualified = (bQualified || Type.Namespace.empty())
                    ? Parent
                    : (Type.Namespace + "::" + Parent);
                if (IsOpaqueWrapperType(Db, Qualified))
                {
                    return GlobalCSharp(Qualified);
                }
            }
            return Root;
        }

        // The function a script write must call, named on the type or the nearest parent that names one.
        std::string FindScriptWriteHook(const FReflectedStruct& Type, const FReflectionDatabase& Db)
        {
            const FReflectedStruct* Current = &Type;
            while (Current != nullptr)
            {
                if (const std::string* Hook = Current->TryGetMetadata("OnScriptWrite"))
                {
                    return *Hook;
                }
                const std::string& Parent = Current->Parent;
                if (Parent.empty())
                {
                    break;
                }
                const bool bQualified = Parent.find("::") != std::string::npos;
                const std::string Qualified = (bQualified || Current->Namespace.empty()) ? Parent : (Current->Namespace + "::" + Parent);
                const FReflectedType* Found = Db.GetReflectedType<FReflectedType>(FStringHash(Qualified));
                if (Found == nullptr || Found->Type == FReflectedType::EType::Enum || Found == Current)
                {
                    break;
                }
                Current = static_cast<const FReflectedStruct*>(Found);
            }
            return {};
        }

        // Narrowed from EPropertyTypeFlags by Classify, then drives the managed member and the native thunk together.
        enum class EBind { None, Number, Bool, Enum, Str, Object, SoftObject, ClassRef, SubStructRef, StructValue, StructOpaque, InstancedStruct, Array, Map, Optional, Span, Delegate, Pointer };

        struct FBinding
        {
            EBind         Kind = EBind::None;
            std::string CSharp;     // C# property type (for Array, the element's C# type)
            std::string Cpp;        // by-value thunk C++ type for Number ("int32", "double", ...)
            std::string TargetCpp;  // qualified C++ type for Enum/Object/StructValue ("Lumina::CStaticMesh")
            bool          bReadOnly = false;
            // Only a CObject has the managed instance slot, so a component pointer must not reach ForObject.
            bool          bCObject = false;
            std::unique_ptr<FBinding> Elem; // Array, Map and Optional kinds, the payload (Map: the key)
            std::unique_ptr<FBinding> Value; // Map kind, the value binding
        };

        static bool IsBlittableElementKind(EBind Kind);

        // An FName is an 8-byte id, so it crosses and is stored as the blittable C# mirror rather than as text.
        template <typename TBinding>
        void BindName(TBinding& B)
        {
            B.Kind = EBind::StructValue;
            B.CSharp = "global::Lumina.FName";
            B.TargetCpp = "Lumina::FName";
        }
        void EmitOffsetField(FCodeWriter& Writer, const std::string& OffName, const FReflectedProperty& Prop, const std::string& TypeName);
        bool FunctionHasMetadata(const FReflectedFunction& Fn, const char* Key);

        bool NumericCpp(const std::string& Kind, std::string& OutCSharp, std::string& OutCpp)
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

        // Unknown element spellings return false, so the whole array is skipped rather than bound wrong.
        bool ClassifyElement(const std::string& RawElem, const std::string& Ns, const FReflectionDatabase& Db, FBinding& B)
        {
            std::string Bare = RawElem;
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
            if (Bare == "FString") { B.Kind = EBind::Str; B.CSharp = "string"; return true; }
            if (Bare == "FName") { BindName(B); return true; }

            // Every element here is one FSoftObjectPath natively, whatever the declared spelling.
            if (Bare == "FSoftObjectPath" || Bare.find("TSoftObjectPtr<") == 0)
            {
                std::string Target;
                if (Bare.find("TSoftObjectPtr<") == 0)
                {
                    const size_t Open = Bare.find('<');
                    const size_t Close = Bare.rfind('>');
                    if (Open != std::string::npos && Close != std::string::npos && Close > Open + 1)
                    {
                        Target = Bare.substr(Open + 1, Close - Open - 1);
                    }
                }
                // Unqualified in the element spelling, so the owner namespace is tried the way types are above.
                if (!Target.empty() && !IsExposed(Db, Target) && Target.find("::") == std::string::npos && !Ns.empty())
                {
                    Target = Ns + "::" + Target;
                }

                B.Kind = EBind::SoftObject;
                B.CSharp = (!Target.empty() && IsExposed(Db, Target))
                    ? "global::Lumina.TSoftObjectPtr<" + GlobalCSharp(Target) + ">"
                    : "global::Lumina.FSoftObjectPath";
                return true;
            }

            std::string Q = RawElem;
            const FReflectedType* T = Db.GetReflectedType<FReflectedType>(FStringHash(Q));
            if (T == nullptr && Q.find("::") == std::string::npos && !Ns.empty())
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
                if (S->HasMetadata("CSharpValueMirror") || IsBlittableValueStruct(*S, Db, Size, Align))
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
            return false; // classes (object elements) and anything else are skipped
        }

        // Returns false for kinds with no binding, such as custom accessors, soft objects and optionals.
        bool Classify(FReflectedProperty& Prop, const std::string& OwnerNs, const FReflectionDatabase& Db, FBinding& B)
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
                auto Elem = std::make_unique<FBinding>();
                if (!ClassifyElement(Arr->ElementTypeName, OwnerNs, Db, *Elem))
                {
                    return false; // element kind we don't bind -> skip the whole array
                }
                B.Kind = EBind::Array;
                B.CSharp = Elem->CSharp;  // element C# type; the property type is TSpan<this>
                B.bReadOnly = true;       // read-only view this pass (no add/remove/set)
                B.Elem = std::move(Elem);
                return true;
            }

            if (auto* Del = dynamic_cast<FReflectedDelegateProperty*>(&Prop))
            {
                B.Kind = EBind::Delegate;
                B.bReadOnly = true;

                // Arguments bind through the frame marshaller, so each only has to classify as an element.
                for (const std::string& ArgType : Del->ArgTypeNames)
                {
                    auto Arg = std::make_unique<FBinding>();
                    // An entity is a plain id in both languages, so it reads straight out of the argument pack.
                    if (ArgType == "ECS::FEntity" || ArgType == "Lumina::ECS::FEntity")
                    {
                        Arg->CSharp = "global::LuminaSharp.FEntity";
                    }
                    else if (!ClassifyElement(ArgType, OwnerNs, Db, *Arg))
                    {
                        return false;
                    }
                    if (!B.CSharp.empty())
                    {
                        B.CSharp += ", ";
                    }
                    B.CSharp += Arg->CSharp;
                }
                return true;
            }

            if (auto* Map = dynamic_cast<FReflectedMapProperty*>(&Prop))
            {
                auto Key = std::make_unique<FBinding>();
                auto Val = std::make_unique<FBinding>();
                // The view routes each slot through the element marshaller, as the vector view already did.
                if (!ClassifyElement(Map->KeyTypeName, OwnerNs, Db, *Key)
                 || !ClassifyElement(Map->ValueTypeName, OwnerNs, Db, *Val))
                {
                    return false;
                }
                B.Kind = EBind::Map;
                B.bReadOnly = IsReadOnlyProp(Prop);
                B.Elem = std::move(Key);
                B.Value = std::move(Val);
                return true;
            }

            if (auto* Opt = dynamic_cast<FReflectedOptionalProperty*>(&Prop))
            {
                auto Elem = std::make_unique<FBinding>();
                // A payload that is not a plain value is viewed rather than copied, so it binds too.
                if (!ClassifyElement(Opt->ElementTypeName, OwnerNs, Db, *Elem))
                {
                    return false;
                }
                B.Kind = EBind::Optional;
                B.CSharp = Elem->Kind == EBind::Bool ? "bool" : Elem->CSharp;
                B.bReadOnly = IsReadOnlyProp(Prop);
                B.Elem = std::move(Elem);
                return true;
            }

            const char* KindPtr = Prop.GetTypeName();
            if (KindPtr == nullptr)
            {
                return false; // a kindless property this emitter does not model
            }
            const std::string Kind = KindPtr;
            B.bReadOnly = IsReadOnlyProp(Prop);

            // Checked before the numeric table, which would otherwise bind an entity handle as a bare int.
            if (IsEntityField(Prop))
            {
                B.Kind = EBind::Number;
                B.CSharp = "global::LuminaSharp.FEntity";
                B.Cpp = "uint32";
                return true;
            }
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
            if (Kind == "FString")
            {
                B.Kind = EBind::Str;
                B.CSharp = "string";
                return true;
            }
            if (Kind == "FName")
            {
                BindName(B);
                return true;
            }
            if (Kind == "Object")
            {
                B.Kind = EBind::Object;
                B.bCObject = true;
                B.TargetCpp = Prop.TypeName;
                B.CSharp = IsExposed(Db, Prop.TypeName) ? GlobalCSharp(Prop.TypeName) : "global::LuminaSharp.NativeObject";
                // Settable through the type-erased helper, so only the property's real readonly flags matter here.
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
                const bool bValue = S->HasMetadata("CSharpValueMirror")   // hand-written blittable value type (math)
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
            if (Kind == "Class")
            {
                // Untyped when the base class has no C# mirror, which still resolves and names a class.
                const bool bTyped = !Prop.TypeName.empty() && IsExposed(Db, Prop.TypeName);
                B.Kind = EBind::ClassRef;
                B.TargetCpp = Prop.TypeName;
                B.CSharp = bTyped
                    ? "global::Lumina.TSubclassOf<" + GlobalCSharp(Prop.TypeName) + ">"
                    : "global::Lumina.TSubclassOf<global::LuminaSharp.NativeObject>";
                return true;
            }
            if (Kind == "SubStruct")
            {
                // Untyped when the base struct has no C# mirror, which still resolves and names a struct.
                const bool bTyped = !Prop.TypeName.empty() && IsExposed(Db, Prop.TypeName);
                B.Kind = EBind::SubStructRef;
                B.TargetCpp = Prop.TypeName;
                B.CSharp = bTyped
                    ? "global::Lumina.TSubStructOf<" + GlobalCSharp(Prop.TypeName) + ">"
                    : "global::Lumina.TSubStructOf<global::LuminaSharp.NativeStruct>";
                return true;
            }
            if (Kind == "InstancedStruct")
            {
                // Untyped when the slot names no base struct, which still reads and writes by runtime name.
                const bool bTyped = !Prop.TypeName.empty() && IsExposed(Db, Prop.TypeName);
                B.Kind = EBind::InstancedStruct;
                B.TargetCpp = Prop.TypeName;
                B.CSharp = bTyped
                    ? "global::Lumina.TInstancedStruct<" + GlobalCSharp(Prop.TypeName) + ">"
                    : "global::Lumina.FInstancedStruct";
                return true;
            }
            if (Kind == "SoftObject")
            {
                // Untyped when the target class has no C# mirror, which still resolves and loads by path.
                const bool bTyped = Prop.RawTypeName.find("TSoftObjectPtr") != std::string::npos
                                 && IsExposed(Db, Prop.TypeName);
                B.Kind = EBind::SoftObject;
                B.TargetCpp = Prop.TypeName;
                B.CSharp = bTyped
                    ? "global::Lumina.TSoftObjectPtr<" + GlobalCSharp(Prop.TypeName) + ">"
                    : "global::Lumina.FSoftObjectPath";
                return true;
            }
            return false; // anything else is not bound yet
        }

        // A bit-for-bit identical element lets the whole TVector be read as a zero-copy ReadOnlySpan<T>.
        static bool IsBlittableElementKind(EBind Kind)
        {
            return Kind == EBind::Number || Kind == EBind::Bool || Kind == EBind::Enum || Kind == EBind::StructValue;
        }

        // A blittable element reads in place at the offset, everything else falls back to per-index thunks.
        void EmitCSharpArray(FCodeWriter& Writer, FReflectedProperty& Prop, const FBinding& B,
            const std::string& Friendly, const std::string& Module, const std::string& TypeName)
        {
            const std::string& Member = Prop.Name;
            const std::string PropName = SafeIdentifier(Member);

            if (IsBlittableElementKind(B.Elem->Kind))
            {
                const std::string OffName = "__off_" + Member;
                if (IsReadOnlyProp(Prop))
                {
                    // Read-only, so a zero-copy ReadOnlySpan<T> over the native storage.
                    Writer.Linef("public global::System.ReadOnlySpan<%s> %s =>", B.Elem->CSharp.c_str(), PropName.c_str());
                    Writer.Linef("    global::LuminaSharp.NativeMarshal.ReadVector<%s>(Self, %s);", B.Elem->CSharp.c_str(), OffName.c_str());
                }
                else
                {
                    // Reads decode the vector header in place, while Add and Remove call the ops function pointers.
                    const std::string OpsFn = "__vecopsfn_" + Member;
                    const std::string OpsField = "__ops_" + Member;
                    const std::string OpsThunk = "LuminaSharp_VecOps_" + Friendly + "_" + Member;
                    Writer.Linef("public global::Lumina.TVector<%s> %s =>", B.Elem->CSharp.c_str(), PropName.c_str());
                    Writer.Linef("    new global::Lumina.TVector<%s>(global::LuminaSharp.NativeBindings.FieldAt(Self, %s), %s);", B.Elem->CSharp.c_str(), OffName.c_str(), OpsField.c_str());
                    Writer.Linef("private static readonly delegate* unmanaged[Cdecl]<nint> %s =", OpsFn.c_str());
                    Writer.Linef("    (delegate* unmanaged[Cdecl]<nint>)global::LuminaSharp.NativeBindings.Resolve(\"%s\", \"%s\");",
                        Module.c_str(), OpsThunk.c_str());
                    Writer.Linef("private static readonly nint %s = %s();", OpsField.c_str(), OpsFn.c_str());
                }
                EmitOffsetField(Writer, OffName, Prop, TypeName);
                return;
            }

            // TVector carries every element kind, and only ElementMarshal differs between them.
            if (B.Elem->Kind == EBind::StructOpaque || B.Elem->Kind == EBind::Str || B.Elem->Kind == EBind::SoftObject)
            {
                const std::string Element = B.Elem->Kind == EBind::Str
                    ? std::string("global::Lumina.FString")
                    : B.Elem->CSharp;
                const std::string View = "global::Lumina.TVector<" + Element + ">";

                const std::string OffName2 = "__off_" + Member;
                const std::string OpsFn2 = "__vecopsfn_" + Member;
                const std::string OpsField2 = "__ops_" + Member;
                const std::string OpsThunk2 = "LuminaSharp_VecOps_" + Friendly + "_" + Member;

                Writer.Linef("public %s %s =>", View.c_str(), PropName.c_str());
                Writer.Linef("    new %s(global::LuminaSharp.NativeBindings.FieldAt(Self, %s), %s);", View.c_str(), OffName2.c_str(), OpsField2.c_str());
                EmitOffsetField(Writer, OffName2, Prop, TypeName);
                Writer.Linef("private static readonly delegate* unmanaged[Cdecl]<nint> %s =", OpsFn2.c_str());
                Writer.Linef("    (delegate* unmanaged[Cdecl]<nint>)global::LuminaSharp.NativeBindings.Resolve(\"%s\", \"%s\");",
                    Module.c_str(), OpsThunk2.c_str());
                Writer.Linef("private static readonly nint %s = %s();", OpsField2.c_str(), OpsFn2.c_str());
                return;
            }

        }

        // The container type comes from decltype, since an unqualified TVector<T> would not resolve here.
        void EmitVectorOpsExport(FCodeWriter& Writer, FReflectedProperty& Prop,
            const std::string& Friendly, const char* Qualified, const char* Api)
        {
            const std::string Thunk = "LuminaSharp_VecOps_" + Friendly + "_" + Prop.Name;
            Writer.Linef("extern \"C\" %s const void* %s() { return ::Lumina::GetVectorOpsFor<decltype(%s::%s)>(); }",
                Api, Thunk.c_str(), Qualified, Prop.Name.c_str());
        }

        // The clang layout gives the offset at generation time, which the generated C++ asserts against the real compiler.
        void EmitOffsetField(FCodeWriter& Writer, const std::string& OffName, const FReflectedProperty& Prop, const std::string& TypeName)
        {
            if (Prop.NativeOffset >= 0)
            {
                Writer.Linef("private const int %s = %lld;", OffName.c_str(), (long long)Prop.NativeOffset);
                return;
            }
            Writer.Linef("private static readonly int %s = (int)global::LuminaSharp.NativeBindings.PropertyOffset(\"%s\", \"%s\");",
                OffName.c_str(), TypeName.c_str(), Prop.Name.c_str());
        }

        // The FProperty* resolved ONCE per property, reused by the generic per-type exporters.
        void EmitTokenField(FCodeWriter& Writer, const std::string& PropFieldName, const std::string& TypeName, const std::string& Member)
        {
            Writer.Linef("private static readonly System.IntPtr %s = global::LuminaSharp.NativeBindings.FindProperty(\"%s\", \"%s\");",
                PropFieldName.c_str(), TypeName.c_str(), Member.c_str());
        }

        // Blittable kinds read native memory at the resolved offset, non-blittable route through exporters.
        void EmitCSharpMember(FCodeWriter& Writer, FReflectedProperty& Prop, const FBinding& B,
            const std::string& Friendly, const std::string& Module, const std::string& TypeName, const std::string& WriteHook, bool bClassOwner)
        {
            // A plain expression setter, or a block that also tells the type a script changed it.
            auto EmitSetter = [&Writer, &WriteHook](const std::string& Statement)
            {
                if (WriteHook.empty())
                {
                    Writer.Linef("set => %s;", Statement.c_str());
                    return;
                }
                Writer.Line("set");
                Writer.BeginBlock();
                Writer.Linef("%s;", Statement.c_str());
                Writer.Linef("%s();", WriteHook.c_str());
                Writer.EndBlock();
            };
            auto Format = [](const char* Pattern, auto... Args)
            {
                char Buffer[1024];
                std::snprintf(Buffer, sizeof(Buffer), Pattern, Args...);
                return std::string(Buffer);
            };
            const std::string& Member = Prop.Name;
            const std::string PropName = SafeIdentifier(Member);
            const std::string OffName = "__off_" + Member;
            const std::string PropFieldName = "__prop_" + Member;
            const bool bRO = B.bReadOnly;
            const char* CS = B.CSharp.c_str();
            if (B.Kind != EBind::Span && B.Kind != EBind::None)
            {
                EmitDocSummary(Writer, Prop.Metadata);
            }

            switch (B.Kind)
            {
                case EBind::StructValue:
                {
                    // A ref aliases the native field, since a by-value get would hand back a copy and drop per-field edits.
                    if (WriteHook.empty() || bRO)
                    {
                        Writer.Linef("public ref %s%s %s => ref global::System.Runtime.CompilerServices.Unsafe.AsRef<%s>((void*)(global::LuminaSharp.NativeBindings.FieldAt(Self, %s)));",
                            bRO ? "readonly " : "", CS, PropName.c_str(), CS, OffName.c_str());
                        EmitOffsetField(Writer, OffName, Prop, TypeName);
                        break;
                    }

                    // A write through the ref cannot call the hook afterward, so handing the ref out is what marks it.
                    Writer.Linef("public ref %s %s", CS, PropName.c_str());
                    Writer.BeginBlock();
                    Writer.Line("get");
                    Writer.BeginBlock();
                    Writer.Linef("%s();", WriteHook.c_str());
                    Writer.Linef("return ref global::System.Runtime.CompilerServices.Unsafe.AsRef<%s>((void*)(global::LuminaSharp.NativeBindings.FieldAt(Self, %s)));", CS, OffName.c_str());
                    Writer.EndBlock();
                    Writer.EndBlock();
                    EmitOffsetField(Writer, OffName, Prop, TypeName);
                    break;
                }
                case EBind::Number:
                case EBind::Bool:
                case EBind::Enum:
                {
                    // A ref rather than a setter, so a write lands even through a handle returned by value from a property.
                    const char* T = (B.Kind == EBind::Bool) ? "bool" : CS;
                    if (WriteHook.empty() || bRO)
                    {
                        Writer.Linef("public ref %s%s %s => ref global::System.Runtime.CompilerServices.Unsafe.AsRef<%s>((void*)(global::LuminaSharp.NativeBindings.FieldAt(Self, %s)));",
                            bRO ? "readonly " : "", T, PropName.c_str(), T, OffName.c_str());
                        EmitOffsetField(Writer, OffName, Prop, TypeName);
                        break;
                    }

                    // A write through the ref cannot call the hook afterward, so handing the ref out is what marks it.
                    Writer.Linef("public ref %s %s", T, PropName.c_str());
                    Writer.BeginBlock();
                    Writer.Line("get");
                    Writer.BeginBlock();
                    Writer.Linef("%s();", WriteHook.c_str());
                    Writer.Linef("return ref global::System.Runtime.CompilerServices.Unsafe.AsRef<%s>((void*)(global::LuminaSharp.NativeBindings.FieldAt(Self, %s)));", T, OffName.c_str());
                    Writer.EndBlock();
                    Writer.EndBlock();
                    EmitOffsetField(Writer, OffName, Prop, TypeName);
                    break;
                }
                case EBind::Str:
                {
                    Writer.Linef("public string %s", PropName.c_str());
                    Writer.BeginBlock();
                    // The FString read decodes in place, while the set still assigns through the native allocator.
                    Writer.Linef("get => global::LuminaSharp.NativeMarshal.ReadString(global::LuminaSharp.NativeBindings.FieldAt(Self, %s));", OffName.c_str());
                    if (!bRO)
                    {
                        EmitSetter(Format("global::LuminaSharp.Native.StringAssign(global::LuminaSharp.NativeBindings.FieldAt(Self, %s), value)", OffName.c_str()));
                    }
                    Writer.EndBlock();
                    EmitOffsetField(Writer, OffName, Prop, TypeName);
                    break;
                }
                case EBind::Object:
                {
                    Writer.Linef("public %s %s", CS, PropName.c_str());
                    Writer.BeginBlock();
                    Writer.Line("get");
                    Writer.BeginBlock();
                    Writer.Linef("System.IntPtr __h = global::LuminaSharp.Native.GetObjectPtr(global::LuminaSharp.NativeBindings.FieldAt(Self, %s));", OffName.c_str());
                    if (B.bCObject)
                    {
                        // Reading the same object property twice returns the SAME managed instance, so identity holds.
                        Writer.Linef("return global::LuminaSharp.NativeObjectMarshal.FromHandle<%s>(__h);", CS);
                    }
                    else
                    {
                        Writer.Linef("return global::LuminaSharp.NativeStructs.FromAddressOrNull<%s>(__h);", StripNullable(B.CSharp).c_str());
                    }
                    Writer.EndBlock();
                    if (!bRO)
                    {
                        EmitSetter(B.bCObject
                            ? Format("global::LuminaSharp.Native.SetObjectPtr(global::LuminaSharp.NativeBindings.FieldAt(Self, %s), global::LuminaSharp.NativeObjectMarshal.ToHandle(value))", OffName.c_str())
                            : Format("global::LuminaSharp.Native.SetObjectPtr(global::LuminaSharp.NativeBindings.FieldAt(Self, %s), global::LuminaSharp.NativeStructs.AddressOf(value))", OffName.c_str()));
                    }
                    Writer.EndBlock();
                    EmitOffsetField(Writer, OffName, Prop, TypeName);
                    break;
                }
                case EBind::Optional:
                {
                    // A payload a Nullable cannot spell is viewed through the slot and its property instead.
                    if (B.Elem != nullptr && !IsBlittableElementKind(B.Elem->Kind))
                    {
                        Writer.Linef("public global::Lumina.TOptional<%s> %s =>", CS, PropName.c_str());
                        Writer.Linef("    new global::Lumina.TOptional<%s>(global::LuminaSharp.NativeBindings.FieldAt(Self, %s), %s);", CS,
                            OffName.c_str(), PropFieldName.c_str());
                        EmitOffsetField(Writer, OffName, Prop, TypeName);
                        EmitTokenField(Writer, PropFieldName, TypeName, Member);
                        break;
                    }
                    // Nullable mirrors TOptional exactly, and the payload is blittable so it reads in place.
                    Writer.Linef("public %s? %s", CS, PropName.c_str());
                    Writer.BeginBlock();
                    Writer.Line("get");
                    Writer.BeginBlock();
                    Writer.Linef("void* __v = (void*)global::LuminaSharp.Native.PropOptionalGetValue(Self, %s);", PropFieldName.c_str());
                    Writer.Linef("return __v == null ? default(%s?) : global::System.Runtime.CompilerServices.Unsafe.ReadUnaligned<%s>(__v);", CS, CS);
                    Writer.EndBlock();
                    if (!bRO)
                    {
                        Writer.Line("set");
                        Writer.BeginBlock();
                        Writer.Line("if (value.HasValue)");
                        Writer.BeginBlock();
                        Writer.Linef("%s __t = value.Value;", CS);
                        Writer.Linef("global::LuminaSharp.Native.PropOptionalSetValue(Self, %s, (System.IntPtr)(&__t));", PropFieldName.c_str());
                        Writer.EndBlock();
                        Writer.Line("else");
                        Writer.BeginBlock();
                        Writer.Linef("global::LuminaSharp.Native.PropOptionalReset(Self, %s);", PropFieldName.c_str());
                        Writer.EndBlock();
                        if (!WriteHook.empty())
                        {
                            Writer.Linef("%s();", WriteHook.c_str());
                        }
                        Writer.EndBlock();
                    }
                    Writer.EndBlock();
                    EmitTokenField(Writer, PropFieldName, TypeName, Member);
                    break;
                }
                case EBind::SoftObject:
                {
                    // One FSoftObjectPath natively whatever the spelling, so the path is the whole value.
                    Writer.Linef("public %s %s", CS, PropName.c_str());
                    Writer.BeginBlock();
                    Writer.Linef("get => global::LuminaSharp.SoftObjectReferenceMarshal.Read<%s>(global::LuminaSharp.Native.SoftPathGet(global::LuminaSharp.NativeBindings.FieldAt(Self, %s)));",
                        CS, OffName.c_str());
                    if (!bRO)
                    {
                        EmitSetter(Format("global::LuminaSharp.Native.SoftPathSet(global::LuminaSharp.NativeBindings.FieldAt(Self, %s), global::LuminaSharp.SoftObjectReferenceMarshal.Write(value))", OffName.c_str()));
                    }
                    Writer.EndBlock();
                    EmitOffsetField(Writer, OffName, Prop, TypeName);
                    break;
                }
                case EBind::ClassRef:
                {
                    Writer.Linef("public %s %s", CS, PropName.c_str());
                    Writer.BeginBlock();
                    Writer.Linef("get => new %s(global::LuminaSharp.Native.PropGetClass(Self, %s));", CS, PropFieldName.c_str());
                    if (!bRO)
                    {
                        EmitSetter(Format("global::LuminaSharp.Native.PropSetClass(Self, %s, value.ClassPtr)", PropFieldName.c_str()));
                    }
                    Writer.EndBlock();
                    EmitTokenField(Writer, PropFieldName, TypeName, Member);
                    break;
                }
                case EBind::SubStructRef:
                {
                    Writer.Linef("public %s %s", CS, PropName.c_str());
                    Writer.BeginBlock();
                    Writer.Linef("get => new %s(global::LuminaSharp.Native.PropGetSubStruct(Self, %s));", CS, PropFieldName.c_str());
                    if (!bRO)
                    {
                        EmitSetter(Format("global::LuminaSharp.Native.PropSetSubStruct(Self, %s, value.StructPtr)", PropFieldName.c_str()));
                    }
                    Writer.EndBlock();
                    EmitTokenField(Writer, PropFieldName, TypeName, Member);
                    break;
                }
                case EBind::InstancedStruct:
                {
                    // A view over the slot, so the stored type and value are reached through it rather than copied.
                    Writer.Linef("public %s %s => new %s(global::LuminaSharp.NativeBindings.FieldAt(Self, %s));", CS, PropName.c_str(), CS, OffName.c_str());
                    EmitOffsetField(Writer, OffName, Prop, TypeName);
                    break;
                }
                case EBind::StructOpaque:
                {
                    // A handle on the embedded struct, tracked through the owner, so edits through it land in place.
                    if (!bClassOwner)
                    {
                        Writer.Linef("public %s %s => new %s(SelfRef.Field(%s));", CS, PropName.c_str(), CS, OffName.c_str());
                        EmitOffsetField(Writer, OffName, Prop, TypeName);
                        break;
                    }

                    // A ref to a cached handle, since a setter called on a handle returned by value is a compile error.
                    const std::string Cache = "__view_" + Member;
                    Writer.Linef("public ref readonly %s %s { get { %s = new %s(SelfRef.Field(%s)); return ref %s; } }",
                        CS, PropName.c_str(), Cache.c_str(), CS, OffName.c_str(), Cache.c_str());
                    Writer.Linef("private %s %s;", CS, Cache.c_str());
                    EmitOffsetField(Writer, OffName, Prop, TypeName);
                    break;
                }
                case EBind::Array:
                    EmitCSharpArray(Writer, Prop, B, Friendly, Module, TypeName);
                    break;
                case EBind::Map:
                {
                    // The ops come from the property token, so the token field has to initialize first.
                    const std::string MapOps = "__mapops_" + Member;
                    const std::string View = "global::Lumina.THashMap<" + B.Elem->CSharp + ", " + B.Value->CSharp + ">";
                    Writer.Linef("public %s %s => new %s(global::LuminaSharp.NativeBindings.FieldAt(Self, %s), %s);",
                        View.c_str(), PropName.c_str(), View.c_str(), OffName.c_str(), MapOps.c_str());
                    EmitTokenField(Writer, PropFieldName, TypeName, Member);
                    EmitOffsetField(Writer, OffName, Prop, TypeName);
                    Writer.Linef("private static readonly nint %s = (nint)global::LuminaSharp.Native.PropMapOps(%s);",
                        MapOps.c_str(), PropFieldName.c_str());
                    break;
                }
                case EBind::Delegate:
                {
                    // An event, so scripts subscribe with += and -=; the view underneath does the native binding.
                    const std::string ViewName = "__delegate_" + Member;
                    if (B.CSharp.empty())
                    {
                        Writer.Linef("public event global::System.Action %s { add => %s.Bind(value); remove => %s.Remove(value); }",
                            PropName.c_str(), ViewName.c_str(), ViewName.c_str());
                        Writer.Linef("private global::LuminaSharp.ScriptDelegate %s => new global::LuminaSharp.ScriptDelegate((void*)(global::LuminaSharp.NativeBindings.FieldAt(Self, %s)));",
                            ViewName.c_str(), OffName.c_str());
                    }
                    else
                    {
                        // The property token is what the argument slots are resolved from, once per signature.
                        Writer.Linef("public event global::System.Action<%s> %s { add => %s.Bind(value); remove => %s.Remove(value); }",
                            CS, PropName.c_str(), ViewName.c_str(), ViewName.c_str());
                        Writer.Linef("private global::LuminaSharp.ScriptDelegate<%s> %s => new global::LuminaSharp.ScriptDelegate<%s>((void*)(global::LuminaSharp.NativeBindings.FieldAt(Self, %s)), %s);",
                            CS, ViewName.c_str(), CS, OffName.c_str(), PropFieldName.c_str());
                        EmitTokenField(Writer, PropFieldName, TypeName, Member);
                    }
                    EmitOffsetField(Writer, OffName, Prop, TypeName);
                    break;
                }
                case EBind::Span:
                case EBind::None:
                case EBind::Pointer:
                    break;
            }
        }

        // Only a non-blittable array element still needs a thunk, so the count is no longer O(properties).
        void EmitNativeThunk(FCodeWriter& Writer, FReflectedProperty& Prop, const FBinding& B,
            const std::string& Friendly, const char* Qualified, const char* Api)
        {
            if (B.Kind != EBind::Array)
            {
                return;
            }
            if (!IsBlittableElementKind(B.Elem->Kind))
            {
                // EmitCSharpArray views these kinds as a TVector, whose static initializer calls the ops export.
                EmitVectorOpsExport(Writer, Prop, Friendly, Qualified, Api);
            }
            else if (!IsReadOnlyProp(Prop))
            {
                // A read-only blittable array needs no export, since C# reads the header directly as a ReadOnlySpan.
                EmitVectorOpsExport(Writer, Prop, Friendly, Qualified, Api);
            }
        }

        // Emits the managed members for every bindable property on an opaque wrapper.
        void EmitProperties(FCodeWriter& Writer, const FReflectedStruct& Type, const FReflectionDatabase& Db, std::set<std::string>* Emitted = nullptr, bool bClassOwner = false)
        {
            const std::string Friendly = Names::FriendlyFromQualified(Type.QualifiedName);
            const std::string Module = ModuleOf(Type);
            const std::string WriteHook = FindScriptWriteHook(Type, Db);
            for (const auto& Prop : Type.Props)
            {
                if (Prop->bInner || IsScriptHidden(*Prop))
                {
                    continue;
                }
                if (Emitted != nullptr && !Emitted->insert(Prop->Name).second)
                {
                    continue;
                }
                FBinding B;
                if (Classify(*Prop, Type.Namespace, Db, B))
                {
                    EmitCSharpMember(Writer, *Prop, B, Friendly, Module, Type.DisplayName, WriteHook, bClassOwner);
                }
            }
        }

        // "a" / 3 -> "a3". Used to name generated function parameters.
        std::string ArgIndexName(char Prefix, size_t I)
        {
            std::string Out;
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
            std::string CSharp;
            std::string Cpp;
            std::string TargetCpp;
            bool          bEntity = false; // An entity handle, ABI-marshalled as uint32 and surfaced as the C# FEntity.
            // Only a CObject has a managed-instance slot, so only a CObject may be rebuilt through ForObject.
            bool          bCObject = false;
            bool          bSoftPath = false; // EBind::Str arg, a soft reference (TargetCpp) built from the marshalled path.
            // A (T* ptr, int32 count) C++ pair maps to one C# Span, with bReadOnlySpan tracking constness.
            std::string SpanElemCpp;
            bool          bReadOnlySpan = false;
            // A const TVector<T>& arg, rebuilt from the span on the native side rather than passed as a pair.
            bool          bVectorArg = false;
            // A mutable TVector<T>& the callee fills, surfaced as the C# return over the two-pass buffer ABI.
            bool          bVectorOut = false;
            std::string VectorElemCpp;
            // Set only for an object element, where the caller buffer is a raw pointer array.
            std::string VectorElemBufferCpp;
            // TStrongObjectPtr<T> by value, which marshals as the handle but has to be re-wrapped at the call.
            bool          bObjectPtrValue = false;
            // The C++ parameter's own name, so the C# signature reads like the declaration it came from.
            std::string Name;
            // The C++ default rewritten as a C# constant, empty when the expression has no C# equivalent.
            std::string DefaultCSharp;
        };

        std::string ArgName(const FArg& Arg, size_t Index)
        {
            return Arg.Name.empty() ? ArgIndexName('a', Index) : SafeIdentifier(Arg.Name);
        }

        bool IsNumericLiteral(const std::string& Text)
        {
            size_t Index = (Text[0] == '-' || Text[0] == '+') ? 1 : 0;
            if (Index >= Text.size())
            {
                return false;
            }
            bool bDigit = false;
            for (; Index < Text.size(); ++Index)
            {
                const char C = Text[Index];
                if (C >= '0' && C <= '9') { bDigit = true; continue; }
                if (C == '.' || C == 'f' || C == 'F' || C == 'x' || C == 'X'
                    || C == 'u' || C == 'U' || C == 'l' || C == 'L'
                    || (C >= 'a' && C <= 'e') || (C >= 'A' && C <= 'E')) { continue; }
                return false;
            }
            return bDigit;
        }

        // Only an expression C# accepts as a compile-time constant converts; anything else drops its default.
        std::string DefaultToCSharp(const FArg& Arg, const std::string& Cpp)
        {
            if (Cpp.empty())
            {
                return std::string();
            }
            if (Arg.Kind == EBind::Bool)
            {
                return (Cpp == "true" || Cpp == "false") ? Cpp : std::string();
            }
            if (Arg.Kind == EBind::Object)
            {
                return Cpp == "nullptr" ? std::string("null") : std::string();
            }
            if (Arg.Kind == EBind::Str)
            {
                // A default-constructed string is the empty one, which is what C# can express.
                if (Cpp == "nullptr") { return std::string("null"); }
                if (Cpp == "FString()" || Cpp == "\"\"") { return std::string("\"\""); }
                return std::string();
            }
            if (Arg.Kind == EBind::StructValue && Arg.TargetCpp == "Lumina::FName")
            {
                return (Cpp == "FName()" || Cpp == "NAME_None" || Cpp == "\"\"") ? std::string("default") : std::string();
            }
            if (Arg.Kind == EBind::Enum)
            {
                const size_t Scope = Cpp.rfind("::");
                if (Scope == std::string::npos || Scope + 2 >= Cpp.size())
                {
                    return std::string();
                }
                return Arg.CSharp + "." + Cpp.substr(Scope + 2);
            }
            if (Arg.Kind == EBind::Number && !Arg.bEntity)
            {
                return IsNumericLiteral(Cpp) ? Cpp : std::string();
            }
            return std::string();
        }

        // C# only allows trailing optional parameters, so a gap drops every default to its left.
        void ResolveDefaults(std::vector<FArg>& Args)
        {
            bool bTailOptional = true;
            for (size_t i = Args.size(); i-- > 0; )
            {
                if (!bTailOptional || Args[i].DefaultCSharp.empty())
                {
                    bTailOptional = false;
                    Args[i].DefaultCSharp.clear();
                }
            }
        }

        // Maps a bare C++ numeric spelling ("uint32") to its C# type ("uint"); false if not a numeric.
        bool NumericCSharp(const std::string& Bare, std::string& OutCSharp)
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

        // Type spelling with const, ampersand, star and whitespace removed, so const FName & becomes FName.
        std::string StripQualifiers(const std::string& T)
        {
            std::string Out;
            for (char C : T)
            {
                if (C != ' ' && C != '\t' && C != '&' && C != '*')
                {
                    Out += C;
                }
            }
            for (size_t P; (P = Out.find("const")) != std::string::npos; )
            {
                Out.erase(P, 5);
            }
            return Out;
        }

        // Clang qualifies a function field to the bare template name, so the argument comes from the spelling.
        std::string FirstTemplateArgument(const std::string& Raw)
        {
            const size_t Open = Raw.find('<');
            const size_t Close = Raw.rfind('>');
            if (Open == std::string::npos || Close == std::string::npos || Close < Open)
            {
                return {};
            }
            int32_t Depth = 0;
            for (size_t i = Open + 1; i < Close; ++i)
            {
                const char C = Raw[i];
                Depth += (C == '<');
                Depth -= (C == '>');
                if (C == ',' && Depth == 0)
                {
                    return StripQualifiers(Raw.substr(Open + 1, i - Open - 1));
                }
            }
            return StripQualifiers(Raw.substr(Open + 1, Close - Open - 1));
        }

        // A template argument arrives unqualified as often as not, so the engine namespace is the fallback.
        std::string ResolveTargetType(const FReflectionDatabase& Db, const std::string& Name)
        {
            if (Name.empty() || IsOpaqueWrapperType(Db, Name) || IsObjectRootType(Name))
            {
                return Name;
            }
            const size_t Scope = Name.rfind("::");
            const std::string Qualified = "Lumina::" + (Scope == std::string::npos ? Name : Name.substr(Scope + 2));
            return (IsOpaqueWrapperType(Db, Qualified) || IsObjectRootType(Qualified)) ? Qualified : Name;
        }

        // The C# and fully-qualified C++ spellings for an element the two-pass buffer can carry by value.
        // OutBufferCpp is set only for an object element, whose caller buffer holds raw pointers rather
        // than the refcounted TStrongObjectPtr the callee fills.
        bool VectorElementCSharp(const FReflectionDatabase& Db, const std::string& Elem,
            std::string& OutCSharp, std::string& OutCpp, std::string& OutBufferCpp)
        {
            OutBufferCpp.clear();
            if (Elem.empty())
            {
                return false;
            }
            if (IsEntitySpelling(Elem))
            {
                OutCSharp = "global::LuminaSharp.FEntity";
                OutCpp = "Lumina::ECS::FEntity";
                return true;
            }
            if (NumericCSharp(Elem, OutCSharp))
            {
                OutCpp = Elem;
                return true;
            }
            // A reflected container of objects holds TStrongObjectPtr, since LRT1001 refuses a raw pointer there.
            if (Elem.rfind("TStrongObjectPtr<", 0) == 0 || Elem.rfind("Lumina::TStrongObjectPtr<", 0) == 0)
            {
                const std::string Target = ResolveTargetType(Db, FirstTemplateArgument(Elem));
                const bool bRoot = IsObjectRootType(Target);
                if (!bRoot && !IsOpaqueWrapperType(Db, Target))
                {
                    return false;
                }
                const std::string Qualified = bRoot ? std::string("Lumina::CObject") : Target;
                OutCSharp = bRoot ? "global::LuminaSharp.NativeObject" : GlobalCSharp(Target);
                OutCpp = "Lumina::TStrongObjectPtr<" + Qualified + ">";
                OutBufferCpp = Qualified + "*";
                return true;
            }

            const FReflectedStruct* S = Db.GetReflectedType<FReflectedStruct>(FStringHash(Elem));
            if (S == nullptr)
            {
                const std::string Qualified = "Lumina::" + Elem;
                S = Db.GetReflectedType<FReflectedStruct>(FStringHash(Qualified));
                if (S == nullptr)
                {
                    return false;
                }
            }

            int Size = 0;
            int Align = 0;
            if (!S->HasMetadata("CSharpValueMirror") && !IsBlittableValueStruct(*S, Db, Size, Align))
            {
                return false;
            }
            OutCSharp = GlobalCSharp(S->QualifiedName);
            OutCpp = S->QualifiedName;
            return true;
        }

        // Conservative by design, so a strong type that merely classifies as an int is skipped, not coerced.
        bool ClassifyField(const FFieldInfo& F, const FReflectionDatabase& Db, bool bIsArg, FArg& B)
        {
            // A pointer to a reflected class or opaque struct marshals as the engine handle, void* at the thunk.
            if (F.RawFieldType.find('*') != std::string::npos)
            {
                // Untyped memory whose type only the caller knows crosses as a bare address.
                if (StripQualifiers(F.RawFieldType) == "void")
                {
                    B.Kind = EBind::Pointer;
                    B.CSharp = "global::System.IntPtr";
                    B.Cpp = "void*";
                    return true;
                }
                if ((F.Flags == EPropertyTypeFlags::Object || F.Flags == EPropertyTypeFlags::Struct)
                    && IsOpaqueWrapperType(Db, F.TypeName))
                {
                    B.Kind = EBind::Object;
                    B.bCObject = (F.Flags == EPropertyTypeFlags::Object);
                    B.TargetCpp = F.TypeName;
                    // A returned component pointer is a tracked reference, invalid where the native call returned null.
                    B.CSharp = !bIsArg && IsComponentType(Db, F.TypeName)
                        ? "global::LuminaSharp.TComponentRef<" + GlobalCSharp(F.TypeName) + ">"
                        : GlobalCSharp(F.TypeName) + (B.bCObject ? "" : "?");
                    return true;
                }
                // CObject never enters the database, so without this the whole function would be dropped.
                if (F.Flags == EPropertyTypeFlags::Object && IsObjectRootType(F.TypeName))
                {
                    B.Kind = EBind::Object;
                    B.bCObject = true;
                    B.TargetCpp = "Lumina::CObject";
                    B.CSharp = "global::LuminaSharp.NativeObject";
                    return true;
                }
                return false; // any other pointer in/out -> ambiguous marshaling
            }
            // Checked ahead of the refusal below, which is about a by-value arg, not an out container.
            if (bIsArg && F.Flags == EPropertyTypeFlags::Vector && F.TypeName == "Lumina::TVector"
                && F.RawFieldType.find('&') != std::string::npos
                && F.RawFieldType.find("const") == std::string::npos)
            {
                const std::string Elem = FirstTemplateArgument(F.RawFieldType);
                std::string ElemCS;
                std::string ElemCpp;
                std::string ElemBufferCpp;
                if (!VectorElementCSharp(Db, Elem, ElemCS, ElemCpp, ElemBufferCpp))
                {
                    return false;
                }
                B.Kind = EBind::Span;
                B.bVectorOut = true;
                B.VectorElemCpp = ElemCpp;
                B.VectorElemBufferCpp = ElemBufferCpp;
                B.CSharp = ElemCS;
                return true;
            }

            if (bIsArg && F.RawFieldType.find('&') != std::string::npos
                && F.RawFieldType.find("const") == std::string::npos)
            {
                return false; // mutable-reference arg can't take a by-value
            }

            const std::string Bare = StripQualifiers(F.RawFieldType);

            // A soft reference arg crosses as its path, since a return would need the two-pass string protocol.
            if (bIsArg && (Bare.find("FSoftObjectPath") != std::string::npos || Bare.find("TSoftObjectPtr<") != std::string::npos))
            {
                B.Kind = EBind::Str;
                B.bSoftPath = true;
                B.TargetCpp = "Lumina::FSoftObjectPath";
                B.CSharp = "global::Lumina.FSoftObjectPath";
                if (Bare.find("TSoftObjectPtr<") != std::string::npos)
                {
                    const std::string Target = ResolveTargetType(Db, FirstTemplateArgument(F.RawFieldType));
                    B.TargetCpp = "Lumina::TSoftObjectPtr<" + Target + ">";
                    if (IsExposed(Db, Target))
                    {
                        B.CSharp = "global::Lumina.TSoftObjectPtr<" + GlobalCSharp(Target) + ">";
                    }
                }
                return true;
            }

            // An ECS::FEntity crosses as its packed id and surfaces as the C# FEntity.
            if (IsEntitySpelling(F.RawFieldType) || IsEntitySpelling(F.TypeName))
            {
                B.Kind = EBind::Number;
                B.CSharp = "global::LuminaSharp.FEntity";
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
                    // Clang canonicalizes int32 to int, so the engine alias is what the source actually wrote.
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
                    if (S->HasMetadata("CSharpValueMirror") || IsBlittableValueStruct(*S, Db, Size, Align))
                    {
                        B.Kind = EBind::StructValue; B.CSharp = GlobalCSharp(F.TypeName); B.TargetCpp = F.TypeName; return true;
                    }
                    // An opaque struct binds for args only; a return has no storage for the wrapper to view.
                    if (bIsArg && IsOpaqueWrapperType(Db, F.TypeName))
                    {
                        B.Kind = EBind::StructOpaque; B.CSharp = GlobalCSharp(F.TypeName); B.TargetCpp = F.TypeName; return true;
                    }
                    return false;
                }
                case EPropertyTypeFlags::Name:
                    BindName(B);
                    return true;
                case EPropertyTypeFlags::String:
                    // An arg passes (UTF-8 byte*, len) while a return uses the engine two-pass caller-buffer protocol.
                    B.Kind = EBind::Str; B.CSharp = "string";
                    return true;
                case EPropertyTypeFlags::Object:
                {
                    // TStrongObjectPtr<T> by value; the pointer spelling took the branch above and the handle ABI matches.
                    if (F.TypeName != "Lumina::TStrongObjectPtr")
                    {
                        return false;
                    }
                    const std::string Target = ResolveTargetType(Db, FirstTemplateArgument(F.RawFieldType));
                    if (!IsOpaqueWrapperType(Db, Target) && !IsObjectRootType(Target))
                    {
                        return false;
                    }
                    B.Kind = EBind::Object;
                    B.bCObject = true;
                    B.bObjectPtrValue = true;
                    const bool bRoot = IsObjectRootType(Target);
                    B.TargetCpp = bRoot ? "Lumina::CObject" : Target;
                    B.CSharp = bRoot ? "global::LuminaSharp.NativeObject" : GlobalCSharp(Target);
                    return true;
                }
                case EPropertyTypeFlags::Vector:
                {
                    // Read-only by-reference only, so the span never outlives the call and nothing is written back.
                    const std::string Elem = FirstTemplateArgument(F.RawFieldType);
                    std::string ElemCS;
                    // TFixedVector shares the flag but not the constructor the thunk rebuilds through.
                    if (F.TypeName != "Lumina::TVector"
                        || !bIsArg || F.RawFieldType.find('&') == std::string::npos
                        || F.RawFieldType.find("const") == std::string::npos
                        || !NumericCSharp(Elem, ElemCS))
                    {
                        return false;
                    }
                    B.Kind = EBind::Span;
                    B.bReadOnlySpan = true;
                    B.bVectorArg = true;
                    B.SpanElemCpp = Elem;
                    B.CSharp = "global::System.ReadOnlySpan<" + ElemCS + ">";
                    return true;
                }
                case EPropertyTypeFlags::Class:
                {
                    // TSubclassOf is one CClass* at offset zero, so it crosses as that pointer.
                    const std::string Target = ResolveTargetType(Db, FirstTemplateArgument(F.RawFieldType));
                    const bool bRoot = IsObjectRootType(Target);
                    if (!bRoot && !IsOpaqueWrapperType(Db, Target))
                    {
                        return false;
                    }
                    B.Kind = EBind::ClassRef;
                    B.TargetCpp = bRoot ? "Lumina::CObject" : Target;
                    B.CSharp = "global::Lumina.TSubclassOf<"
                        + (bRoot ? std::string("global::LuminaSharp.NativeObject") : GlobalCSharp(Target)) + ">";
                    return true;
                }
                default:
                    return false; // SoftObject, Optional, SubStruct and Map are not in functions yet
            }
        }

        struct FFnBinding
        {
            bool                bVoid = true;
            FArg                Ret;
            std::vector<FArg> Args;
        };

        // A T* immediately followed by an int32 count is one C# Span over the same pair, caller owned.
        bool TryClassifySpanPair(const std::vector<FFieldInfo>& Arguments, size_t Index,
            const FReflectionDatabase& Db, FArg& Out)
        {
            const FFieldInfo& Arg = Arguments[Index];
            if (Arg.RawFieldType.find('*') == std::string::npos || Index + 1 >= Arguments.size())
            {
                return false;
            }

            const std::string NextBare = StripQualifiers(Arguments[Index + 1].RawFieldType);
            if (NextBare != "int32" && NextBare != "int")
            {
                return false;
            }

            std::string ElemCS;
            std::string ElemCpp;
            std::string ElemBufferCpp;
            if (!VectorElementCSharp(Db, StripQualifiers(Arg.RawFieldType), ElemCS, ElemCpp, ElemBufferCpp)
                || !ElemBufferCpp.empty())
            {
                return false;
            }

            Out = FArg();
            Out.Kind = EBind::Span;
            Out.SpanElemCpp = ElemCpp;
            Out.bReadOnlySpan = Arg.RawFieldType.find("const") != std::string::npos;
            Out.CSharp = (Out.bReadOnlySpan ? std::string("global::System.ReadOnlySpan<")
                                            : std::string("global::System.Span<")) + ElemCS + ">";
            Out.Name = Arg.Name;
            return true;
        }

        // Binds when no arg was dropped (LRT1005) and every arg and the return is a kind C# can carry.
        bool ClassifySignature(const FReflectedFunction& Fn, const FReflectionDatabase& Db, FFnBinding& Out)
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
                FArg A;
                if (TryClassifySpanPair(Fn.Arguments, i, Db, A))
                {
                    Out.Args.push_back(A);
                    ++i; // consume the count parameter
                    continue;
                }

                const FFieldInfo& Arg = Fn.Arguments[i];
                if (!ClassifyField(Arg, Db, true, A))
                {
                    return false;
                }
                A.Name = Arg.Name;
                A.DefaultCSharp = DefaultToCSharp(A, Arg.DefaultValue);
                Out.Args.push_back(A);
            }
            ResolveDefaults(Out.Args);
            return true;
        }

        // Bound only when the name is unique in the type, so no two C# methods can collide.
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
            if (NameCount != 1 || !ClassifySignature(Fn, Db, Out))
            {
                return false;
            }

            // The thunk returns the out container's element count, leaving room for one and no real return.
            int OutContainers = 0;
            for (const FArg& A : Out.Args)
            {
                OutContainers += A.bVectorOut ? 1 : 0;
            }
            if (OutContainers > 1 || (OutContainers == 1 && !Out.bVoid))
            {
                return false;
            }
            return true;
        }
        
        void EmitCSharpFunction(FCodeWriter& Writer, const FReflectedFunction& Fn, const FFnBinding& FB,
            const std::string& CallThunk, const std::string& Module, bool bSuppressGCTransition, bool bStatic)
        {
            const std::string& Name = Fn.Name;

            // An out container leaves the signature and comes back as the return, an array of its elements.
            std::string SigParams;
            std::string VectorOutCS;
            for (size_t i = 0; i < FB.Args.size(); ++i)
            {
                if (FB.Args[i].bVectorOut)
                {
                    VectorOutCS = FB.Args[i].CSharp;
                    continue;
                }
                if (!SigParams.empty()) { SigParams += ", "; }
                SigParams += FB.Args[i].CSharp + " " + ArgName(FB.Args[i], i);
                if (!FB.Args[i].DefaultCSharp.empty()) { SigParams += " = " + FB.Args[i].DefaultCSharp; }
            }

            const std::string RetCS = !VectorOutCS.empty() ? (VectorOutCS + "[]")
                                    : FB.bVoid ? std::string("void") : FB.Ret.CSharp;

            EmitDocSummary(Writer, Fn.Metadata);
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
            // A getter that takes nothing reads as a property, so World.Physics stands for World.GetPhysics().
            const std::string* PropertyName = FindMetadata(Fn.Metadata, "ScriptProperty");
            const bool bProperty = PropertyName != nullptr && SigParams.empty() && !FB.bVoid && VectorOutCS.empty();
            Writer.Linef("%s %spartial %s %s(%s);", (bProperty || FunctionHasMetadata(Fn, "ScriptInternal")) ? "internal" : "public",
                bStatic ? "static " : "", RetCS.c_str(), Name.c_str(), SigParams.c_str());

            if (bProperty)
            {
                std::string Property = *PropertyName;
                if (Property.empty() || Property == "true")
                {
                    Property = Name.rfind("Get", 0) == 0 && Name.size() > 3 ? Name.substr(3) : Name;
                }
                EmitDocSummary(Writer, Fn.Metadata);
                Writer.Linef("public %s%s %s => %s();", bStatic ? "static " : "", RetCS.c_str(), Property.c_str(), Name.c_str());
            }
        }

        // A C# Span arrives as (T* pinned, int Length); a TVector param is rebuilt from that pair for the call.
        void EmitSpanArg(const FArg& A, const std::string& An, std::string& Params, std::string& CallArgs)
        {
            const std::string Ptr = A.bReadOnlySpan ? ("const " + A.SpanElemCpp + "* ") : (A.SpanElemCpp + "* ");
            Params += Ptr + An + ", int " + An + "Count";
            CallArgs += A.bVectorArg
                ? ("Lumina::TVector<" + A.SpanElemCpp + ">(" + An + ", " + An + " + " + An + "Count)")
                : (An + ", " + An + "Count");
        }

        // The native `extern "C"` thunk, taking Self first when SelfType names the instance the callee runs on.
        void EmitNativeFunction(FCodeWriter& Writer, const FFnBinding& FB, const std::string& CallThunk,
            const std::string& Callee, const char* SelfType, const char* Api)
        {

            std::string Params;
            std::string CallArgs;
            std::string VectorOutElem;
            std::string VectorOutBuffer;
            for (size_t i = 0; i < FB.Args.size(); ++i)
            {
                const FArg& A = FB.Args[i];
                const std::string An = ArgIndexName('A', i);
                if (!CallArgs.empty())
                {
                    CallArgs += ", ";
                }

                // The callee fills a local the thunk owns, then copies it into the caller's buffer.
                if (A.bVectorOut)
                {
                    VectorOutElem = A.VectorElemCpp;
                    VectorOutBuffer = A.VectorElemBufferCpp;
                    CallArgs += "__vec";
                    continue;
                }

                if (!Params.empty())
                {
                    Params += ", ";
                }

                if (A.bEntity)
                {
                    Params += "uint32 " + An; CallArgs += "static_cast<Lumina::ECS::FEntity>(" + An + ")";
                }
                else if (A.Kind == EBind::Str)
                {
                    // (const char* utf8, int len) -> a temporary FString or soft reference bound to the (const ref) param.
                    Params += "const char* " + An + ", int " + An + "Len";
                    const std::string Ctor = A.bSoftPath ? A.TargetCpp : std::string("Lumina::FString");
                    CallArgs += "((" + An + "Len > 0) ? " + Ctor + "(Lumina::FStringView(" + An + ", (size_t)" + An + "Len)) : " + Ctor + "())";
                }
                else if (A.Kind == EBind::Span)
                {
                    EmitSpanArg(A, An, Params, CallArgs);
                }
                else
                {
                    switch (A.Kind)
                    {
                    case EBind::Number:      Params += A.Cpp + " " + An;            CallArgs += An;                            break;
                    case EBind::Pointer:     Params += "void* " + An;               CallArgs += An;                            break;
                    case EBind::Bool:        Params += "unsigned char " + An;       CallArgs += "(" + An + " != 0)";           break;
                    case EBind::Enum:        Params += "int " + An;                 CallArgs += "(" + A.TargetCpp + ")" + An;  break;
                    case EBind::StructValue: Params += A.TargetCpp + " " + An;      CallArgs += An;                            break;
                    case EBind::Object:      Params += "void* " + An;               CallArgs += "static_cast<" + A.TargetCpp + "*>(" + An + ")"; break;
                    // The wrapper hands over the address it views, and a null one stands in as a default value.
                    case EBind::StructOpaque:
                        Params += "void* " + An;
                        CallArgs += "(" + An + " ? *static_cast<" + A.TargetCpp + "*>(" + An + ") : "
                            + A.TargetCpp + "{})";
                        break;
                    case EBind::ClassRef:
                        Params += "void* " + An;
                        CallArgs += "Lumina::TSubclassOf<" + A.TargetCpp + ">(static_cast<Lumina::CClass*>(" + An + "))";
                        break;
                    default: break;
                    }
                }
            }

            const std::string CallExpr = Callee + "(" + CallArgs + ")";
            std::string RetCpp = "void";
            std::string Body = CallExpr + ";";
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
                    case EBind::Pointer:     RetCpp = "void*";          Body = "return const_cast<void*>(static_cast<const void*>(" + CallExpr + "));"; break;
                    case EBind::Bool:        RetCpp = "unsigned char";  Body = "return " + CallExpr + " ? 1 : 0;";  break;
                    case EBind::Enum:        RetCpp = "int";            Body = "return (int)" + CallExpr + ";";      break;
                    case EBind::StructValue:
                        RetCpp = "Lumina::Scripting::TScriptReturn<" + FB.Ret.TargetCpp + ">::Type";
                        Body = "return Lumina::Scripting::PackScriptReturn(" + CallExpr + ");";
                        break;
                    case EBind::Object:
                        RetCpp = "void*";
                        Body = FB.Ret.bObjectPtrValue
                            ? ("return (void*)(" + CallExpr + ").Get();")
                            : ("return (void*)(" + CallExpr + ");");
                        break;
                    case EBind::ClassRef:
                        RetCpp = "void*";
                        Body = "return (void*)(" + CallExpr + ").Get();";
                        break;
                    case EBind::Str:
                        // Always returns the full byte length, so C# can size an exact buffer on its first call.
                        bStringReturn = true;
                        RetCpp = "int";
                        Body = "return Lumina::Scripting::ReturnStringToCaller(" + CallExpr + ", Buffer, Capacity);";
                        break;
                    default: break;
                    }
                }
            }

            if (!VectorOutElem.empty())
            {
                const bool bObjectElem = !VectorOutBuffer.empty();
                const std::string Read = bObjectElem ? "__vec[(size_t)__i].Get()" : "__vec[(size_t)__i]";
                RetCpp = "int";
                const std::string BufferElem = bObjectElem ? VectorOutBuffer : VectorOutElem;
                const std::string Stash = bObjectElem
                    ? ("Lumina::TVector<" + BufferElem + "> __o; __o.resize((size_t)__n); "
                       + "for (int __i = 0; __i < __n; ++__i) { __o[(size_t)__i] = " + Read + "; } "
                       + "Lumina::Scripting::StashCallOverflow(Buffer, __o.data(), (size_t)__n * sizeof(" + BufferElem + "));")
                    : ("Lumina::Scripting::StashCallOverflow(Buffer, __vec.data(), (size_t)__n * sizeof(" + BufferElem + "));");
                Body = "Lumina::TVector<" + VectorOutElem + "> __vec; " + CallExpr + "; "
                     + "const int __n = (int)__vec.size(); "
                     + "if (Buffer && Capacity > 0) { const int __c = __n < Capacity ? __n : Capacity; "
                     + (bObjectElem
                         ? "for (int __i = 0; __i < __c; ++__i) { Buffer[__i] = " + Read + "; } } "
                         : "Lumina::Memory::Memcpy(Buffer, __vec.data(), (size_t)__c * sizeof(" + BufferElem + ")); } ")
                     + "if (Buffer && __n > Capacity) { " + Stash + " } "
                     + "return __n;";
                Params += (Params.empty() ? std::string() : std::string(", "))
                     + (bObjectElem ? VectorOutBuffer : VectorOutElem) + "* Buffer, int Capacity";
            }
            else if (bStringReturn)
            {
                Params += Params.empty() ? "char* Buffer, int Capacity" : ", char* Buffer, int Capacity";
            }

            if (SelfType == nullptr)
            {
                Writer.Linef("extern \"C\" %s %s %s(%s) { %s }",
                    Api, RetCpp.c_str(), CallThunk.c_str(), Params.c_str(), Body.c_str());
                return;
            }

            const std::string ParamSig = Params.empty() ? std::string() : (", " + Params);
            Writer.Linef("extern \"C\" %s %s %s(%s* Self%s) { %s }",
                Api, RetCpp.c_str(), CallThunk.c_str(), SelfType, ParamSig.c_str(), Body.c_str());
        }
        
        bool FunctionHasMetadata(const FReflectedFunction& Fn, const char* Key)
        {
            return FindMetadata(Fn.Metadata, Key) != nullptr;
        }

        bool IsScriptEvent(const FReflectedFunction& Fn, const FReflectedStruct& Type); // defined below

        void EmitFunctions(FCodeWriter& Writer, const FReflectedStruct& Type, const FReflectionDatabase& Db, std::set<std::string>* Emitted = nullptr)
        {
            const std::string Friendly = Names::FriendlyFromQualified(Type.QualifiedName);
            const std::string Module = ModuleOf(Type);
            const bool bStructFastCalls = Type.HasMetadata("ScriptFastCalls");
            for (const auto& Fn : Type.Functions)
            {
                if (IsScriptEvent(*Fn, Type)) { continue; } // ScriptEvents are emitted specially in EmitForClass
                if (Emitted != nullptr && !Emitted->insert(Fn->Name).second)
                {
                    continue;
                }
                FFnBinding FB;
                if (ClassifyFunction(*Fn, Type, Db, FB))
                {
                    // Opt in per-function or struct-wide (ScriptFastCalls); NoSuppressGCTransition opts back out.
                    const bool bSuppressGC =
                        (bStructFastCalls || FunctionHasMetadata(*Fn, "SuppressGCTransition"))
                        && !FunctionHasMetadata(*Fn, "NoSuppressGCTransition");
                    EmitCSharpFunction(Writer, *Fn, FB, "LuminaSharp_Call_" + Friendly + "_" + Fn->Name, Module, bSuppressGC, Fn->bIsStatic);
                }
            }
        }

        void EmitNativeFunctions(FCodeWriter& Writer, const FReflectedStruct& Type, const char* Qualified, const char* Api, const FReflectionDatabase& Db)
        {
            const std::string Friendly = Names::FriendlyFromQualified(Type.QualifiedName);
            for (const auto& Fn : Type.Functions)
            {
                if (IsScriptEvent(*Fn, Type)) { continue; } // ScriptEvent base/dispatch thunks come from EmitScriptableNative
                FFnBinding FB;
                if (ClassifyFunction(*Fn, Type, Db, FB))
                {
                    // A static has no instance, so it is called on the type and its thunk takes no Self.
                    const std::string Callee = Fn->bIsStatic ? (std::string(Qualified) + "::" + Fn->Name) : ("Self->" + Fn->Name);
                    EmitNativeFunction(Writer, FB, "LuminaSharp_Call_" + Friendly + "_" + Fn->Name, Callee, Fn->bIsStatic ? nullptr : Qualified, Api);
                }
            }
        }

        // A ScriptEvent is a native virtual a C# subclass may override, marshalled like a forward thunk reversed.

        // Gated on virtual because the shim emits an override, and on Scriptable because only those get a shim.
        bool IsScriptEvent(const FReflectedFunction& Fn, const FReflectedStruct& Type)
        {
            return Fn.bIsVirtual && Type.HasMetadata("Scriptable");
        }

        // Strings are still deferred, since their return needs the two-pass protocol the dispatcher lacks.
        bool ScriptEventArgSupported(const FArg& A)
        {
            if (A.bEntity) { return true; }
            switch (A.Kind)
            {
            case EBind::Number: case EBind::Bool: case EBind::Enum: case EBind::StructValue: case EBind::StructOpaque: case EBind::Object: return true;
            default: return false;
            }
        }

        // Also has to be virtual, enforced at C++ compile time since libclang's isVirtual is unreliable here.
        bool ClassifyScriptEvent(const FReflectedFunction& Fn, const FReflectedStruct& Type, const FReflectionDatabase& Db, FFnBinding& Out)
        {
            if (!ClassifyFunction(Fn, Type, Db, Out)) { return false; }
            if (!Out.bVoid && !ScriptEventArgSupported(Out.Ret)) { return false; }
            for (const FArg& A : Out.Args) { if (!ScriptEventArgSupported(A)) { return false; } }
            return true;
        }

        struct FScriptEvent { const FReflectedFunction* Fn; FFnBinding FB; int Index; };

        // The bit index counts EVERY ScriptEvent in declaration order, so shim and C# tags align by construction.
        void CollectScriptEvents(const FReflectedStruct& Type, const FReflectionDatabase& Db, std::vector<FScriptEvent>& Out)
        {
            int Index = 0;
            for (const auto& Fn : Type.Functions)
            {
                if (!IsScriptEvent(*Fn, Type)) { continue; }
                FFnBinding FB;
                if (ClassifyScriptEvent(*Fn, Type, Db, FB)) { Out.push_back({ Fn.get(), std::move(FB), Index }); }
                ++Index;
            }
        }

        struct FInheritedScriptEvent { const FReflectedStruct* Declaring; FScriptEvent Event; };

        const FReflectedStruct* FindScriptableParent(const FReflectedStruct& Type, const FReflectionDatabase& Db)
        {
            const FReflectedStruct* Current = &Type;
            while (!Current->Parent.empty())
            {
                const std::string& Parent = Current->Parent;
                const bool bQualified = Parent.find("::") != std::string::npos;
                const std::string Qualified = (bQualified || Current->Namespace.empty()) ? Parent : (Current->Namespace + "::" + Parent);
                const FReflectedType* Found = Db.GetReflectedType<FReflectedType>(FStringHash(Qualified));
                if (Found == nullptr || Found->Type != FReflectedType::EType::Class || Found == Current)
                {
                    return nullptr;
                }
                Current = static_cast<const FReflectedStruct*>(Found);
                if (Current->HasMetadata("Scriptable"))
                {
                    return Current;
                }
            }
            return nullptr;
        }

        // Ancestors first, the same order the runtime chains each class's registered thunks, so a slot means one event everywhere.
        void CollectInheritedScriptEvents(const FReflectedStruct& Type, const FReflectionDatabase& Db, std::vector<FInheritedScriptEvent>& Out)
        {
            if (const FReflectedStruct* Parent = FindScriptableParent(Type, Db))
            {
                CollectInheritedScriptEvents(*Parent, Db, Out);
            }
            std::vector<FScriptEvent> Own;
            CollectScriptEvents(Type, Db, Own);
            for (FScriptEvent& E : Own)
            {
                Out.push_back({ &Type, std::move(E) });
            }
        }

        //~ C# reverse-dispatcher ABI mapping (native ABI <-> C# call value).
        std::string SeArgAbiCS(const FArg& A)
        {
            if (A.bEntity) { return "uint"; }
            switch (A.Kind) { case EBind::Bool: return "byte"; case EBind::Enum: return "int"; case EBind::Object: case EBind::StructOpaque: return "global::System.IntPtr"; default: return A.CSharp; }
        }
        std::string SeArgFromAbiCS(const FArg& A, const std::string& N)
        {
            if (A.bEntity) { return "new global::LuminaSharp.FEntity(" + N + ")"; }
            switch (A.Kind)
            {
            case EBind::Bool:   return "(" + N + " != 0)";
            case EBind::Enum:   return "(" + A.CSharp + ")" + N;
            // A CObject goes through the per-object wrapper cache, but an opaque struct has no managed-instance slot.
            case EBind::Object: return A.bCObject
                ? "global::LuminaSharp.NativeObjectMarshal.FromHandle<" + A.CSharp + ">(" + N + ")"
                : "global::LuminaSharp.NativeStructs.FromAddressOrNull<" + StripNullable(A.CSharp) + ">(" + N + ")";
            // Passed by reference, so the handle views the caller's value for the length of the call.
            case EBind::StructOpaque: return "global::LuminaSharp.NativeStructs.FromAddress<" + A.CSharp + ">(" + N + ")";
            default:            return N;
            }
        }
        std::string SeRetAbiCS(const FFnBinding& FB)
        {
            if (FB.bVoid) { return "void"; }
            if (FB.Ret.bEntity) { return "uint"; }
            switch (FB.Ret.Kind) { case EBind::Bool: return "byte"; case EBind::Enum: return "int"; case EBind::Object: return "global::System.IntPtr"; default: return FB.Ret.CSharp; }
        }
        std::string SeRetToAbiCS(const FFnBinding& FB, const std::string& Expr)
        {
            if (FB.Ret.bEntity) { return "(" + Expr + ").Id"; }
            switch (FB.Ret.Kind)
            {
            case EBind::Bool:   return "(byte)((" + Expr + ") ? 1 : 0)";
            case EBind::Enum:   return "(int)(" + Expr + ")";
            case EBind::Object: return FB.Ret.bCObject
                ? "global::LuminaSharp.NativeObjectMarshal.ToHandle(" + Expr + ")"
                : "global::LuminaSharp.NativeStructs.AddressOf(" + Expr + ")";
            default:            return Expr;
            }
        }

        //~ Native shim ABI mapping (real C++ type <-> reverse-thunk ABI type).
        std::string SeArgCppParam(const FArg& A)
        {
            if (A.bEntity) { return "Lumina::ECS::FEntity"; }
            switch (A.Kind) { case EBind::Bool: return "bool"; case EBind::Enum: return A.TargetCpp; case EBind::StructValue: return A.TargetCpp; case EBind::StructOpaque: return "const " + A.TargetCpp + "&"; case EBind::Object: return A.TargetCpp + "*"; default: return A.Cpp; }
        }
        std::string SeArgAbiCpp(const FArg& A)
        {
            if (A.bEntity) { return "uint32"; }
            switch (A.Kind) { case EBind::Bool: return "unsigned char"; case EBind::Enum: return "int"; case EBind::StructValue: return A.TargetCpp; case EBind::Object: case EBind::StructOpaque: return "void*"; default: return A.Cpp; }
        }
        std::string SeArgCppToAbi(const FArg& A, const std::string& N)
        {
            if (A.bEntity) { return "(uint32)" + N; }
            switch (A.Kind) { case EBind::Bool: return "(unsigned char)(" + N + " ? 1 : 0)"; case EBind::Enum: return "(int)" + N; case EBind::Object: return "(void*)" + N; case EBind::StructOpaque: return "(void*)&" + N; default: return N; }
        }
        // A parms field for an object is a TStrongObjectPtr, where the shim's own parameter is already a raw pointer.
        std::string SeParmsField(const FArg& A, const std::string& N)
        {
            return A.Kind == EBind::Object ? N + ".Get()" : N;
        }
        std::string SeRetCpp(const FFnBinding& FB)
        {
            if (FB.bVoid) { return "void"; }
            if (FB.Ret.bEntity) { return "Lumina::ECS::FEntity"; }
            switch (FB.Ret.Kind) { case EBind::Bool: return "bool"; case EBind::Enum: return FB.Ret.TargetCpp; case EBind::StructValue: return FB.Ret.TargetCpp; case EBind::Object: return FB.Ret.TargetCpp + "*"; default: return FB.Ret.Cpp; }
        }
        std::string SeRetAbiCpp(const FFnBinding& FB)
        {
            if (FB.bVoid) { return "void"; }
            if (FB.Ret.bEntity) { return "uint32"; }
            switch (FB.Ret.Kind) { case EBind::Bool: return "unsigned char"; case EBind::Enum: return "int"; case EBind::StructValue: return FB.Ret.TargetCpp; case EBind::Object: return "void*"; default: return FB.Ret.Cpp; }
        }
        std::string SeRetAbiToCpp(const FFnBinding& FB, const std::string& Expr)
        {
            if (FB.Ret.bEntity) { return "static_cast<Lumina::ECS::FEntity>(" + Expr + ")"; }
            switch (FB.Ret.Kind) { case EBind::Bool: return "(" + Expr + " != 0)"; case EBind::Enum: return "(" + FB.Ret.TargetCpp + ")(" + Expr + ")"; case EBind::Object: return "static_cast<" + FB.Ret.TargetCpp + "*>(" + Expr + ")"; default: return Expr; }
        }

        // Emits a private base partial running the C++ default, the overridable virtual, and the dispatcher.
        void EmitScriptEventCSharp(FCodeWriter& Writer, const FScriptEvent& E, const std::string& Friendly,
            const std::string& Module, const std::string& ClassName)
        {
            const std::string& Name = E.Fn->Name;
            const FFnBinding& FB = E.FB;
            const std::string BaseThunk = "LuminaSharp_Base_" + Friendly + "_" + Name;
            const std::string Dispatch  = "__ScriptEvent_" + Friendly + "_" + Name;

            std::string SigParams, ArgNames;
            for (size_t i = 0; i < FB.Args.size(); ++i)
            {
                if (i) { SigParams += ", "; ArgNames += ", "; }
                SigParams += FB.Args[i].CSharp + " " + ArgName(FB.Args[i], i);
                ArgNames  += ArgName(FB.Args[i], i);
            }
            const std::string RetCS = FB.bVoid ? std::string("void") : FB.Ret.CSharp;

            Writer.Linef("[global::LuminaSharp.NativeCall(Module = \"%s\", EntryPoint = \"%s\")]", Module.c_str(), BaseThunk.c_str());
            Writer.Linef("private partial %s __base_%s(%s);", RetCS.c_str(), Name.c_str(), SigParams.c_str());

            EmitDocSummary(Writer, E.Fn->Metadata);
            Writer.Linef("[global::LuminaSharp.ScriptEvent(%d)]", E.Index);
            Writer.Linef("public virtual %s %s(%s)", RetCS.c_str(), Name.c_str(), SigParams.c_str());
            Writer.BeginBlock();
            Writer.Linef("%s__base_%s(%s);", FB.bVoid ? "" : "return ", Name.c_str(), ArgNames.c_str());
            Writer.EndBlock();

            // Hand-written engine C# implements this in a partial of the class to run its own work ahead of the script's.
            Writer.Linef("partial void __BeforeScriptEvent_%s(%s);", Name.c_str(), SigParams.c_str());

            std::string AbiParams, CallArgs, ConvertArgs;
            for (size_t i = 0; i < FB.Args.size(); ++i)
            {
                const std::string Local = "__a" + std::to_string(i);
                AbiParams += ", " + SeArgAbiCS(FB.Args[i]) + " " + ArgName(FB.Args[i], i);
                ConvertArgs += "var " + Local + " = " + SeArgFromAbiCS(FB.Args[i], ArgName(FB.Args[i], i)) + "; ";
                if (i) { CallArgs += ", "; }
                CallArgs += Local;
            }
            Writer.Line("[global::LuminaSharp.ManagedExport]");
            Writer.Line("[global::System.Runtime.InteropServices.UnmanagedCallersOnly(CallConvs = new[] { typeof(global::System.Runtime.CompilerServices.CallConvStdcall) })]");
            // Internal only so the export table can reach it, and script assemblies see LuminaSharp's internals, so completion has to be told to skip it.
            Writer.Line("[global::System.ComponentModel.EditorBrowsable(global::System.ComponentModel.EditorBrowsableState.Never)]");
            Writer.Linef("internal static %s %s(global::System.IntPtr __handle%s)", SeRetAbiCS(FB).c_str(), Dispatch.c_str(), AbiParams.c_str());
            Writer.BeginBlock();
            // Declared outside the try so the catch can name the script that threw.
            Writer.Line("object __target = null;");
            Writer.Line("try");
            Writer.BeginBlock();
            Writer.Linef("if (global::System.Runtime.InteropServices.GCHandle.FromIntPtr(__handle).Target is %s __o)", ClassName.c_str());
            Writer.BeginBlock();
            Writer.Line("__target = __o;");
            Writer.Line("using var __scope = global::LuminaSharp.Engine.EnterScriptEvent(__o);");
            if (!ConvertArgs.empty())
            {
                Writer.Line(ConvertArgs.c_str());
            }
            Writer.Linef("__o.__BeforeScriptEvent_%s(%s);", Name.c_str(), CallArgs.c_str());
            if (FB.bVoid)
            {
                Writer.Linef("__o.%s(%s);", Name.c_str(), CallArgs.c_str());
            }
            else
            {
                // Stored first so a conversion that names the expression more than once cannot call twice.
                Writer.Linef("var __r = __o.%s(%s);", Name.c_str(), CallArgs.c_str());
                Writer.Linef("return %s;", SeRetToAbiCS(FB, "__r").c_str());
            }
            Writer.EndBlock();
            Writer.EndBlock();
            Writer.Linef("catch (global::System.Exception __ex) { global::LuminaSharp.NativeBindings.ScriptEventException(__target, \"%s\", __ex); }", Name.c_str());
            if (!FB.bVoid) { Writer.Line("return default;"); }
            Writer.EndBlock();
        }

        // Per-event base thunks, a forwarding shim subclass, and a static registration minting the CClass.
        void EmitScriptableNative(FCodeWriter& Writer, const FReflectedStruct& Type, const FReflectionDatabase& Db, const char* Qualified, const char* Api)
        {
            const std::string Friendly = Names::FriendlyFromQualified(Type.QualifiedName);
            const std::string Shim = Friendly + "__Script";

            std::vector<FScriptEvent> Events;
            CollectScriptEvents(Type, Db, Events);
            std::vector<FInheritedScriptEvent> AllEvents;
            CollectInheritedScriptEvents(Type, Db, AllEvents);

            // Base-call thunks (qualified, non-virtual) backing each event's C# default body.
            for (const FScriptEvent& E : Events)
            {
                const FFnBinding& FB = E.FB;
                std::string Params, CallArgs;
                for (size_t i = 0; i < FB.Args.size(); ++i)
                {
                    if (i) { Params += ", "; CallArgs += ", "; }
                    Params += SeArgAbiCpp(FB.Args[i]) + " " + ArgIndexName('A', i);
                    // ABI param -> the C++ base call argument (inverse of SeArgCppToAbi).
                    const FArg& A = FB.Args[i];
                    const std::string An = ArgIndexName('A', i);
                    if (A.bEntity)                   { CallArgs += "static_cast<Lumina::ECS::FEntity>(" + An + ")"; }
                    else if (A.Kind == EBind::Bool)  { CallArgs += "(" + An + " != 0)"; }
                    else if (A.Kind == EBind::Enum)  { CallArgs += "(" + A.TargetCpp + ")" + An; }
                    else if (A.Kind == EBind::Object){ CallArgs += "static_cast<" + A.TargetCpp + "*>(" + An + ")"; }
                    else if (A.Kind == EBind::StructOpaque) { CallArgs += "*static_cast<const " + A.TargetCpp + "*>(" + An + ")"; }
                    else                             { CallArgs += An; }
                }
                const std::string Call = std::string("Self->") + Qualified + "::" + E.Fn->Name + "(" + CallArgs + ")";
                const std::string ParamSig = Params.empty() ? std::string() : (", " + Params);
                if (FB.bVoid)
                {
                    Writer.Linef("extern \"C\" %s void LuminaSharp_Base_%s_%s(%s* Self%s) { %s; }",
                        Api, Friendly.c_str(), E.Fn->Name.c_str(), Qualified, ParamSig.c_str(), Call.c_str());
                }
                else
                {
                    // Return the ABI form, the inverse of SeRetAbiToCpp, so bool becomes 0/1 and enum becomes int.
                    std::string RetExpr;
                    if (FB.Ret.bEntity)                    { RetExpr = "return (uint32)(" + Call + ");"; }
                    else if (FB.Ret.Kind == EBind::Bool)   { RetExpr = "return (" + Call + ") ? 1 : 0;"; }
                    else if (FB.Ret.Kind == EBind::Enum)   { RetExpr = "return (int)(" + Call + ");"; }
                    else if (FB.Ret.Kind == EBind::Object) { RetExpr = "return (void*)(" + Call + ");"; }
                    else                                   { RetExpr = "return " + Call + ";"; }
                    Writer.Linef("extern \"C\" %s %s LuminaSharp_Base_%s_%s(%s* Self%s) { %s }",
                        Api, SeRetAbiCpp(FB).c_str(), Friendly.c_str(), E.Fn->Name.c_str(), Qualified, ParamSig.c_str(), RetExpr.c_str());
                }
            }

            // The forwarding shim and its registration, in an anonymous namespace so both stay TU-local.
            Writer.Line("namespace");
            Writer.Line("{");

            // The single crossing per event, so the shim and the reflection adapter share one marshalling.
            for (const FInheritedScriptEvent& Inherited : AllEvents)
            {
                const FScriptEvent& E = Inherited.Event;
                const FFnBinding& FB = E.FB;
                const std::string& Name = E.Fn->Name;
                const std::string DeclFriendly = Names::FriendlyFromQualified(Inherited.Declaring->QualifiedName);

                std::string AbiParams, AbiTypes, AbiArgs;
                for (size_t i = 0; i < FB.Args.size(); ++i)
                {
                    const std::string An = ArgIndexName('A', i);
                    AbiParams += ", " + SeArgAbiCpp(FB.Args[i]) + " " + An;
                    AbiTypes  += ", " + SeArgAbiCpp(FB.Args[i]);
                    AbiArgs   += ", " + An;
                }

                Writer.Linef("    static %s __ScriptCall_%s_%s(void* __h%s)",
                    SeRetAbiCpp(FB).c_str(), DeclFriendly.c_str(), Name.c_str(), AbiParams.c_str());
                Writer.Line("    {");
                Writer.Linef("        typedef %s (*FThunk)(void*%s);", SeRetAbiCpp(FB).c_str(), AbiTypes.c_str());
                // Zero-initialized to skip the thread-safe-static guard, and retried so an early miss is not cached.
                Writer.Line("        static FThunk __t = nullptr;");
                Writer.Linef("        if (__t == nullptr) { __t = (FThunk)Lumina::DotNet::ResolveManagedExport(\"__ScriptEvent_%s_%s\"); }",
                    DeclFriendly.c_str(), Name.c_str());
                if (FB.bVoid)
                {
                    Writer.Line("        if (__t == nullptr) { return; }");
                    Writer.Linef("        __t(__h%s);", AbiArgs.c_str());
                }
                else
                {
                    Writer.Line("        if (__t == nullptr) { return {}; }");
                    Writer.Linef("        return __t(__h%s);", AbiArgs.c_str());
                }
                Writer.Line("    }");
            }
            Writer.Line();

            Writer.Linef("    class %s final : public %s", Shim.c_str(), Qualified);
            Writer.Line("    {");
            // The managed instance lives in the object's slot and the override bits on the CClass, so this is bare.
            Writer.Line("    public:");
            for (size_t Slot = 0; Slot < AllEvents.size(); ++Slot)
            {
                const FScriptEvent& E = AllEvents[Slot].Event;
                const FFnBinding& FB = E.FB;
                const std::string& Name = E.Fn->Name;
                const std::string DeclFriendly = Names::FriendlyFromQualified(AllEvents[Slot].Declaring->QualifiedName);
                std::string CppParams, BaseArgs, AbiArgs;
                for (size_t i = 0; i < FB.Args.size(); ++i)
                {
                    const std::string An = ArgIndexName('A', i);
                    if (i) { CppParams += ", "; BaseArgs += ", "; }
                    CppParams += SeArgCppParam(FB.Args[i]) + " " + An;
                    BaseArgs  += An;
                    AbiArgs   += ", " + SeArgCppToAbi(FB.Args[i], An);
                }
                const std::string BaseCall = std::string(Qualified) + "::" + Name + "(" + BaseArgs + ")";
                const std::string Call = "__ScriptCall_" + DeclFriendly + "_" + Name + "(__h" + AbiArgs + ")";

                Writer.Linef("        virtual %s %s(%s) override", SeRetCpp(FB).c_str(), Name.c_str(), CppParams.c_str());
                Writer.Line("        {");
                // A null covers both an unoverridden event and a declared one whose managed side is absent.
                Writer.Linef("            if (void* __h = Lumina::Scriptable::GetOverrideInstance(this, %d))", (int)Slot);
                Writer.Line("            {");
                if (FB.bVoid)
                {
                    Writer.Linef("                %s;", Call.c_str());
                    Writer.Line("                return;");
                }
                else
                {
                    Writer.Linef("                return %s;", SeRetAbiToCpp(FB, Call).c_str());
                }
                Writer.Line("            }");
                Writer.Linef("            %s%s;", FB.bVoid ? "" : "return ", BaseCall.c_str());
                Writer.Line("        }");
            }
            Writer.Line("    };");

            // One per event, for a caller arriving through the reflection system rather than the shim.
            for (const FScriptEvent& E : Events)
            {
                const FFnBinding& FB = E.FB;
                const std::string& Name = E.Fn->Name;
                const std::string Parms = Names::FunctionParmsStruct(Type.DisplayName, Name);
                const std::string QualifiedParms = Type.Namespace.empty() ? Parms : (Type.Namespace + "::" + Parms);

                std::string AbiArgs;
                for (size_t i = 0; i < FB.Args.size(); ++i)
                {
                    AbiArgs += ", " + SeArgCppToAbi(FB.Args[i],
                        SeParmsField(FB.Args[i], "__p." + E.Fn->Arguments[i].Name));
                }
                const std::string Call = "__ScriptCall_" + Friendly + "_" + Name + "(__h" + AbiArgs + ")";

                Writer.Linef("    static void __ScriptDispatch_%s_%s(const Lumina::FFunction&, void* Context, void* Frame)",
                    Friendly.c_str(), Name.c_str());
                Writer.Line("    {");
                if (FB.Args.empty() && FB.bVoid)
                {
                    Writer.Line("        (void)Frame;");
                }
                else
                {
                    Writer.Linef("        %s& __p = *(%s*)Frame;", QualifiedParms.c_str(), QualifiedParms.c_str());
                }
                Writer.Line("        void* __h = Lumina::Scriptable::GetOrCreateInstance((Lumina::CObject*)Context);");
                Writer.Line("        if (__h == nullptr) { return; }");
                if (FB.bVoid)
                {
                    Writer.Linef("        %s;", Call.c_str());
                }
                else
                {
                    const bool bObjectReturn = E.Fn->ReturnIndex >= 0
                        && (size_t)E.Fn->ReturnIndex < E.Fn->ParameterObjectCastTypes.size()
                        && !E.Fn->ParameterObjectCastTypes[E.Fn->ReturnIndex].empty();
                    if (bObjectReturn)
                    {
                        Writer.Linef("        __p.ReturnValue = (Lumina::CObject*)%s;", Call.c_str());
                    }
                    else
                    {
                        Writer.Linef("        __p.ReturnValue = %s;", SeRetAbiToCpp(FB, Call).c_str());
                    }
                }
                Writer.Line("    }");
            }
            Writer.Line();

            Writer.Linef("    struct __ScriptableReg_%s", Friendly.c_str());
            Writer.Line("    {");
            Writer.Linef("        __ScriptableReg_%s()", Friendly.c_str());
            Writer.Line("        {");
            Writer.Line("            Lumina::FScriptableNativeInfo __I;");
            Writer.Linef("            __I.GetBaseClass = []() -> Lumina::CClass* { return %s::StaticClass(); };", Qualified);
            Writer.Linef("            __I.Factory = [](void* __m) -> Lumina::CObject* { return Lumina::Memory::ConstructAt(static_cast<%s*>(__m)); };", Shim.c_str());
            Writer.Linef("            __I.ShimSize = (uint32)sizeof(%s);", Shim.c_str());
            Writer.Linef("            __I.ShimAlign = (uint32)alignof(%s);", Shim.c_str());
            for (const FScriptEvent& E : Events)
            {
                Writer.Linef("            __I.EventThunks.push_back({ \"%s\", &__ScriptDispatch_%s_%s });",
                    E.Fn->Name.c_str(), Friendly.c_str(), E.Fn->Name.c_str());
            }
            Writer.Linef("            Lumina::FScriptableRegistry::RegisterNative(\"%s\", __I);", Type.DisplayName.c_str());
            Writer.Line("        }");
            Writer.Line("    };");
            Writer.Linef("    static __ScriptableReg_%s __scriptableReg_%s;", Friendly.c_str(), Friendly.c_str());
            Writer.Line("}");
        }

        // ---- SCRIPT_EXPORT free functions (no owning type) ----

        // An empty target defaults to namespace Lumina and class ScriptExports.
        void SplitTarget(const std::string& Target, std::string& OutNs, std::string& OutClass)
        {
            const std::string T = Target.empty() ? std::string("Lumina.ScriptExports") : Target;
            const size_t Dot = T.find_last_of('.');
            if (Dot == std::string::npos)
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

        // Made unique across the module via the qualified name.
        std::string FreeThunkName(const FReflectedFunction& Fn)
        {
            return "LuminaSharp_Export_" + Names::FriendlyFromQualified(Fn.QualifiedName);
        }



    }

    namespace
    {
        // The reflected ancestors of a struct, nearest first, stopping at the first one C# does not mirror.
        std::vector<const FReflectedStruct*> ReflectedAncestors(const FReflectedStruct& Type, const FReflectionDatabase& Db)
        {
            std::vector<const FReflectedStruct*> Chain;
            const FReflectedStruct* Current = &Type;
            while (!Current->Parent.empty() && Chain.size() < 32)
            {
                const std::string& Parent = Current->Parent;
                const bool bQualified = Parent.find("::") != std::string::npos;
                const std::string Qualified = (bQualified || Current->Namespace.empty()) ? Parent : (Current->Namespace + "::" + Parent);
                const FReflectedType* Found = Db.GetReflectedType<FReflectedType>(FStringHash(Qualified));
                if (Found == nullptr || Found->Type != FReflectedType::EType::Structure || Found == Current || IsExcludedFromCSharp(*Found))
                {
                    break;
                }
                Current = static_cast<const FReflectedStruct*>(Found);
                Chain.push_back(Current);
            }
            return Chain;
        }

        // A handle is one NativeRef, so every member reads at a constant offset from the address it views.
        void EmitHandleStruct(FCodeWriter& Writer, const FReflectedStruct& Struct, const FReflectionDatabase& Database)
        {
            const std::vector<const FReflectedStruct*> Ancestors = ReflectedAncestors(Struct, Database);

            bool bComponent = Struct.HasMetadata("Component");
            for (const FReflectedStruct* Ancestor : Ancestors)
            {
                bComponent |= Ancestor->HasMetadata("Component");
            }

            const char* Name = Struct.DisplayName.c_str();
            EmitDocSummary(Writer, Struct.Metadata);
            Writer.Linef("[global::LuminaSharp.NativeType(\"%s\")]", Name);
            // An event view also carries the marker the world's event queues are read and sent through.
            Writer.Linef("public readonly unsafe partial struct %s : global::LuminaSharp.%s<%s>%s", Name,
                bComponent ? "IComponent" : "INativeStruct", Name, Struct.HasMetadata("Event") ? ", global::LuminaSharp.IEvent" : "");
            Writer.BeginBlock();
            if (bComponent)
            {
                // C# only reaches a component in place through a ref, so its own address is the storage every member reads.
                Writer.Linef("public static string NativeName => \"%s\";", Name);
                Writer.Line("private System.IntPtr Self => (System.IntPtr)global::System.Runtime.CompilerServices.Unsafe.AsPointer(ref global::System.Runtime.CompilerServices.Unsafe.AsRef(in this));");
                Writer.Line("internal System.IntPtr Handle => Self;");
                Writer.Line("private global::LuminaSharp.NativeRef SelfRef => new global::LuminaSharp.NativeRef(Self);");
            }
            else
            {
                Writer.Line("private readonly global::LuminaSharp.NativeRef __Ref;");
                Writer.Linef("public %s(global::LuminaSharp.NativeRef Ref) { __Ref = Ref; }", Name);
                Writer.Linef("public %s(System.IntPtr Address) { __Ref = new global::LuminaSharp.NativeRef(Address); }", Name);
                Writer.Line("public global::LuminaSharp.NativeRef Ref => __Ref;");
                Writer.Linef("public static string NativeName => \"%s\";", Name);
                Writer.Line("public bool IsValid => __Ref.IsValid;");
                Writer.Line("internal System.IntPtr Handle => __Ref.Address;");
                Writer.Line("private System.IntPtr Self => __Ref.Address;");
                Writer.Line("private global::LuminaSharp.NativeRef SelfRef => __Ref;");
            }
            // Copies the value through the struct's own ops, so a heap member is duplicated rather than aliased.
            auto DeclaresCopyFrom = [](const FReflectedStruct& Owner)
            {
                auto Named = [](const auto& Member) { return Member->Name == "CopyFrom"; };
                return std::any_of(Owner.Props.begin(), Owner.Props.end(), Named)
                    || std::any_of(Owner.Functions.begin(), Owner.Functions.end(), Named);
            };
            bool bDeclaresCopyFrom = DeclaresCopyFrom(Struct);
            for (const FReflectedStruct* Ancestor : Ancestors)
            {
                bDeclaresCopyFrom |= DeclaresCopyFrom(*Ancestor);
            }
            if (!bDeclaresCopyFrom)
            {
                Writer.Linef("public void CopyFrom(%s%s Source) => global::LuminaSharp.Native.StructAssign(Handle, Source.Handle, NativeName);",
                    bComponent ? "in " : "", Name);
            }
            for (const FReflectedStruct* Ancestor : Ancestors)
            {
                // A value mirror ancestor is a copy rather than a view, so a handle cannot convert to it.
                int Size = 0;
                int Align = 0;
                if (Ancestor->HasMetadata("CSharpValueMirror") || IsBlittableValueStruct(*Ancestor, Database, Size, Align))
                {
                    continue;
                }
                const std::string AncestorCSharp = GlobalCSharp(Ancestor->QualifiedName);
                if (bComponent)
                {
                    // A conversion would take the component by value, so the base is a view of the same storage instead.
                    Writer.Linef("public %s As%s => new %s(SelfRef);", AncestorCSharp.c_str(), Ancestor->DisplayName.c_str(), AncestorCSharp.c_str());
                    continue;
                }
                Writer.Linef("public static implicit operator %s(%s Value) => new %s(Value.__Ref);",
                    AncestorCSharp.c_str(), Name, AncestorCSharp.c_str());
            }

            std::set<std::string> EmittedProperties;
            std::set<std::string> EmittedFunctions;
            EmitProperties(Writer, Struct, Database, &EmittedProperties);
            EmitFunctions(Writer, Struct, Database, &EmittedFunctions);
            for (const FReflectedStruct* Ancestor : Ancestors)
            {
                EmitProperties(Writer, *Ancestor, Database, &EmittedProperties);
                EmitFunctions(Writer, *Ancestor, Database, &EmittedFunctions);
            }
            Writer.EndBlock();
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

        const bool bBitMask = Enum.HasMetadata("BitMask");
        EmitDocSummary(Writer, Enum.Metadata);
        if (bBitMask)
        {
            Writer.Line("[global::System.Flags]");
        }
        Writer.Linef("[global::LuminaSharp.NativeType(\"%s\")]", Enum.DisplayName.c_str());
        Writer.Linef("public enum %s : %s", Enum.DisplayName.c_str(), CSharpEnumBackingType(Enum));
        Writer.BeginBlock();
        for (const FReflectedEnum::FConstant& Constant : Enum.Constants)
        {
            EmitDocSummary(Writer, Constant.Description);
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

        const std::string Module = (Header->Project != nullptr) ? Header->Project->Name : std::string("Runtime");

        // One static partial-class block per function; C# merges partials targeting the same class.
        bool bEmittedAny = false;
        for (const auto& Fn : It->second)
        {
            FFnBinding FB;
            if (!ClassifySignature(*Fn, Database, FB))
            {
                continue;
            }

            std::string Ns;
            std::string Cls;
            SplitTarget(Fn->CSharpTarget, Ns, Cls);

            if (!Ns.empty())
            {
                Writer.Linef("namespace %s", Ns.c_str());
                Writer.BeginBlock();
            }
            Writer.Linef("public static unsafe partial class %s", Cls.c_str());
            Writer.BeginBlock();
            EmitCSharpFunction(Writer, *Fn, FB, FreeThunkName(*Fn), Module, FunctionHasMetadata(*Fn, "SuppressGCTransition"), true);
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

        // Thunks are resolved by name at runtime, so they need the always-dllexport macro, not the module one.
        const char* Api = "LUMINA_SCRIPT_API";
        for (const auto& Fn : It->second)
        {
            FFnBinding FB;
            if (ClassifySignature(*Fn, Database, FB))
            {
                EmitNativeFunction(Writer, FB, FreeThunkName(*Fn), Fn->QualifiedName, nullptr, Api);
            }
        }
    }

    bool CSharpBindingEmitter::EmitForStruct(FCodeWriter& Writer, const FReflectedStruct& Struct, const FReflectionDatabase& Database)
    {
        if (IsExcludedFromCSharp(Struct))
        {
            return false;
        }

        // Most reflected structs hold strings or smart pointers and would corrupt if mirrored by value.
        int Size = 0;
        int Align = 0;
        const bool bBlittable = IsBlittableValueStruct(Struct, Database, Size, Align);

        bool bNs = false;
        OpenNamespace(Writer, Struct.Namespace, bNs);

        if (bBlittable)
        {
            EmitDocSummary(Writer, Struct.Metadata);
            Writer.Linef("[global::LuminaSharp.NativeType(\"%s\")]", Struct.DisplayName.c_str());
            Writer.Line("[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]");
            Writer.Linef("public partial struct %s%s", Struct.DisplayName.c_str(), Struct.HasMetadata("Event") ? " : global::LuminaSharp.IEvent" : "");
            Writer.BeginBlock();
            for (const auto& Prop : Struct.Props)
            {
                if (Prop->bInner)
                {
                    continue;
                }
                EmitDocSummary(Writer, Prop->Metadata);
                Writer.Linef("public %s %s;", CSharpFieldType(*Prop).c_str(), SafeIdentifier(Prop->Name).c_str());
            }
            // The NativeCall generator pins this for an instance call, so a method runs on the caller's copy.
            EmitFunctions(Writer, Struct, Database);
            Writer.EndBlock();
        }
        else
        {
            EmitHandleStruct(Writer, Struct, Database);
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

        // Derives its reflected base's wrapper, inheriting its members, and adds its own bound properties.
        const std::string Base = CSharpBase(Class, Database, "global::LuminaSharp.NativeObject");
        const bool bScriptable = Class.HasMetadata("Scriptable");
        EmitDocSummary(Writer, Class.Metadata);
        Writer.Linef("[global::LuminaSharp.NativeType(\"%s\")]", Class.DisplayName.c_str());
        Writer.Linef("public unsafe partial class %s : %s", Class.DisplayName.c_str(), Base.c_str());
        Writer.BeginBlock();
        Writer.Linef("public %s(System.IntPtr handle) : base(handle) { }", Class.DisplayName.c_str());
        // The subclass is Activator-created first, then bound, chaining to the base parameterless ctor.
        Writer.Linef("protected %s() : base() { }", Class.DisplayName.c_str());
        // Hides the base's, so the name resolves to the class it is written on rather than an inherited one.
        const bool bHidesBaseStaticClass = Base != "global::LuminaSharp.NativeObject";
        Writer.Linef("public %sstatic global::Lumina.TSubclassOf<%s> StaticClass()"
            " => global::LuminaSharp.NativeClass<%s>.Of;",
            bHidesBaseStaticClass ? "new " : "",
            GlobalCSharp(Class.QualifiedName).c_str(), GlobalCSharp(Class.QualifiedName).c_str());
        if (bScriptable)
        {
            const std::string Module = ModuleOf(Class);
            const std::string Friendly = Names::FriendlyFromQualified(Class.QualifiedName);
            std::vector<FScriptEvent> Events;
            CollectScriptEvents(Class, Database, Events);
            for (const FScriptEvent& E : Events)
            {
                EmitScriptEventCSharp(Writer, E, Friendly, Module, Class.DisplayName);
            }
        }
        EmitProperties(Writer, Class, Database, nullptr, true);
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
            Struct.EmittedCppQualifiedName().c_str(), Size, Struct.DisplayName.c_str());
    }

    void CSharpBindingEmitter::EmitNativeThunks(FCodeWriter& Writer, const FReflectedStruct& Type, const FReflectionDatabase& Database)
    {
        if (IsExcludedFromCSharp(Type))
        {
            return;
        }
        const std::string Friendly = Names::FriendlyFromQualified(Type.QualifiedName);
        const char* Qualified = Type.EmittedCppQualifiedName().c_str();
        // Thunks are resolved by name at runtime, so they need the always-dllexport macro, not the module one.
        const char* ApiMacro = "LUMINA_SCRIPT_API";

        // A blittable value mirror exposes its fields directly, so only its functions need thunks.
        int Size = 0;
        int Align = 0;
        if (Type.Type == FReflectedType::EType::Structure && IsBlittableValueStruct(Type, Database, Size, Align))
        {
            EmitNativeFunctions(Writer, Type, Qualified, ApiMacro, Database);
            return;
        }

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

        // A Scriptable class emits the forwarding shim, its base thunks, and the CClass-minting registration.
        if (Type.Type == FReflectedType::EType::Class && Type.HasMetadata("Scriptable"))
        {
            EmitScriptableNative(Writer, Type, Database, Qualified, ApiMacro);
        }
    }
}
