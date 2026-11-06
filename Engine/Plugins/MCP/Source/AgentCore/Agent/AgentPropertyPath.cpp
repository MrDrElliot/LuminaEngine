#include "AgentCorePCH.h"
#include "Agent/AgentPropertyPath.h"

#include "Agent/AgentReflectionUtils.h"
#include "Containers/StringFormat.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"

namespace Lumina::Agent
{
    namespace AgentPropertyPathInternal
    {
        struct FSegment
        {
            FString Name;

            // Negative when the segment names a field rather than an element.
            int32 Index = -1;
        };

        bool ParseIndex(FStringView Text, int32& OutIndex)
        {
            if (Text.IsEmpty() || Text.Len() > 9)
            {
                return false;
            }

            int32 Value = 0;
            for (char Character : Text)
            {
                if (Character < '0' || Character > '9')
                {
                    return false;
                }

                Value = Value * 10 + (Character - '0');
            }

            OutIndex = Value;
            return true;
        }

        // Splits "Materials[2].Slot" into Materials, [2], Slot so each step is one lookup.
        bool ParsePath(FStringView Path, TVector<FSegment>& Out, FString& OutError)
        {
            int32 Cursor = 0;

            while (Cursor < Path.Len())
            {
                if (Path[Cursor] == '[')
                {
                    const int32 Close = Path.Find(']', Cursor);
                    if (Close == Constants::kIndexNone)
                    {
                        OutError = "The path has an unclosed bracket.";
                        return false;
                    }

                    FSegment Segment;
                    if (!ParseIndex(Path.Mid(Cursor + 1, Close - Cursor - 1), Segment.Index))
                    {
                        OutError = "An index in the path is not a number.";
                        return false;
                    }

                    Out.Add(Move(Segment));
                    Cursor = Close + 1;

                    if (Cursor < Path.Len() && Path[Cursor] == '.')
                    {
                        ++Cursor;
                    }

                    continue;
                }

                int32 End = Cursor;
                while (End < Path.Len() && Path[End] != '.' && Path[End] != '[')
                {
                    ++End;
                }

                if (End == Cursor)
                {
                    OutError = "The path has an empty step.";
                    return false;
                }

                FSegment Segment;
                Segment.Name = FString(Path.GetData() + Cursor, End - Cursor);
                Out.Add(Move(Segment));

                Cursor = End;
                if (Cursor < Path.Len() && Path[Cursor] == '.')
                {
                    ++Cursor;
                }
            }

            if (Out.IsEmpty())
            {
                OutError = "The path is empty.";
                return false;
            }

            return true;
        }

        FProperty* FindProperty(CStruct* Struct, const FString& Name)
        {
            for (CStruct* Current : Detail::CollectStructChain(Struct))
            {
                if (FProperty* Found = Current->GetProperty(FName(*Name)))
                {
                    return Found;
                }
            }

            return nullptr;
        }
    }

    using namespace AgentPropertyPathInternal;

    bool ResolvePropertyPath(CStruct* Root, void* RootData, FStringView Path, FResolvedProperty& Out, FString& OutError)
    {
        Out = FResolvedProperty();

        if (Root == nullptr || RootData == nullptr)
        {
            OutError = "There is nothing to walk.";
            return false;
        }

        TVector<FSegment> Segments;
        if (!ParsePath(Path, Segments, OutError))
        {
            return false;
        }

        CStruct* Struct = Root;
        void* Data = RootData;

        FProperty* Property = nullptr;
        void* ValuePtr = nullptr;

        // Names the value reached so far, such as "Values[0]", since an index segment has no name of its own.
        FString Label;

        for (int32 Step = 0; Step < Segments.Num(); ++Step)
        {
            const FSegment& Segment = Segments[Step];

            if (Segment.Index < 0)
            {
                if (Struct == nullptr)
                {
                    OutError = Lumina::Format("'{}' does not name a field of anything.", Segment.Name);
                    return false;
                }

                Property = FindProperty(Struct, Segment.Name);
                if (Property == nullptr)
                {
                    OutError = Lumina::Format("'{}' is not a field of {}.", Segment.Name, Struct->GetName());
                    return false;
                }

                ValuePtr = Property->GetValuePtr<uint8>(Data);
                Label    = Segment.Name;
            }
            else
            {
                if (Property == nullptr || Property->GetType() != EPropertyTypeFlags::Vector)
                {
                    OutError = "An index can only follow an array field.";
                    return false;
                }

                FArrayProperty* Array = static_cast<FArrayProperty*>(Property);

                const int32 Count = Array->Num(ValuePtr);
                if (static_cast<uint32>(Segment.Index) >= static_cast<uint32>(Count))
                {
                    OutError = Lumina::Format("Index {} is past the end of an array holding {}.",
                        Segment.Index, Count);
                    return false;
                }

                ValuePtr = Array->GetAt(ValuePtr, Segment.Index);
                Property = Array->GetInternalProperty();
                Label    = Lumina::Format("{}[{}]", Label, Segment.Index);

                if (Property == nullptr)
                {
                    OutError = "That array has no element type.";
                    return false;
                }
            }

            // Only a struct can be stepped into, so anything else has to be the end of the path.
            const bool bMore = Step + 1 < Segments.Num();
            if (!bMore)
            {
                break;
            }

            if (Property->GetType() == EPropertyTypeFlags::Struct)
            {
                Struct = static_cast<FStructProperty*>(Property)->GetStruct();
                Data   = ValuePtr;
            }
            else if (Property->GetType() == EPropertyTypeFlags::Vector)
            {
                Struct = nullptr;
            }
            else if (Property->GetType() == EPropertyTypeFlags::Object)
            {
                // Entity scripts hang off SEntityScriptComponent::Scripts, so a path has to reach into objects.
                CObject* Object = static_cast<TStrongObjectPtr<CObject>*>(ValuePtr)->Get();
                if (Object == nullptr)
                {
                    OutError = Lumina::Format("'{}' is null, so nothing can follow it in the path.", Label);
                    return false;
                }
                Struct = Object->GetClass();
                Data   = Object;
            }
            else
            {
                OutError = Lumina::Format("'{}' holds a value, so nothing can follow it in the path.", Label);
                return false;
            }
        }

        Out.Property = Property;
        Out.ValuePtr = ValuePtr;

        return Out.IsValid();
    }
}
