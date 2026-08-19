// https://github.com/Ka1serM/ImViewGuizmo
//
// ImViewGuizmo Single-Header Library by Marcel Kazemi
//
// Copyright (c) 2025 Marcel Kazemi
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// Lumina changes: GLM swapped for the in-house math types, GLM's -Z-forward camera
// convention swapped for Lumina's +Z, the mat4 pipeline dropped (the projection was an
// identity ortho), pitch clamped at the poles, per-viewport Context, Enable().
#pragma once

#include <array>
#include <algorithm>
#include <cfloat>
#include <cmath>

#include "imgui.h"
#include "Core/Math/Math.h"

namespace ImViewGuizmo
{
    using vec3_t = Lumina::FVector3;
    using quat_t = Lumina::FQuat;

    namespace GizmoMath
    {
        inline vec3_t make_vec3(float x, float y, float z) { return vec3_t(x, y, z); }
        inline quat_t angleAxis(float angle, const vec3_t& axis) { return Lumina::Math::AngleAxis(angle, axis); }
        inline quat_t quatLookAt(const vec3_t& forward, const vec3_t& up) { return Lumina::Math::QuatLookAt(forward, up); }

        inline vec3_t cross(const vec3_t& a, const vec3_t& b) { return Lumina::Math::Cross(a, b); }
        inline float dot(const vec3_t& a, const vec3_t& b) { return Lumina::Math::Dot(a, b); }
        inline float dot(const quat_t& a, const quat_t& b) { return Lumina::Math::Dot(a, b); }
        inline float length(const vec3_t& v) { return Lumina::Math::Length(v); }
        inline float length2(const vec3_t& v) { return Lumina::Math::LengthSquared(v); }
        inline vec3_t normalize(const vec3_t& v) { return Lumina::Math::Normalize(v); }
        inline vec3_t mix(const vec3_t& a, const vec3_t& b, float t) { return Lumina::Math::Mix(a, b, t); }
        inline vec3_t add_vv(const vec3_t& a, const vec3_t& b) { return a + b; }
        inline vec3_t subtract_vv(const vec3_t& a, const vec3_t& b) { return a - b; }
        inline vec3_t multiply_vf(const vec3_t& v, float f) { return v * f; }

        inline quat_t multiply_qq(const quat_t& a, const quat_t& b) { return a * b; }
        inline vec3_t multiply_qv(const quat_t& q, const vec3_t& v) { return q * v; }
    }

    struct Style
    {
        float scale = 1.f;

        // Axis visuals
        float lineLength = 0.5f;
        float lineWidth = 4.0f;
        float circleRadius = 15.0f;
        float fadeFactor = 0.25f;

        // Highlight
        ImU32 highlightColor = IM_COL32(255, 255, 0, 255);
        float highlightWidth = 2.0f;

        // Axis
        ImU32 axisColors[3] = {
            IM_COL32(233, 62, 85, 255),  // X
            IM_COL32(140, 206, 40, 255), // Y
            IM_COL32(49, 155, 249, 255)  // Z
            };

        // Labels
        float labelSize = 1.0f;
        const char* axisLabels[6] = {"X", "-X", "Y", "-Y", "Z", "-Z"};
        ImU32 labelColor = IM_COL32(14, 18, 24, 255);

        //Big Circle
        float bigCircleRadius = 80.0f;
        ImU32 bigCircleColor = IM_COL32(255, 255, 255, 50);

        // Animation
        bool animateSnap = true;
        float snapAnimationDuration = 0.5f; // in seconds

        // Zoom/Pan Button Visuals
        float toolButtonRadius = 25.f;
        float toolButtonInnerPadding = 4.f;

        ImU32 toolButtonColor = IM_COL32(144, 144, 144, 50);
        ImU32 toolButtonHoveredColor = IM_COL32(215, 215, 215, 50);
        ImU32 toolButtonIconColor = IM_COL32(215, 215, 215, 225);
    };

    // Lumina is +Z forward / +Y up.
    inline const vec3_t origin = GizmoMath::make_vec3(0.f, 0.f, 0.f);
    inline const vec3_t worldRight = GizmoMath::make_vec3(1.f, 0.f, 0.f);
    inline const vec3_t worldUp = GizmoMath::make_vec3(0.f, 1.f, 0.f);
    inline const vec3_t worldForward = GizmoMath::make_vec3(0.f, 0.f, 1.f);
    inline const vec3_t axisVectors[3] = { GizmoMath::make_vec3(1,0,0), GizmoMath::make_vec3(0,1,0), GizmoMath::make_vec3(0,0,1) };

    // sin(89deg); past this the look-at up vector degenerates.
    inline constexpr float maxPitchSin = 0.9998f;

    inline Style& GetStyle()
    {
        static Style style;
        return style;
    }

    struct GizmoAxis
    {
        int id; // 0-5 for (+X,-X,+Y,-Y,+Z,-Z), 6=center
        int axisIndex; // 0=X, 1=Y, 2=Z
        float depth; // Screen-space depth
        vec3_t direction; // 3D vector
    };

    enum ActiveTool
    {
        TOOL_NONE,
        TOOL_GIZMO,
        TOOL_DOLLY,
        TOOL_PAN
        };

    struct Context
    {
        int hoveredAxisID = -1;
        bool isZoomButtonHovered = false;
        bool isPanButtonHovered = false;
        bool enabled = true;
        ActiveTool activeTool = TOOL_NONE;

        int lastFrame = -1;

        // Animation state
        bool isAnimating = false;
        float animationStartTime = 0.f;

        vec3_t startPos = origin;
        vec3_t targetPos = origin;
        vec3_t startUp = worldUp;
        vec3_t targetUp = worldUp;

        vec3_t animStartDir = worldForward;
        vec3_t animTargetDir = worldForward;
        float animStartDist = 0.f;
        float animTargetDist = 0.f;

        void Reset()
        {
            hoveredAxisID = -1;
            isZoomButtonHovered = false;
            isPanButtonHovered = false;
        }

        // For a caller that stops drawing the gizmo: leaving a drag or hover latched would keep
        // reporting IsUsing/IsOver forever.
        void Clear()
        {
            Reset();
            activeTool = TOOL_NONE;
            isAnimating = false;
        }
    };

    // Each viewport installs its own Context; the drag and snap-animation state must not be
    // shared or a second viewport drawing mid-drag would steer its own camera.
    inline Context*& CurrentContextRef()
    {
        static Context* Current = nullptr;
        return Current;
    }

    inline Context& GetContext()
    {
        static Context fallback;
        Context* Current = CurrentContextRef();
        return Current ? *Current : fallback;
    }

    inline void SetContext(Context* ctx) { CurrentContextRef() = ctx; }

    inline float GizmoLengthSqr(const ImVec2& v) { return v.x * v.x + v.y * v.y; }
    inline float mix(float a, float b, float t) { return a * (1.0f - t) + b * t; }

    inline void BeginFrame()
    {
        Context& ctx = GetContext();
        const int currentFrame = ImGui::GetFrameCount();
        if (ctx.lastFrame != currentFrame)
        {
            ctx.lastFrame = currentFrame;
            ctx.Reset();
        }
    }

    // Gates hover and drag-start only; an in-flight drag or snap always runs to completion.
    inline void Enable(bool enabled) { GetContext().enabled = enabled; }

    inline bool IsUsing(const Context& ctx) { return ctx.activeTool != TOOL_NONE; }
    inline bool IsUsing() { return IsUsing(GetContext()); }

    inline bool IsAnimating(const Context& ctx) { return ctx.isAnimating; }
    inline bool IsAnimating() { return IsAnimating(GetContext()); }

    inline bool IsOver(const Context& ctx)
    {
        return ctx.hoveredAxisID != -1 || ctx.isZoomButtonHovered || ctx.isPanButtonHovered;
    }
    inline bool IsOver() { return IsOver(GetContext()); }

    // cameraPos/cameraRot are modified in place; position is the gizmo's center in screen space.
    // Returns true when the camera moved.
    inline bool Rotate(vec3_t& cameraPos, quat_t& cameraRot, const vec3_t& pivot, ImVec2 position, float rotationSpeed = 0.01f)
    {
        auto& io = ImGui::GetIO();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        auto& ctx = GetContext();
        auto& style = GetStyle();
        bool wasModified = false;

        if (ctx.isAnimating)
        {
            float elapsedTime = static_cast<float>(ImGui::GetTime()) - ctx.animationStartTime;
            float t = Lumina::Math::Min(1.0f, elapsedTime / style.snapAnimationDuration);
            t = 1.0f - (1.0f - t) * (1.0f - t); // ease-out quad

            vec3_t currentDir = GizmoMath::normalize(GizmoMath::mix(ctx.animStartDir, ctx.animTargetDir, t));
            float currentDistance = mix(ctx.animStartDist, ctx.animTargetDist, t);
            cameraPos = GizmoMath::add_vv(pivot, GizmoMath::multiply_vf(currentDir, currentDistance));

            vec3_t currentUp = GizmoMath::normalize(GizmoMath::mix(ctx.startUp, ctx.targetUp, t));
            // currentDir is pivot->camera; negate for the look direction.
            cameraRot = GizmoMath::quatLookAt(GizmoMath::multiply_vf(currentDir, -1.f), currentUp);

            wasModified = true;

            if (t >= 1.0f)
            {
                cameraPos = ctx.targetPos;
                cameraRot = GizmoMath::quatLookAt(GizmoMath::multiply_vf(ctx.animTargetDir, -1.f), ctx.targetUp);
                ctx.isAnimating = false;
            }
        }

        const float gizmoDiameter = 256.f * style.scale;
        const float halfGizmoSize = gizmoDiameter / 2.f;
        const float scaledCircleRadius = style.circleRadius * style.scale;
        const float scaledBigCircleRadius = style.bigCircleRadius * style.scale;
        const float scaledLineWidth = style.lineWidth * style.scale;
        const float scaledHighlightWidth = style.highlightWidth * style.scale;
        const float scaledHighlightRadius = (style.circleRadius + 2.0f) * style.scale;
        const float scaledFontSize = ImGui::GetFontSize() * style.labelSize;

        // World->view is just the camera basis. camBack is negated because Lumina's camera looks
        // down +Z while the gizmo's depth axis points back at the viewer.
        const vec3_t camRight = GizmoMath::multiply_qv(cameraRot, worldRight);
        const vec3_t camUp = GizmoMath::multiply_qv(cameraRot, worldUp);
        const vec3_t camBack = GizmoMath::multiply_vf(GizmoMath::multiply_qv(cameraRot, worldForward), -1.f);

        std::array<GizmoAxis, 6> axes;
        axes[0] = {0, 0, camBack.x, axisVectors[0]};
        axes[1] = {1, 0, -camBack.x, GizmoMath::multiply_vf(axisVectors[0], -1.0f)};
        axes[2] = {2, 1, camBack.y, axisVectors[1]};
        axes[3] = {3, 1, -camBack.y, GizmoMath::multiply_vf(axisVectors[1], -1.0f)};
        axes[4] = {4, 2, camBack.z, axisVectors[2]};
        axes[5] = {5, 2, -camBack.z, GizmoMath::multiply_vf(axisVectors[2], -1.0f)};

        Lumina::Algo::Sort(axes.begin(), axes.end(), [](const GizmoAxis& a, const GizmoAxis& b)
        {
            return a.depth < b.depth;
        });

        // The upstream projection was an identity ortho, so this is the whole of it.
        auto worldToScreen = [&](const vec3_t& worldPos) -> ImVec2
        {
            const float ndcX = GizmoMath::dot(worldPos, camRight);
            const float ndcY = GizmoMath::dot(worldPos, camUp);
            return {position.x + ndcX * halfGizmoSize, position.y - ndcY * halfGizmoSize};
        };

        const ImVec2 originScreenPos = worldToScreen(origin);

        const bool canInteract = ctx.enabled && !(io.ConfigFlags & ImGuiConfigFlags_NoMouse);
        if (canInteract && ctx.activeTool == TOOL_NONE && !ctx.isAnimating)
        {
            ImVec2 mousePos = io.MousePos;
            float distToCenterSq = GizmoLengthSqr(ImVec2(mousePos.x - position.x, mousePos.y - position.y));

            if (distToCenterSq < (halfGizmoSize + scaledCircleRadius) * (halfGizmoSize + scaledCircleRadius))
            {
                const float minDistanceSq = scaledCircleRadius * scaledCircleRadius;
                for (const auto& axis : axes)
                {
                    if (axis.depth < -0.1f)
                    {
                        continue;
                    }

                    ImVec2 handlePos = worldToScreen(GizmoMath::multiply_vf(axis.direction, style.lineLength));
                    if (GizmoLengthSqr(ImVec2(handlePos.x - mousePos.x, handlePos.y - mousePos.y)) < minDistanceSq)
                    {
                        ctx.hoveredAxisID = axis.id;
                    }
                }
                if (ctx.hoveredAxisID == -1
                    && GizmoLengthSqr(ImVec2(originScreenPos.x - mousePos.x, originScreenPos.y - mousePos.y)) < scaledBigCircleRadius * scaledBigCircleRadius)
                {
                    ctx.hoveredAxisID = 6;
                }
            }
        }

        if (ctx.hoveredAxisID == 6 || ctx.activeTool == TOOL_GIZMO)
        {
            drawList->AddCircleFilled(originScreenPos, scaledBigCircleRadius, style.bigCircleColor);
        }

        ImFont* font = ImGui::GetFont();
        for (const auto& axis : axes)
        {
            // Even ids are the positive axis; only those get a spoke and a permanent label.
            const bool isPrimary = (axis.id % 2) == 0;
            const float colorFactor = mix(style.fadeFactor, 1.0f, (axis.depth + 1.0f) * 0.5f);
            const ImVec4 axisColor = ImGui::ColorConvertU32ToFloat4(style.axisColors[axis.axisIndex]);

            const ImVec2 handlePos = worldToScreen(GizmoMath::multiply_vf(axis.direction, style.lineLength));

            ImVec4 fillColorF = axisColor;
            if (isPrimary)
            {
                fillColorF.w *= colorFactor;
            }
            else
            {
                fillColorF.x *= 0.6f; fillColorF.y *= 0.6f; fillColorF.z *= 0.6f;
                fillColorF.w *= colorFactor * 0.92f;
            }
            const ImU32 fillColor = ImGui::ColorConvertFloat4ToU32(fillColorF);

            if (isPrimary)
            {
                ImVec4 lineColorF = axisColor;
                lineColorF.w *= colorFactor;
                const ImU32 lineColor = ImGui::ColorConvertFloat4ToU32(lineColorF);

                ImVec2 lineDir = {handlePos.x - originScreenPos.x, handlePos.y - originScreenPos.y};
                float lineLengthVal = sqrtf(lineDir.x * lineDir.x + lineDir.y * lineDir.y) + 1e-6f;
                lineDir.x /= lineLengthVal; lineDir.y /= lineLengthVal;
                ImVec2 lineEndPos = {handlePos.x - lineDir.x * scaledCircleRadius, handlePos.y - lineDir.y * scaledCircleRadius};

                drawList->AddLine(originScreenPos, lineEndPos, lineColor, scaledLineWidth);
                drawList->AddCircleFilled(handlePos, scaledCircleRadius, fillColor);
            }
            else
            {
                ImVec4 outlineColorF = axisColor;
                outlineColorF.w *= colorFactor * 0.95f;
                const ImU32 outlineColor = ImGui::ColorConvertFloat4ToU32(outlineColorF);

                drawList->AddCircleFilled(handlePos, scaledCircleRadius, fillColor);
                drawList->AddCircle(handlePos, scaledCircleRadius, outlineColor, 0, scaledLineWidth * 0.5f);
            }

            if (ctx.hoveredAxisID == axis.id)
            {
                drawList->AddCircle(handlePos, scaledHighlightRadius, style.highlightColor, 0, scaledHighlightWidth);
            }

            if (isPrimary || ctx.hoveredAxisID == axis.id)
            {
                float textFactor = Lumina::Math::Max(0.0f, Lumina::Math::Min(1.0f, 1.0f + axis.depth * 2.5f));
                if (textFactor > 0.01f)
                {
                    ImVec4 textColor = ImGui::ColorConvertU32ToFloat4(style.labelColor);
                    textColor.w *= textFactor;
                    const char* label = style.axisLabels[axis.id];
                    ImVec2 textSize = font->CalcTextSizeA(scaledFontSize, FLT_MAX, 0.f, label);
                    drawList->AddText(font, scaledFontSize, {handlePos.x - textSize.x * 0.5f, handlePos.y - textSize.y * 0.5f}, ImGui::ColorConvertFloat4ToU32(textColor), label);
                }
            }
        }

        if (canInteract && ImGui::IsMouseClicked(0) && ctx.activeTool == TOOL_NONE && ctx.hoveredAxisID == 6)
        {
            ctx.activeTool = TOOL_GIZMO;
            ctx.isAnimating = false;
        }

        if (ctx.activeTool == TOOL_GIZMO)
        {
            // Drag signs match the editor's own orbit/look gestures: right yaws the camera left,
            // down lifts it.
            const float yawAngle = -io.MouseDelta.x * rotationSpeed;
            const float pitchAngle = io.MouseDelta.y * rotationSpeed;

            const quat_t yawRotation = GizmoMath::angleAxis(yawAngle, worldUp);
            const vec3_t rightAxis = GizmoMath::multiply_qv(cameraRot, worldRight);
            const quat_t pitchRotation = GizmoMath::angleAxis(pitchAngle, rightAxis);

            const vec3_t relativeCamPos = GizmoMath::subtract_vv(cameraPos, pivot);
            const vec3_t yawed = GizmoMath::multiply_qv(yawRotation, relativeCamPos);
            const vec3_t pitched = GizmoMath::multiply_qv(pitchRotation, yawed);

            // Drop the pitch alone when it would cross the pole; rolling past vertical flips the view.
            const float pitchedDist = GizmoMath::length(pitched);
            const bool overPole = pitchedDist < 1e-5f || std::fabs(pitched.y / pitchedDist) > maxPitchSin;

            const quat_t totalRotation = overPole ? yawRotation : GizmoMath::multiply_qq(yawRotation, pitchRotation);

            cameraPos = GizmoMath::add_vv(pivot, overPole ? yawed : pitched);
            cameraRot = GizmoMath::multiply_qq(totalRotation, cameraRot);

            wasModified = true;
        }

        if (canInteract && ImGui::IsMouseReleased(0) && ctx.hoveredAxisID >= 0 && ctx.hoveredAxisID <= 5 && ctx.activeTool == TOOL_NONE)
        {
            int axisIndex = ctx.hoveredAxisID / 2;
            float sign = (ctx.hoveredAxisID % 2 == 0) ? 1.0f : -1.0f;
            vec3_t targetDir = GizmoMath::multiply_vf(axisVectors[axisIndex], sign);

            float currentDistance = GizmoMath::length(GizmoMath::subtract_vv(cameraPos, pivot));
            vec3_t targetPosition = GizmoMath::add_vv(pivot, GizmoMath::multiply_vf(targetDir, currentDistance));

            vec3_t dirNormalized = GizmoMath::normalize(targetDir);

            vec3_t targetUp = worldUp;
            if (std::fabs(GizmoMath::dot(dirNormalized, targetUp)) > 0.999f)
            {
                if (dirNormalized.y > 0.0f) // pivot->camera points up, so we are looking down
                {
                    targetUp = worldForward;
                }
                else
                {
                    targetUp = GizmoMath::multiply_vf(worldForward, -1.f);
                }
            }

            // targetDir is pivot->camera; negate for the look direction.
            quat_t targetRotation = GizmoMath::quatLookAt(GizmoMath::multiply_vf(targetDir, -1.f), targetUp);

            if (style.animateSnap && style.snapAnimationDuration > 0.0f)
            {
                bool pos_is_different = GizmoMath::length2(GizmoMath::subtract_vv(cameraPos, targetPosition)) > 0.0001f;
                bool rot_is_different = (1.0f - std::fabs(GizmoMath::dot(cameraRot, targetRotation))) > 0.0001f;

                if (pos_is_different || rot_is_different)
                {
                    ctx.isAnimating = true;
                    ctx.animationStartTime = static_cast<float>(ImGui::GetTime());
                    ctx.startPos = cameraPos;
                    ctx.targetPos = targetPosition;
                    ctx.startUp = GizmoMath::multiply_qv(cameraRot, worldUp);
                    ctx.targetUp = targetUp;

                    ctx.animStartDist = GizmoMath::length(GizmoMath::subtract_vv(ctx.startPos, pivot));
                    ctx.animTargetDist = GizmoMath::length(GizmoMath::subtract_vv(ctx.targetPos, pivot));

                    if (ctx.animStartDist > 0.0001f)
                    {
                        ctx.animStartDir = GizmoMath::normalize(GizmoMath::subtract_vv(ctx.startPos, pivot));
                    }
                    else
                    {
                        ctx.animStartDir = GizmoMath::multiply_vf(worldForward, -1.f);
                    }

                    if (ctx.animTargetDist > 0.0001f)
                    {
                        ctx.animTargetDir = GizmoMath::normalize(GizmoMath::subtract_vv(ctx.targetPos, pivot));
                    }
                }
            }
            else
            {
                cameraRot = targetRotation;
                cameraPos = targetPosition;
                wasModified = true;
            }
        }

        if (!io.MouseDown[0] && ctx.activeTool != TOOL_NONE)
        {
            ctx.activeTool = TOOL_NONE;
        }

        return wasModified;
    }

    // position is the button's top-left in screen space.
    inline bool Dolly(vec3_t& cameraPos, const quat_t& cameraRot, const ImVec2 position, const float zoomSpeed = 0.05f)
    {
        const ImGuiIO& io = ImGui::GetIO();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        auto& ctx = GetContext();
        const Style& style = GetStyle();
        bool wasModified = false;

        const bool canInteract = ctx.enabled && !(io.ConfigFlags & ImGuiConfigFlags_NoMouse);
        const float radius = style.toolButtonRadius * style.scale;
        const ImVec2 center = { position.x + radius, position.y + radius };

        bool isHovered = false;
        if (canInteract && (ctx.activeTool == TOOL_NONE || ctx.activeTool == TOOL_DOLLY))
        {
            if (GizmoLengthSqr({io.MousePos.x - center.x, io.MousePos.y - center.y}) < radius * radius)
            {
                isHovered = true;
            }
        }
        ctx.isZoomButtonHovered = isHovered;

        if (canInteract && (isHovered || ctx.activeTool == TOOL_DOLLY))
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        }

        if (canInteract && isHovered && ImGui::IsMouseClicked(0) && ctx.activeTool == TOOL_NONE)
        {
            ctx.activeTool = TOOL_DOLLY;
            ctx.isAnimating = false;
        }

        if (ctx.activeTool == TOOL_DOLLY && io.MouseDelta.y != 0.0f)
        {
            vec3_t forwardMovement = GizmoMath::multiply_vf(
                GizmoMath::multiply_qv(cameraRot, worldForward),
                -io.MouseDelta.y * zoomSpeed
            );
            cameraPos = GizmoMath::add_vv(cameraPos, forwardMovement);
            wasModified = true;
        }

        const ImU32 bgColor = (ctx.activeTool == TOOL_DOLLY || isHovered) ? style.toolButtonHoveredColor : style.toolButtonColor;
        drawList->AddCircleFilled(center, radius, bgColor);

        const float p = style.toolButtonInnerPadding * style.scale;
        const float th = 2.0f * style.scale;
        const ImU32 iconColor = style.toolButtonIconColor;
        constexpr float iconScale = 0.5f;
        const float scaledP = p * iconScale;
        const float scaledRadius = radius * iconScale;

        ImVec2 glassCenter = { center.x - scaledP / 2.0f, center.y - scaledP / 2.0f };
        const float glassRadius = scaledRadius - scaledP;
        drawList->AddCircle(glassCenter, glassRadius, iconColor, 0, th);

        const ImVec2 handleStart = { center.x + scaledRadius / 2.0f, center.y + scaledRadius / 2.0f };
        const ImVec2 handleEnd = { center.x + scaledRadius - (p * iconScale), center.y + scaledRadius - (p * iconScale) };
        drawList->AddLine(handleStart, handleEnd, iconColor, th);

        const float plusHalfSize = glassRadius * 0.5f;
        drawList->AddLine({glassCenter.x, glassCenter.y - plusHalfSize}, {glassCenter.x, glassCenter.y + plusHalfSize}, iconColor, th);
        drawList->AddLine({glassCenter.x - plusHalfSize, glassCenter.y}, {glassCenter.x + plusHalfSize, glassCenter.y}, iconColor, th);

        return wasModified;
    }

    // position is the button's top-left in screen space.
    inline bool Pan(vec3_t& cameraPos, const quat_t& cameraRot, const ImVec2 position, const float panSpeed = 0.01f)
    {
        const ImGuiIO& io = ImGui::GetIO();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        auto& ctx = GetContext();
        const Style& style = GetStyle();
        bool wasModified = false;

        const bool canInteract = ctx.enabled && !(io.ConfigFlags & ImGuiConfigFlags_NoMouse);
        const float radius = style.toolButtonRadius * style.scale;
        const ImVec2 center = { position.x + radius, position.y + radius };

        bool isHovered = false;
        if (canInteract && (ctx.activeTool == TOOL_NONE || ctx.activeTool == TOOL_PAN))
        {
            if (GizmoLengthSqr({io.MousePos.x - center.x, io.MousePos.y - center.y}) < radius * radius)
            {
                isHovered = true;
            }
        }
        ctx.isPanButtonHovered = isHovered;

        if (canInteract && (isHovered || ctx.activeTool == TOOL_PAN))
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }

        if (canInteract && isHovered && ImGui::IsMouseClicked(0) && ctx.activeTool == TOOL_NONE)
        {
            ctx.activeTool = TOOL_PAN;
            ctx.isAnimating = false;
        }

        if (ctx.activeTool == TOOL_PAN && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f))
        {
            vec3_t rightMovement = GizmoMath::multiply_vf(GizmoMath::multiply_qv(cameraRot, worldRight), -io.MouseDelta.x * panSpeed);
            vec3_t upMovement = GizmoMath::multiply_vf(GizmoMath::multiply_qv(cameraRot, worldUp), io.MouseDelta.y * panSpeed);
            cameraPos = GizmoMath::add_vv(cameraPos, rightMovement);
            cameraPos = GizmoMath::add_vv(cameraPos, upMovement);
            wasModified = true;
        }

        const ImU32 bgColor = (isHovered || ctx.activeTool == TOOL_PAN) ? style.toolButtonHoveredColor : style.toolButtonColor;
        drawList->AddCircleFilled(center, radius, bgColor);

        const ImU32 iconColor = style.toolButtonIconColor;
        const float th = 2.0f * style.scale;
        const float size = radius * 0.5f;
        const float arm = size * 0.25f;

        const ImVec2 topTip    = { center.x, center.y - size };
        drawList->AddLine({ topTip.x - arm, topTip.y + arm }, topTip, iconColor, th);
        drawList->AddLine({ topTip.x + arm, topTip.y + arm }, topTip, iconColor, th);
        const ImVec2 botTip    = { center.x, center.y + size };
        drawList->AddLine({ botTip.x - arm, botTip.y - arm }, botTip, iconColor, th);
        drawList->AddLine({ botTip.x + arm, botTip.y - arm }, botTip, iconColor, th);
        const ImVec2 leftTip   = { center.x - size, center.y };
        drawList->AddLine({ leftTip.x + arm, leftTip.y - arm }, leftTip, iconColor, th);
        drawList->AddLine({ leftTip.x + arm, leftTip.y + arm }, leftTip, iconColor, th);
        const ImVec2 rightTip  = { center.x + size, center.y };
        drawList->AddLine({ rightTip.x - arm, rightTip.y - arm }, rightTip, iconColor, th);
        drawList->AddLine({ rightTip.x - arm, rightTip.y + arm }, rightTip, iconColor, th);

        return wasModified;
    }
}
