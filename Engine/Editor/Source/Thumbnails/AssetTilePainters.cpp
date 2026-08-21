#include "AssetTilePainters.h"

#include "ThumbnailManager.h"
#include "ThumbnailUtils.h"
#include "Assets/AssetTypes/Curve/CurveAsset.h"
#include "Core/Math/Math.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/Object.h"
#include "Core/Object/Package/Thumbnail/PackageThumbnail.h"
#include "Tools/UI/ImGui/EditorColors.h"

namespace Lumina
{
    FAssetTilePainterRegistry& FAssetTilePainterRegistry::Get()
    {
        static FAssetTilePainterRegistry Instance;
        return Instance;
    }

    void FAssetTilePainterRegistry::Register(CClass* AssetClass, FAssetTilePainterFn Painter)
    {
        if (AssetClass == nullptr || !Painter)
        {
            return;
        }
        Painters.insert_or_assign(AssetClass, std::move(Painter));
    }

    FAssetTilePainterFn* FAssetTilePainterRegistry::Find(CClass* AssetClass)
    {
        // Most-derived first, matching the editor-tool registry, so a subclass can override its base.
        for (CClass* Current = AssetClass; Current != nullptr; Current = Cast<CClass>(Current->GetSuperStruct()))
        {
            auto It = Painters.find(Current);
            if (It != Painters.end())
            {
                return &It->second;
            }
        }
        return nullptr;
    }

    namespace
    {
        /** Max samples in a curve preview. Fixed so both backends can use a stack array. */
        constexpr int32 kMaxCurveSamples = 128;

        // Breathing room, so the curve extremes are not welded to the border.
        constexpr float kInsetFraction = 0.12f;

        // Both axes normalized to [0,1] with Y flipped so 0 is the TOP, shared by tile and thumbnail paths.
        struct FCurveSamples
        {
            float X[kMaxCurveSamples] = {};
            float Y[kMaxCurveSamples] = {};
            int32 Count = 0;

            // Valid only when zero falls INSIDE the value range, since a clamped baseline reads as part of the shape.
            bool  bHasZeroLine = false;
            float ZeroY = 0.0f;

            // Key positions, for backends that want to mark them.
            float KeyX[kMaxCurveSamples] = {};
            float KeyY[kMaxCurveSamples] = {};
            int32 KeyCount = 0;
        };

        // A degenerate curve yields a flat centered line, since a blank preview reads as a load failure.
        void SampleCurve(const SKeyedCurve& Curve, int32 DesiredSamples, FCurveSamples& Out)
        {
            const int32 SampleCount = Math::Clamp(DesiredSamples, 2, kMaxCurveSamples);

            auto EmitFlat = [&]()
            {
                Out.Count = 2;
                Out.X[0] = 0.0f; Out.Y[0] = 0.5f;
                Out.X[1] = 1.0f; Out.Y[1] = 0.5f;
                Out.KeyCount = 0;
                Out.bHasZeroLine = false;
            };

            if (Curve.NumKeys() < 2)
            {
                EmitFlat();
                return;
            }

            float TimeMin = 0.0f;
            float TimeMax = 0.0f;
            Curve.GetTimeRange(TimeMin, TimeMax);
            const float TimeSpan = TimeMax - TimeMin;
            if (TimeSpan <= 0.0f)
            {
                EmitFlat();
                return;
            }

            float ValueMin = 0.0f;
            float ValueMax = 0.0f;
            Curve.GetValueRange(ValueMin, ValueMax);

            // A constant curve has zero value span, so widen it to keep the divide finite and the line centered.
            float ValueSpan = ValueMax - ValueMin;
            if (ValueSpan <= Math::Epsilon<float>())
            {
                const float Pad = Math::Max(Math::Abs(ValueMax) * 0.5f, 1.0f);
                ValueMin -= Pad;
                ValueMax += Pad;
                ValueSpan = ValueMax - ValueMin;
            }

            auto ToNormalizedY = [&](float Value)
            {
                return 1.0f - ((Value - ValueMin) / ValueSpan);
            };

            Out.Count = SampleCount;
            for (int32 i = 0; i < SampleCount; ++i)
            {
                const float Alpha = (float)i / (float)(SampleCount - 1);
                Out.X[i] = Alpha;
                Out.Y[i] = ToNormalizedY(Curve.Evaluate(TimeMin + Alpha * TimeSpan));
            }

            Out.bHasZeroLine = (ValueMin < 0.0f && ValueMax > 0.0f);
            Out.ZeroY        = Out.bHasZeroLine ? ToNormalizedY(0.0f) : 0.0f;

            Out.KeyCount = Math::Min((int32)Curve.Keys.size(), kMaxCurveSamples);
            for (int32 i = 0; i < Out.KeyCount; ++i)
            {
                Out.KeyX[i] = (Curve.Keys[i].Time - TimeMin) / TimeSpan;
                Out.KeyY[i] = ToNormalizedY(Curve.Keys[i].Value);
            }
        }

        //~ Live tile painter -------------------------------------------------------------------------

        void PaintCurveTile(CObject* Asset, ImDrawList& DrawList, const ImVec2& Min, const ImVec2& Max)
        {
            const CCurveAsset* CurveAsset = Cast<CCurveAsset>(Asset);
            if (CurveAsset == nullptr)
            {
                return;
            }

            const float Width  = Max.x - Min.x;
            const float Height = Max.y - Min.y;
            if (Width <= 4.0f || Height <= 4.0f)
            {
                return;
            }

            const float PlotL = Min.x + Width * kInsetFraction;
            const float PlotR = Max.x - Width * kInsetFraction;
            const float PlotT = Min.y + Height * kInsetFraction;
            const float PlotB = Max.y - Height * kInsetFraction;

            FCurveSamples Samples;
            SampleCurve(CurveAsset->Curve, (int32)((PlotR - PlotL) * 0.5f), Samples);

            const ImU32 CurveColor = EditorColors::U32(EditorColors::Accent());
            const ImU32 AxisColor  = EditorColors::U32(EditorColors::WithAlpha(EditorColors::TextMuted(), 0.35f));

            auto ToScreen = [&](float NX, float NY)
            {
                return ImVec2(PlotL + NX * (PlotR - PlotL), PlotT + NY * (PlotB - PlotT));
            };

            if (Samples.bHasZeroLine)
            {
                const float ZeroY = PlotT + Samples.ZeroY * (PlotB - PlotT);
                DrawList.AddLine(ImVec2(PlotL, ZeroY), ImVec2(PlotR, ZeroY), AxisColor, 1.0f);
            }

            ImVec2 Points[kMaxCurveSamples];
            for (int32 i = 0; i < Samples.Count; ++i)
            {
                Points[i] = ToScreen(Samples.X[i], Samples.Y[i]);
            }
            DrawList.AddPolyline(Points, Samples.Count, CurveColor, ImDrawFlags_None, 2.0f);

            // Key dots, but only on a tile big enough that they read as points rather than noise.
            if ((PlotR - PlotL) >= 56.0f)
            {
                for (int32 i = 0; i < Samples.KeyCount; ++i)
                {
                    DrawList.AddCircleFilled(ToScreen(Samples.KeyX[i], Samples.KeyY[i]), 2.0f, CurveColor);
                }
            }
        }

        //~ CPU thumbnail rasterizer ------------------------------------------------------------------

        struct FRGBA { uint8 R, G, B, A; };

        FRGBA ToRGBA8(const ImVec4& C)
        {
            auto Chan = [](float V) { return (uint8)(Math::Clamp(V, 0.0f, 1.0f) * 255.0f + 0.5f); };
            return FRGBA{ Chan(C.x), Chan(C.y), Chan(C.z), Chan(C.w) };
        }

        // A curve is a FUNCTION, so filling each column between adjacent samples needs no line algorithm.
        bool PaintCurveThumbnail(CObject* Asset, uint32 Size, FPackageThumbnail& Out)
        {
            const CCurveAsset* CurveAsset = Cast<CCurveAsset>(Asset);
            if (CurveAsset == nullptr || Size == 0)
            {
                return false;
            }

            constexpr size_t BytesPerPixel = 4;
            Out.LoadState.store(FPackageThumbnail::EState::None, std::memory_order_relaxed);
            Out.ImageWidth  = Size;
            Out.ImageHeight = Size;
            Out.ImageData.assign((size_t)Size * Size * BytesPerPixel, 0);

            const FRGBA Background = ToRGBA8(ImVec4(0.16f, 0.16f, 0.17f, 1.0f));   // matches the tile frame
            const FRGBA CurveColor = ToRGBA8(EditorColors::Accent());
            const FRGBA AxisColor  = ToRGBA8(ImVec4(0.45f, 0.45f, 0.48f, 1.0f));

            uint8* Pixels = Out.ImageData.data();

            // StoreDownsampledRGBA stores flipped and the upload flips back, so every write goes through Plot.
            auto Plot = [&](int32 X, int32 Y, const FRGBA& C)
            {
                if (X < 0 || Y < 0 || X >= (int32)Size || Y >= (int32)Size)
                {
                    return;
                }
                const size_t Index = ((size_t)((int32)Size - 1 - Y) * Size + X) * BytesPerPixel;
                Pixels[Index + 0] = C.R;
                Pixels[Index + 1] = C.G;
                Pixels[Index + 2] = C.B;
                Pixels[Index + 3] = C.A;
            };

            for (uint32 Y = 0; Y < Size; ++Y)
            {
                for (uint32 X = 0; X < Size; ++X)
                {
                    Plot((int32)X, (int32)Y, Background);
                }
            }

            const float Inset = (float)Size * kInsetFraction;
            const float PlotL = Inset;
            const float PlotR = (float)Size - Inset;
            const float PlotT = Inset;
            const float PlotB = (float)Size - Inset;

            FCurveSamples Samples;
            SampleCurve(CurveAsset->Curve, kMaxCurveSamples, Samples);

            if (Samples.bHasZeroLine)
            {
                const int32 ZeroY = (int32)(PlotT + Samples.ZeroY * (PlotB - PlotT));
                for (int32 X = (int32)PlotL; X <= (int32)PlotR; ++X)
                {
                    Plot(X, ZeroY, AxisColor);
                }
            }

            auto SampleYAt = [&](float PixelX)
            {
                // Interpolates between the two samples straddling the pixel, so sample count is independent of it.
                const float Alpha = Math::Clamp((PixelX - PlotL) / Math::Max(PlotR - PlotL, 1.0f), 0.0f, 1.0f);
                const float Scaled = Alpha * (float)(Samples.Count - 1);
                const int32 Index0 = Math::Clamp((int32)Scaled, 0, Samples.Count - 1);
                const int32 Index1 = Math::Min(Index0 + 1, Samples.Count - 1);
                const float Frac   = Scaled - (float)Index0;
                const float NY     = Samples.Y[Index0] + (Samples.Y[Index1] - Samples.Y[Index0]) * Frac;
                return PlotT + NY * (PlotB - PlotT);
            };

            constexpr int32 HalfThickness = 1;   // 3px line, which reads cleanly at 256

            float PreviousY = SampleYAt(PlotL);
            for (int32 X = (int32)PlotL; X <= (int32)PlotR; ++X)
            {
                const float CurrentY = SampleYAt((float)X);

                // Span from the previous column's value to this one, so steep segments stay connected.
                int32 Y0 = (int32)Math::Min(PreviousY, CurrentY) - HalfThickness;
                int32 Y1 = (int32)Math::Max(PreviousY, CurrentY) + HalfThickness;
                for (int32 Y = Y0; Y <= Y1; ++Y)
                {
                    Plot(X, Y, CurveColor);
                }

                PreviousY = CurrentY;
            }

            return true;
        }
    }

    namespace AssetTilePainters
    {
        void RegisterBuiltin()
        {
            FAssetTilePainterRegistry::Get().Register(CCurveAsset::StaticClass(), &PaintCurveTile);
            CThumbnailManager::Get().RegisterThumbnailPainter(CCurveAsset::StaticClass(), &PaintCurveThumbnail);
        }
    }
}
