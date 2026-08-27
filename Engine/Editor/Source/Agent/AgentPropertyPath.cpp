#include "EditorPCH.h"
#include "Agent/AgentPropertyPath.h"

#include "Agent/AgentReflectionUtils.h"
#include "Containers/StringFormat.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"

namespace Lumina::Agent
{
    namespace
    {
        struct FSegment
        {
            FString Name;

            // Negative when the segment names a field rather than an element.
            int32 Index = -1;
        };

        bool ParseIndex(FStringView Text, int32& OutIndex)
        {
            if (Text.empty() || Text.size() > 9)
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
            size_t Cursor = 0;

            while (Cursor < Path.size())
            {
                if (Path[Cursor] == '[')
                {
                    const size_t Close = Path.find(']', Cursor);
                    if (Close == FStringView::npos)
                    {
                        OutError = "The path has an unclosed bracket.";
                        return false;
                    }

                    FSegment Segment;
                    if (!ParseIndex(Path.substr(Cursor + 1, Close - Cursor - 1), Segment.Index))
                    {
                        OutError = "An index in the path is not a number.";
                        return false;
                    }

                    Out.push_back(Move(Segment));
                    Cursor = Close + 1;

                    if (Cursor < Path.size() && Path[Cursor] == '.')
                    {
                        ++Cursor;
                    }

                    continue;
                }

                size_t End = Cursor;
                while (End < Path.size() && Path[End] != '.' && Path[End] != '[')
                {
                    ++End;
                }

                if (End == Cursor)
                {
                    OutError = "The path has an empty step.";
                    return false;
                }

                FSegment Segment;
                Segment.Name = FString(Path.data() + Cursor, End - Cursor);
                Out.push_back(Move(Segment));

                Cursor = End;
                if (Cursor < Path.size() && Path[Cursor] == '.')
                {
                    ++Cursor;
                }
            }

            if (Out.empty())
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
                if (FProperty* Found = Current->GetProperty(FName(Name.c_str())))
                {
                    return Found;
                }
            }

            return nullptr;
        }
    }

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

        for (size_t Step = 0; Step < Segments.size(); ++Step)
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
            }
            else
            {
                if (Property == nullptr || Property->GetType() != EPropertyTypeFlags::Vector)
                {
                    OutError = "An index can only follow an array field.";
                    return false;
                }

                FArrayProperty* Array = static_cast<FArrayProperty*>(Property);

                const size_t Count = Array->GetNum(ValuePtr);
                if (static_cast<size_t>(Segment.Index) >= Count)
                {
                    OutError = Lumina::Format("Index {} is past the end of an array holding {}.",
                        Segment.Index, Count);
                    return false;
                }

                ValuePtr = Array->GetAt(ValuePtr, static_cast<size_t>(Segment.Index));
                Property = Array->GetInternalProperty();

                if (Property == nullptr)
                {
                    OutError = "That array has no element type.";
                    return false;
                }
            }

            // Only a struct can be stepped into, so anything else has to be the end of the path.
            const bool bMore = Step + 1 < Segments.size();
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
            else
            {
                OutError = Lumina::Format("'{}' holds a value, so nothing can follow it in the path.",
                    Segment.Name);
                return false;
            }
        }

        Out.Property = Property;
        Out.ValuePtr = ValuePtr;

        return Out.IsValid();
    }
}
