#include "ImGuiXNumeric.h"
#include "imgui_internal.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>
#include <vector>

namespace Lumina::ImGuiX
{
    namespace
    {
        struct FExpressionEdit
        {
            ImGuiContext* Context = nullptr;
            ImGuiID Id = 0;
            char Buffer[256] = {};
            bool FocusNext = false;
        };

        std::vector<FExpressionEdit> ActiveEdits;

        FExpressionEdit* FindEdit(ImGuiID Id)
        {
            for (FExpressionEdit& Edit : ActiveEdits)
            {
                if (Edit.Context == GImGui && Edit.Id == Id)
                {
                    return &Edit;
                }
            }
            return nullptr;
        }

        void BeginEdit(ImGuiID Id, ImGuiDataType DataType, const void* Data, bool FocusNext)
        {
            FExpressionEdit Edit;
            Edit.Context = GImGui;
            Edit.Id = Id;
            Edit.FocusNext = FocusNext;
            const ImGuiDataTypeInfo* Info = ImGui::DataTypeGetInfo(DataType);
            ImGui::DataTypeFormatString(Edit.Buffer, IM_ARRAYSIZE(Edit.Buffer), DataType, Data, Info->PrintFmt);
            ActiveEdits.push_back(Edit);
        }

        void EndEdit(ImGuiID Id)
        {
            for (auto It = ActiveEdits.begin(); It != ActiveEdits.end(); ++It)
            {
                if (It->Context == GImGui && It->Id == Id)
                {
                    ActiveEdits.erase(It);
                    return;
                }
            }
        }

        class FExpressionParser
        {
        public:
            explicit FExpressionParser(const char* Text) : At(Text) {}

            bool Parse(double& Result)
            {
                if (!Sum(Result))
                {
                    return false;
                }
                SkipSpaces();
                return *At == '\0' && std::isfinite(Result);
            }

        private:
            void SkipSpaces()
            {
                while (*At == ' ' || *At == '\t')
                {
                    ++At;
                }
            }

            bool Sum(double& Result)
            {
                if (!Product(Result))
                {
                    return false;
                }
                while (true)
                {
                    SkipSpaces();
                    const char Operator = *At;
                    if (Operator != '+' && Operator != '-')
                    {
                        return true;
                    }
                    ++At;
                    double Right;
                    if (!Product(Right))
                    {
                        return false;
                    }
                    Result = Operator == '+' ? Result + Right : Result - Right;
                    if (!std::isfinite(Result))
                    {
                        return false;
                    }
                }
            }

            bool Product(double& Result)
            {
                if (!Unary(Result))
                {
                    return false;
                }
                while (true)
                {
                    SkipSpaces();
                    const char Operator = *At;
                    if (Operator != '*' && Operator != '/' && Operator != '%')
                    {
                        return true;
                    }
                    ++At;
                    double Right;
                    if (!Unary(Right) || (Right == 0.0 && Operator != '*'))
                    {
                        return false;
                    }
                    Result = Operator == '*' ? Result * Right
                        : Operator == '/' ? Result / Right : std::fmod(Result, Right);
                    if (!std::isfinite(Result))
                    {
                        return false;
                    }
                }
            }

            bool Unary(double& Result)
            {
                SkipSpaces();
                if (++Depth > 64)
                {
                    return false;
                }
                bool Valid;
                if (*At == '+' || *At == '-')
                {
                    const bool Negative = *At++ == '-';
                    Valid = Unary(Result);
                    if (Valid && Negative)
                    {
                        Result = -Result;
                    }
                }
                else if (*At == '(')
                {
                    ++At;
                    Valid = Sum(Result);
                    SkipSpaces();
                    Valid = Valid && *At == ')';
                    if (Valid)
                    {
                        ++At;
                    }
                }
                else
                {
                    char* End = nullptr;
                    Valid = (*At >= '0' && *At <= '9') || *At == '.';
                    if (Valid)
                    {
                        Result = std::strtod(At, &End);
                        Valid = End != At && std::isfinite(Result);
                        At = End;
                    }
                }
                --Depth;
                return Valid;
            }

            const char* At;
            int Depth = 0;
        };

        template<typename T>
        bool StoreResult(double Result, void* Data)
        {
            if constexpr (std::is_integral_v<T> && sizeof(T) == 8)
            {
                if (std::abs(Result) > 9007199254740992.0)
                {
                    return false;
                }
            }
            if (Result < static_cast<double>(std::numeric_limits<T>::lowest())
                || Result > static_cast<double>(std::numeric_limits<T>::max()))
            {
                return false;
            }
            *static_cast<T*>(Data) = static_cast<T>(Result);
            return true;
        }

        bool ApplyExpression(const char* Text, ImGuiDataType DataType, void* Data)
        {
            double Result;
            if (!FExpressionParser(Text).Parse(Result))
            {
                return false;
            }
            switch (DataType)
            {
            case ImGuiDataType_S8: return StoreResult<ImS8>(Result, Data);
            case ImGuiDataType_U8: return StoreResult<ImU8>(Result, Data);
            case ImGuiDataType_S16: return StoreResult<ImS16>(Result, Data);
            case ImGuiDataType_U16: return StoreResult<ImU16>(Result, Data);
            case ImGuiDataType_S32: return StoreResult<ImS32>(Result, Data);
            case ImGuiDataType_U32: return StoreResult<ImU32>(Result, Data);
            case ImGuiDataType_S64: return StoreResult<ImS64>(Result, Data);
            case ImGuiDataType_U64: return StoreResult<ImU64>(Result, Data);
            case ImGuiDataType_Float: return StoreResult<float>(Result, Data);
            case ImGuiDataType_Double: return StoreResult<double>(Result, Data);
            default: return false;
            }
        }

        bool ApplyInput(const char* Text, ImGuiDataType DataType, void* Data, const char* Format)
        {
            const ImGuiDataTypeInfo* Info = ImGui::DataTypeGetInfo(DataType);
            if (Format == nullptr)
            {
                Format = Info->PrintFmt;
            }
            ImGuiDataTypeStorage Previous;
            std::memcpy(&Previous, Data, Info->Size);
            const char* First = Text;
            while (*First == ' ' || *First == '\t')
            {
                ++First;
            }
            bool HasOperator = false;
            for (const char* At = First; *At != '\0'; ++At)
            {
                if (*At == '*' || *At == '/' || *At == '%' || *At == '(' || *At == ')'
                    || ((*At == '+' || *At == '-') && At != First && At[-1] != 'e' && At[-1] != 'E'))
                {
                    HasOperator = true;
                    break;
                }
            }
            if (!HasOperator)
            {
                return ImGui::DataTypeApplyFromText(Text, DataType, Data, Format);
            }
            if (!ApplyExpression(Text, DataType, Data)) return false;
            return std::memcmp(&Previous, Data, Info->Size) != 0;
        }

        bool DrawExpressionText(const char* Label, ImGuiDataType DataType, void* Data, const char* Format,
            const void* Min, const void* Max, ImGuiInputTextFlags Flags)
        {
            const ImGuiID Id = ImGui::GetID(Label);
            FExpressionEdit* Edit = FindEdit(Id);
            char LocalBuffer[256] = {};
            if (Edit == nullptr)
            {
                const ImGuiDataTypeInfo* Info = ImGui::DataTypeGetInfo(DataType);
                ImGui::DataTypeFormatString(LocalBuffer, IM_ARRAYSIZE(LocalBuffer), DataType, Data, Info->PrintFmt);
            }
            if (Edit != nullptr && Edit->FocusNext)
            {
                ImGui::SetKeyboardFocusHere();
                Edit->FocusNext = false;
            }
            ImGui::InputText(Label, Edit != nullptr ? Edit->Buffer : LocalBuffer, 256,
                Flags | ImGuiInputTextFlags_AutoSelectAll);
            if (Edit == nullptr && ImGui::IsItemActivated())
            {
                BeginEdit(Id, DataType, Data, false);
                Edit = FindEdit(Id);
                std::memcpy(Edit->Buffer, LocalBuffer, sizeof(LocalBuffer));
            }

            const bool Commit = Edit != nullptr && ImGui::IsItemDeactivated();
            bool Changed = false;
            if (Commit)
            {
                Changed = ApplyInput(Edit->Buffer, DataType, Data, Format);
                // Either side alone clamps, but a pair with no range is unclamped as ImGui's drag reads it, or every typed value commits as zero.
                const bool bNoRange = Min != nullptr && Max != nullptr && ImGui::DataTypeCompare(DataType, Min, Max) >= 0;
                if (Changed && (Min != nullptr || Max != nullptr) && !bNoRange)
                {
                    ImGui::DataTypeClamp(DataType, Data, Min, Max);
                }
                EndEdit(Id);
            }
            if (!Commit)
            {
                GImGui->LastItemData.StatusFlags &= ~ImGuiItemStatusFlags_Edited;
            }
            else if (Changed)
            {
                ImGui::MarkItemEdited(Id);
            }
            return Changed;
        }
    }

    bool DragScalar(const char* Label, ImGuiDataType DataType, void* Data, float Speed,
        const void* Min, const void* Max, const char* Format, ImGuiSliderFlags Flags)
    {
        const ImGuiID Id = ImGui::GetID(Label);
        if (FindEdit(Id) != nullptr)
        {
            return DrawExpressionText(Label, DataType, Data, Format, Min, Max, 0);
        }

        const bool Changed = ImGui::DragScalar(Label, DataType, Data, Speed, Min, Max, Format,
            Flags | ImGuiSliderFlags_NoInput);
        if (ImGui::IsItemHovered() && (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
            || (ImGui::GetIO().KeyCtrl && ImGui::IsMouseClicked(ImGuiMouseButton_Left))))
        {
            BeginEdit(Id, DataType, Data, true);
            ImGui::ClearActiveID();
        }
        return Changed;
    }

    bool DragScalarN(const char* Label, ImGuiDataType DataType, void* Data, int Components, float Speed,
        const void* Min, const void* Max, const char* Format, ImGuiSliderFlags Flags)
    {
        bool Changed = false;
        ImGui::BeginGroup();
        ImGui::PushID(Label);
        ImGui::PushMultiItemsWidths(Components, ImGui::CalcItemWidth());
        const size_t TypeSize = ImGui::DataTypeGetInfo(DataType)->Size;
        for (int Index = 0; Index < Components; ++Index)
        {
            ImGui::PushID(Index);
            if (Index > 0)
            {
                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            }
            Changed |= DragScalar("", DataType, Data, Speed, Min, Max, Format, Flags);
            ImGui::PopID();
            ImGui::PopItemWidth();
            Data = static_cast<char*>(Data) + TypeSize;
        }
        ImGui::PopID();
        const char* LabelEnd = ImGui::FindRenderedTextEnd(Label);
        if (Label != LabelEnd)
        {
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::TextEx(Label, LabelEnd);
        }
        ImGui::EndGroup();
        return Changed;
    }

    bool InputScalar(const char* Label, ImGuiDataType DataType, void* Data, const void* Step,
        const void* StepFast, const char* Format, ImGuiInputTextFlags Flags)
    {
        if (Step == nullptr)
        {
            return DrawExpressionText(Label, DataType, Data, Format, nullptr, nullptr, Flags);
        }

        bool Changed = false;
        ImGui::BeginGroup();
        ImGui::PushID(Label);
        const float ButtonSize = ImGui::GetFrameHeight();
        const float Spacing = ImGui::GetStyle().ItemInnerSpacing.x;
        ImGui::SetNextItemWidth(ImMax(1.0f, ImGui::CalcItemWidth() - (ButtonSize + Spacing) * 2.0f));
        Changed |= DrawExpressionText("", DataType, Data, Format, nullptr, nullptr, Flags);
        ImGui::SameLine(0.0f, Spacing);
        if (ImGui::Button("-", ImVec2(ButtonSize, ButtonSize)))
        {
            ImGui::DataTypeApplyOp(DataType, '-', Data, Data, Step);
            Changed = true;
        }
        ImGui::SameLine(0.0f, Spacing);
        if (ImGui::Button("+", ImVec2(ButtonSize, ButtonSize)))
        {
            const void* ChosenStep = StepFast != nullptr && ImGui::GetIO().KeyCtrl ? StepFast : Step;
            ImGui::DataTypeApplyOp(DataType, '+', Data, Data, ChosenStep);
            Changed = true;
        }
        ImGui::PopID();
        ImGui::EndGroup();
        return Changed;
    }

    bool InputScalarN(const char* Label, ImGuiDataType DataType, void* Data, int Components,
        const void* Step, const void* StepFast, const char* Format, ImGuiInputTextFlags Flags)
    {
        bool Changed = false;
        ImGui::BeginGroup();
        ImGui::PushID(Label);
        ImGui::PushMultiItemsWidths(Components, ImGui::CalcItemWidth());
        const size_t TypeSize = ImGui::DataTypeGetInfo(DataType)->Size;
        for (int Index = 0; Index < Components; ++Index)
        {
            ImGui::PushID(Index);
            if (Index > 0)
            {
                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            }
            Changed |= InputScalar("", DataType, Data, Step, StepFast, Format, Flags);
            ImGui::PopID();
            ImGui::PopItemWidth();
            Data = static_cast<char*>(Data) + TypeSize;
        }
        ImGui::PopID();
        const char* LabelEnd = ImGui::FindRenderedTextEnd(Label);
        if (Label != LabelEnd)
        {
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::TextEx(Label, LabelEnd);
        }
        ImGui::EndGroup();
        return Changed;
    }

    bool DragFloat(const char* Label, float* Data, float Speed, float Min, float Max,
        const char* Format, ImGuiSliderFlags Flags)
    {
        return DragScalar(Label, ImGuiDataType_Float, Data, Speed, &Min, &Max, Format, Flags);
    }

    bool DragInt(const char* Label, int* Data, float Speed, int Min, int Max,
        const char* Format, ImGuiSliderFlags Flags)
    {
        return DragScalar(Label, ImGuiDataType_S32, Data, Speed, &Min, &Max, Format, Flags);
    }

    bool DragFloat2(const char* Label, float Data[2], float Speed, float Min, float Max,
        const char* Format, ImGuiSliderFlags Flags)
    {
        return DragScalarN(Label, ImGuiDataType_Float, Data, 2, Speed, &Min, &Max, Format, Flags);
    }

    bool DragFloat3(const char* Label, float Data[3], float Speed, float Min, float Max,
        const char* Format, ImGuiSliderFlags Flags)
    {
        return DragScalarN(Label, ImGuiDataType_Float, Data, 3, Speed, &Min, &Max, Format, Flags);
    }

    bool DragFloat4(const char* Label, float Data[4], float Speed, float Min, float Max,
        const char* Format, ImGuiSliderFlags Flags)
    {
        return DragScalarN(Label, ImGuiDataType_Float, Data, 4, Speed, &Min, &Max, Format, Flags);
    }
}
