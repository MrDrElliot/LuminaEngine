using System;
using System.Text;
using Lumina;

namespace LuminaSharp;

// Dear ImGui for scripts, named as in C++, for debug windows built in a World.Debug.OnDebugGui handler. Every call is a no-op where no frame is open, as in Shipping.
public static unsafe partial class ImGui
{
    // Text larger than this goes to the heap rather than the stack.
    private const int StackTextLimit = 1024;

    // True on the game thread while a frame is open, the only time a widget does anything.
    public static bool IsAvailable => ImGui_IsAvailable();

    public static float Framerate => ImGui_GetFramerate();

    // The window ImGui draws into, in pixels, for placing a window relative to the screen.
    public static FVector2 DisplaySize
    {
        get
        {
            float width;
            float height;
            ImGui_GetDisplaySize(&width, &height);
            return new FVector2(width, height);
        }
    }

    //~ Windows

    public static bool Begin(string name, ImGuiWindowFlags flags = ImGuiWindowFlags.None)
    {
        return ImGui_Begin(name, null, (int)flags);
    }

    // Draws a close button that writes false to open, so the caller stops calling Begin for it.
    public static bool Begin(string name, ref bool open, ImGuiWindowFlags flags = ImGuiWindowFlags.None)
    {
        fixed (bool* openPtr = &open)
        {
            return ImGui_Begin(name, (byte*)openPtr, (int)flags);
        }
    }

    // Always paired with Begin, whatever Begin returned.
    public static void End() => ImGui_End();

    public static bool BeginChild(string id, FVector2 size = default, ImGuiChildFlags childFlags = ImGuiChildFlags.None, ImGuiWindowFlags windowFlags = ImGuiWindowFlags.None)
    {
        return ImGui_BeginChild(id, size.X, size.Y, (int)childFlags, (int)windowFlags);
    }

    public static void EndChild() => ImGui_EndChild();

    // In pixels from the main window's top left corner, in a game build and the editor alike.
    public static void SetNextWindowPos(FVector2 position, ImGuiCond cond = ImGuiCond.None, FVector2 pivot = default)
    {
        ImGui_SetNextWindowPos(position.X, position.Y, (int)cond, pivot.X, pivot.Y);
    }

    public static void SetNextWindowSize(FVector2 size, ImGuiCond cond = ImGuiCond.None)
    {
        ImGui_SetNextWindowSize(size.X, size.Y, (int)cond);
    }

    public static void SetNextWindowCollapsed(bool collapsed, ImGuiCond cond = ImGuiCond.None) => ImGui_SetNextWindowCollapsed(collapsed, (int)cond);

    public static void SetNextWindowBgAlpha(float alpha) => ImGui_SetNextWindowBgAlpha(alpha);

    // ImGui's own tour of every widget, the quickest reference for what a layout can do.
    public static void ShowDemoWindow(ref bool open)
    {
        fixed (bool* openPtr = &open)
        {
            ImGui_ShowDemoWindow((byte*)openPtr);
        }
    }

    //~ Layout

    public static void Separator() => ImGui_Separator();

    public static void SeparatorText(string text) => ImGui_SeparatorText(text);

    public static void SameLine(float offsetFromStart = 0.0f, float spacing = -1.0f) => ImGui_SameLine(offsetFromStart, spacing);

    public static void NewLine() => ImGui_NewLine();

    public static void Spacing() => ImGui_Spacing();

    public static void Dummy(FVector2 size) => ImGui_Dummy(size.X, size.Y);

    public static void Indent(float width = 0.0f) => ImGui_Indent(width);

    public static void Unindent(float width = 0.0f) => ImGui_Unindent(width);

    public static void BeginGroup() => ImGui_BeginGroup();

    public static void EndGroup() => ImGui_EndGroup();

    public static void SetNextItemWidth(float width) => ImGui_SetNextItemWidth(width);

    public static void PushItemWidth(float width) => ImGui_PushItemWidth(width);

    public static void PopItemWidth() => ImGui_PopItemWidth();

    public static void AlignTextToFramePadding() => ImGui_AlignTextToFramePadding();

    public static FVector2 GetContentRegionAvail()
    {
        float width;
        float height;
        ImGui_GetContentRegionAvail(&width, &height);
        return new FVector2(width, height);
    }

    //~ Text

    public static void Text(string text) => ImGui_Text(text);

    public static void TextColored(FVector4 color, string text) => ImGui_TextColored(color.X, color.Y, color.Z, color.W, text);

    public static void TextDisabled(string text) => ImGui_TextDisabled(text);

    public static void TextWrapped(string text) => ImGui_TextWrapped(text);

    public static void LabelText(string label, string text) => ImGui_LabelText(label, text);

    public static void BulletText(string text) => ImGui_BulletText(text);

    //~ Widgets

    public static bool Button(string label, FVector2 size = default) => ImGui_Button(label, size.X, size.Y);

    public static bool SmallButton(string label) => ImGui_SmallButton(label);

    public static bool Checkbox(string label, ref bool value)
    {
        fixed (bool* valuePtr = &value)
        {
            return ImGui_Checkbox(label, (byte*)valuePtr);
        }
    }

    public static bool RadioButton(string label, bool active) => ImGui_RadioButton(label, active);

    public static void ProgressBar(float fraction, FVector2 size = default, string overlay = "")
    {
        ImGui_ProgressBar(fraction, size.X, size.Y, overlay);
    }

    public static bool SliderFloat(string label, ref float value, float min, float max, string format = "", ImGuiSliderFlags flags = ImGuiSliderFlags.None)
    {
        fixed (float* valuePtr = &value)
        {
            return ImGui_SliderFloatN(label, valuePtr, 1, min, max, format, (int)flags);
        }
    }

    public static bool SliderFloat2(string label, ref FVector2 value, float min, float max, string format = "", ImGuiSliderFlags flags = ImGuiSliderFlags.None)
    {
        fixed (FVector2* valuePtr = &value)
        {
            return ImGui_SliderFloatN(label, (float*)valuePtr, 2, min, max, format, (int)flags);
        }
    }

    public static bool SliderFloat3(string label, ref FVector3 value, float min, float max, string format = "", ImGuiSliderFlags flags = ImGuiSliderFlags.None)
    {
        fixed (FVector3* valuePtr = &value)
        {
            return ImGui_SliderFloatN(label, (float*)valuePtr, 3, min, max, format, (int)flags);
        }
    }

    public static bool SliderInt(string label, ref int value, int min, int max, string format = "", ImGuiSliderFlags flags = ImGuiSliderFlags.None)
    {
        fixed (int* valuePtr = &value)
        {
            return ImGui_SliderInt(label, valuePtr, min, max, format, (int)flags);
        }
    }

    public static bool DragFloat(string label, ref float value, float speed = 1.0f, float min = 0.0f, float max = 0.0f, string format = "", ImGuiSliderFlags flags = ImGuiSliderFlags.None)
    {
        fixed (float* valuePtr = &value)
        {
            return ImGui_DragFloatN(label, valuePtr, 1, speed, min, max, format, (int)flags);
        }
    }

    public static bool DragFloat2(string label, ref FVector2 value, float speed = 1.0f, float min = 0.0f, float max = 0.0f, string format = "", ImGuiSliderFlags flags = ImGuiSliderFlags.None)
    {
        fixed (FVector2* valuePtr = &value)
        {
            return ImGui_DragFloatN(label, (float*)valuePtr, 2, speed, min, max, format, (int)flags);
        }
    }

    public static bool DragFloat3(string label, ref FVector3 value, float speed = 1.0f, float min = 0.0f, float max = 0.0f, string format = "", ImGuiSliderFlags flags = ImGuiSliderFlags.None)
    {
        fixed (FVector3* valuePtr = &value)
        {
            return ImGui_DragFloatN(label, (float*)valuePtr, 3, speed, min, max, format, (int)flags);
        }
    }

    public static bool DragInt(string label, ref int value, float speed = 1.0f, int min = 0, int max = 0, string format = "", ImGuiSliderFlags flags = ImGuiSliderFlags.None)
    {
        fixed (int* valuePtr = &value)
        {
            return ImGui_DragInt(label, valuePtr, speed, min, max, format, (int)flags);
        }
    }

    public static bool InputFloat(string label, ref float value, float step = 0.0f, float stepFast = 0.0f, string format = "", ImGuiInputTextFlags flags = ImGuiInputTextFlags.None)
    {
        fixed (float* valuePtr = &value)
        {
            return ImGui_InputFloat(label, valuePtr, step, stepFast, format, (int)flags);
        }
    }

    public static bool InputInt(string label, ref int value, int step = 1, int stepFast = 100, ImGuiInputTextFlags flags = ImGuiInputTextFlags.None)
    {
        fixed (int* valuePtr = &value)
        {
            return ImGui_InputInt(label, valuePtr, step, stepFast, (int)flags);
        }
    }

    // Edits value in place, holding up to maxLength characters.
    public static bool InputText(string label, ref string value, int maxLength = 256, ImGuiInputTextFlags flags = ImGuiInputTextFlags.None)
    {
        value ??= string.Empty;
        if (value.Length > maxLength)
        {
            value = value.Substring(0, maxLength);
        }

        // Four bytes covers any UTF-8 character, and one more holds the terminator ImGui writes.
        int capacity = maxLength * 4 + 1;
        Span<byte> buffer = capacity <= StackTextLimit ? stackalloc byte[capacity] : new byte[capacity];
        int written = Encoding.UTF8.GetBytes(value, buffer);
        buffer[written] = 0;

        fixed (byte* bufferPtr = buffer)
        {
            if (!ImGui_InputText(label, bufferPtr, capacity, (int)flags))
            {
                return false;
            }
            int length = buffer.IndexOf((byte)0);
            value = Encoding.UTF8.GetString(bufferPtr, length < 0 ? capacity : length);
            return true;
        }
    }

    public static bool ColorEdit3(string label, ref FVector3 color, ImGuiColorEditFlags flags = ImGuiColorEditFlags.None)
    {
        fixed (FVector3* colorPtr = &color)
        {
            return ImGui_ColorEdit(label, (float*)colorPtr, false, (int)flags);
        }
    }

    public static bool ColorEdit4(string label, ref FVector4 color, ImGuiColorEditFlags flags = ImGuiColorEditFlags.None)
    {
        fixed (FVector4* colorPtr = &color)
        {
            return ImGui_ColorEdit(label, (float*)colorPtr, true, (int)flags);
        }
    }

    public static bool Combo(string label, ref int current, ReadOnlySpan<string> items, int maxVisibleItems = -1)
    {
        int total = 0;
        foreach (string item in items)
        {
            total += Encoding.UTF8.GetByteCount(item ?? string.Empty) + 1;
        }

        Span<byte> separated = total <= StackTextLimit ? stackalloc byte[total] : new byte[total];
        int offset = 0;
        foreach (string item in items)
        {
            offset += Encoding.UTF8.GetBytes(item ?? string.Empty, separated.Slice(offset));
            separated[offset++] = 0;
        }

        fixed (int* currentPtr = &current)
        {
            return ImGui_Combo(label, currentPtr, separated, maxVisibleItems);
        }
    }

    public static bool Selectable(string label, bool selected = false, ImGuiSelectableFlags flags = ImGuiSelectableFlags.None, FVector2 size = default)
    {
        return ImGui_Selectable(label, selected, (int)flags, size.X, size.Y);
    }

    //~ Trees

    // Pair with TreePop when it returns true.
    public static bool TreeNode(string label, ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags.None) => ImGui_TreeNodeEx(label, (int)flags);

    public static void TreePop() => ImGui_TreePop();

    public static bool CollapsingHeader(string label, ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags.None) => ImGui_CollapsingHeader(label, (int)flags);

    public static void SetNextItemOpen(bool open, ImGuiCond cond = ImGuiCond.None) => ImGui_SetNextItemOpen(open, (int)cond);

    //~ Tables

    // Pair with EndTable when it returns true.
    public static bool BeginTable(string id, int columns, ImGuiTableFlags flags = ImGuiTableFlags.None, FVector2 outerSize = default, float innerWidth = 0.0f)
    {
        return ImGui_BeginTable(id, columns, (int)flags, outerSize.X, outerSize.Y, innerWidth);
    }

    public static void EndTable() => ImGui_EndTable();

    public static void TableNextRow(ImGuiTableRowFlags flags = ImGuiTableRowFlags.None, float minHeight = 0.0f) => ImGui_TableNextRow((int)flags, minHeight);

    public static bool TableNextColumn() => ImGui_TableNextColumn();

    public static bool TableSetColumnIndex(int column) => ImGui_TableSetColumnIndex(column);

    public static void TableSetupColumn(string label, ImGuiTableColumnFlags flags = ImGuiTableColumnFlags.None, float widthOrWeight = 0.0f)
    {
        ImGui_TableSetupColumn(label, (int)flags, widthOrWeight);
    }

    public static void TableSetupScrollFreeze(int columns, int rows) => ImGui_TableSetupScrollFreeze(columns, rows);

    public static void TableHeadersRow() => ImGui_TableHeadersRow();

    //~ Tooltips and popups

    public static void SetTooltip(string text) => ImGui_SetTooltip(text);

    public static bool BeginTooltip() => ImGui_BeginTooltip();

    // A tooltip for the last item, shown only while it is hovered.
    public static bool BeginItemTooltip() => ImGui_BeginItemTooltip();

    public static void EndTooltip() => ImGui_EndTooltip();

    public static void OpenPopup(string id, ImGuiPopupFlags flags = ImGuiPopupFlags.None) => ImGui_OpenPopup(id, (int)flags);

    public static bool BeginPopup(string id, ImGuiWindowFlags flags = ImGuiWindowFlags.None) => ImGui_BeginPopup(id, (int)flags);

    public static bool BeginPopupModal(string name, ImGuiWindowFlags flags = ImGuiWindowFlags.None) => ImGui_BeginPopupModal(name, null, (int)flags);

    // An empty id opens on a right click of the last item.
    public static bool BeginPopupContextItem(string id = "", ImGuiPopupFlags flags = ImGuiPopupFlags.MouseButtonRight)
    {
        return ImGui_BeginPopupContextItem(id, (int)flags);
    }

    public static void EndPopup() => ImGui_EndPopup();

    public static void CloseCurrentPopup() => ImGui_CloseCurrentPopup();

    //~ Menus

    public static bool BeginMenuBar() => ImGui_BeginMenuBar();

    public static void EndMenuBar() => ImGui_EndMenuBar();

    public static bool BeginMainMenuBar() => ImGui_BeginMainMenuBar();

    public static void EndMainMenuBar() => ImGui_EndMainMenuBar();

    public static bool BeginMenu(string label, bool enabled = true) => ImGui_BeginMenu(label, enabled);

    public static void EndMenu() => ImGui_EndMenu();

    public static bool MenuItem(string label, string shortcut = "", bool selected = false, bool enabled = true)
    {
        return ImGui_MenuItem(label, shortcut, selected, enabled);
    }

    //~ Plots

    public static void PlotLines(string label, ReadOnlySpan<float> values, int offset = 0, string overlay = "", float scaleMin = float.MaxValue, float scaleMax = float.MaxValue, FVector2 size = default)
    {
        ImGui_PlotLines(label, values, offset, overlay, scaleMin, scaleMax, size.X, size.Y, false);
    }

    public static void PlotHistogram(string label, ReadOnlySpan<float> values, int offset = 0, string overlay = "", float scaleMin = float.MaxValue, float scaleMax = float.MaxValue, FVector2 size = default)
    {
        ImGui_PlotLines(label, values, offset, overlay, scaleMin, scaleMax, size.X, size.Y, true);
    }

    //~ Item and window queries

    public static bool IsItemHovered(ImGuiHoveredFlags flags = ImGuiHoveredFlags.None) => ImGui_IsItemHovered((int)flags);

    public static bool IsItemClicked(ImGuiMouseButton button = ImGuiMouseButton.Left) => ImGui_IsItemClicked((int)button);

    public static bool IsItemActive() => ImGui_IsItemActive();

    public static bool IsWindowHovered(ImGuiHoveredFlags flags = ImGuiHoveredFlags.None) => ImGui_IsWindowHovered((int)flags);

    public static bool IsWindowFocused(ImGuiFocusedFlags flags = ImGuiFocusedFlags.None) => ImGui_IsWindowFocused((int)flags);

    //~ Ids and style

    public static void PushID(string id) => ImGui_PushID(id);

    public static void PushID(int id) => ImGui_PushIDInt(id);

    public static void PopID() => ImGui_PopID();

    public static void PushStyleColor(ImGuiCol index, FVector4 color) => ImGui_PushStyleColor((int)index, color.X, color.Y, color.Z, color.W);

    public static void PopStyleColor(int count = 1) => ImGui_PopStyleColor(count);

    public static void PushStyleVar(ImGuiStyleVar index, float value) => ImGui_PushStyleVarFloat((int)index, value);

    public static void PushStyleVar(ImGuiStyleVar index, FVector2 value) => ImGui_PushStyleVarVec2((int)index, value.X, value.Y);

    public static void PopStyleVar(int count = 1) => ImGui_PopStyleVar(count);

    //~ Native

    [NativeCall] private static partial bool ImGui_IsAvailable();
    [NativeCall] private static partial float ImGui_GetFramerate();
    [NativeCall] private static partial bool ImGui_Begin(string name, byte* open, int flags);
    [NativeCall] private static partial void ImGui_End();
    [NativeCall] private static partial bool ImGui_BeginChild(string id, float width, float height, int childFlags, int windowFlags);
    [NativeCall] private static partial void ImGui_EndChild();
    [NativeCall] private static partial void ImGui_SetNextWindowPos(float x, float y, int cond, float pivotX, float pivotY);
    [NativeCall] private static partial void ImGui_SetNextWindowSize(float width, float height, int cond);
    [NativeCall] private static partial void ImGui_SetNextWindowCollapsed(bool collapsed, int cond);
    [NativeCall] private static partial void ImGui_SetNextWindowBgAlpha(float alpha);
    [NativeCall] private static partial void ImGui_ShowDemoWindow(byte* open);
    [NativeCall] private static partial void ImGui_Separator();
    [NativeCall] private static partial void ImGui_SeparatorText(string text);
    [NativeCall] private static partial void ImGui_SameLine(float offsetFromStart, float spacing);
    [NativeCall] private static partial void ImGui_NewLine();
    [NativeCall] private static partial void ImGui_Spacing();
    [NativeCall] private static partial void ImGui_Dummy(float width, float height);
    [NativeCall] private static partial void ImGui_Indent(float width);
    [NativeCall] private static partial void ImGui_Unindent(float width);
    [NativeCall] private static partial void ImGui_BeginGroup();
    [NativeCall] private static partial void ImGui_EndGroup();
    [NativeCall] private static partial void ImGui_SetNextItemWidth(float width);
    [NativeCall] private static partial void ImGui_PushItemWidth(float width);
    [NativeCall] private static partial void ImGui_PopItemWidth();
    [NativeCall] private static partial void ImGui_AlignTextToFramePadding();
    [NativeCall] private static partial void ImGui_GetContentRegionAvail(float* width, float* height);
    [NativeCall] private static partial void ImGui_GetDisplaySize(float* width, float* height);
    [NativeCall] private static partial void ImGui_Text(string text);
    [NativeCall] private static partial void ImGui_TextColored(float r, float g, float b, float a, string text);
    [NativeCall] private static partial void ImGui_TextDisabled(string text);
    [NativeCall] private static partial void ImGui_TextWrapped(string text);
    [NativeCall] private static partial void ImGui_LabelText(string label, string text);
    [NativeCall] private static partial void ImGui_BulletText(string text);
    [NativeCall] private static partial bool ImGui_Button(string label, float width, float height);
    [NativeCall] private static partial bool ImGui_SmallButton(string label);
    [NativeCall] private static partial bool ImGui_Checkbox(string label, byte* value);
    [NativeCall] private static partial bool ImGui_RadioButton(string label, bool active);
    [NativeCall] private static partial void ImGui_ProgressBar(float fraction, float width, float height, string overlay);
    [NativeCall] private static partial bool ImGui_SliderFloatN(string label, float* values, int components, float min, float max, string format, int flags);
    [NativeCall] private static partial bool ImGui_SliderInt(string label, int* value, int min, int max, string format, int flags);
    [NativeCall] private static partial bool ImGui_DragFloatN(string label, float* values, int components, float speed, float min, float max, string format, int flags);
    [NativeCall] private static partial bool ImGui_DragInt(string label, int* value, float speed, int min, int max, string format, int flags);
    [NativeCall] private static partial bool ImGui_InputFloat(string label, float* value, float step, float stepFast, string format, int flags);
    [NativeCall] private static partial bool ImGui_InputInt(string label, int* value, int step, int stepFast, int flags);
    [NativeCall] private static partial bool ImGui_InputText(string label, byte* buffer, int bufferSize, int flags);
    [NativeCall] private static partial bool ImGui_ColorEdit(string label, float* color, bool withAlpha, int flags);
    [NativeCall] private static partial bool ImGui_Combo(string label, int* current, ReadOnlySpan<byte> items, int maxVisibleItems);
    [NativeCall] private static partial bool ImGui_Selectable(string label, bool selected, int flags, float width, float height);
    [NativeCall] private static partial bool ImGui_TreeNodeEx(string label, int flags);
    [NativeCall] private static partial void ImGui_TreePop();
    [NativeCall] private static partial bool ImGui_CollapsingHeader(string label, int flags);
    [NativeCall] private static partial void ImGui_SetNextItemOpen(bool open, int cond);
    [NativeCall] private static partial bool ImGui_BeginTable(string id, int columns, int flags, float outerWidth, float outerHeight, float innerWidth);
    [NativeCall] private static partial void ImGui_EndTable();
    [NativeCall] private static partial void ImGui_TableNextRow(int rowFlags, float minHeight);
    [NativeCall] private static partial bool ImGui_TableNextColumn();
    [NativeCall] private static partial bool ImGui_TableSetColumnIndex(int column);
    [NativeCall] private static partial void ImGui_TableSetupColumn(string label, int flags, float widthOrWeight);
    [NativeCall] private static partial void ImGui_TableSetupScrollFreeze(int columns, int rows);
    [NativeCall] private static partial void ImGui_TableHeadersRow();
    [NativeCall] private static partial void ImGui_SetTooltip(string text);
    [NativeCall] private static partial bool ImGui_BeginTooltip();
    [NativeCall] private static partial bool ImGui_BeginItemTooltip();
    [NativeCall] private static partial void ImGui_EndTooltip();
    [NativeCall] private static partial void ImGui_OpenPopup(string id, int flags);
    [NativeCall] private static partial bool ImGui_BeginPopup(string id, int flags);
    [NativeCall] private static partial bool ImGui_BeginPopupModal(string name, byte* open, int flags);
    [NativeCall] private static partial bool ImGui_BeginPopupContextItem(string id, int flags);
    [NativeCall] private static partial void ImGui_EndPopup();
    [NativeCall] private static partial void ImGui_CloseCurrentPopup();
    [NativeCall] private static partial bool ImGui_BeginMenuBar();
    [NativeCall] private static partial void ImGui_EndMenuBar();
    [NativeCall] private static partial bool ImGui_BeginMainMenuBar();
    [NativeCall] private static partial void ImGui_EndMainMenuBar();
    [NativeCall] private static partial bool ImGui_BeginMenu(string label, bool enabled);
    [NativeCall] private static partial void ImGui_EndMenu();
    [NativeCall] private static partial bool ImGui_MenuItem(string label, string shortcut, bool selected, bool enabled);
    [NativeCall] private static partial void ImGui_PlotLines(string label, ReadOnlySpan<float> values, int offset, string overlay, float scaleMin, float scaleMax, float width, float height, bool histogram);
    [NativeCall] private static partial bool ImGui_IsItemHovered(int flags);
    [NativeCall] private static partial bool ImGui_IsItemClicked(int button);
    [NativeCall] private static partial bool ImGui_IsItemActive();
    [NativeCall] private static partial bool ImGui_IsWindowHovered(int flags);
    [NativeCall] private static partial bool ImGui_IsWindowFocused(int flags);
    [NativeCall] private static partial void ImGui_PushID(string id);
    [NativeCall] private static partial void ImGui_PushIDInt(int id);
    [NativeCall] private static partial void ImGui_PopID();
    [NativeCall] private static partial void ImGui_PushStyleColor(int index, float r, float g, float b, float a);
    [NativeCall] private static partial void ImGui_PopStyleColor(int count);
    [NativeCall] private static partial void ImGui_PushStyleVarFloat(int index, float value);
    [NativeCall] private static partial void ImGui_PushStyleVarVec2(int index, float x, float y);
    [NativeCall] private static partial void ImGui_PopStyleVar(int count);
}
