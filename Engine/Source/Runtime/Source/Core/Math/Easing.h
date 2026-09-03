#pragma once

#include "Core/Math/Math.h"
#include "Core/Object/ObjectMacros.h"
#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"
#include "Easing.generated.h"

namespace Lumina
{
    /** Curve family, chosen independently of the direction it is applied in. */
    REFLECT()
    enum class EEaseTransition : uint8
    {
        Linear,
        Sine,
        Quad,
        Cubic,
        Quart,
        Quint,
        Expo,
        Circ,
        /** Overshoots slightly before settling. */
        Back,
        /** Oscillates past the target and rings down. */
        Elastic,
        /** Settles in decaying hops. */
        Bounce,
        /** Overshoots once, softer than Back. */
        Spring,
    };

    /** Which end of the curve the shaping is applied to. */
    REFLECT()
    enum class EEaseType : uint8
    {
        In,
        Out,
        InOut,
        OutIn,
    };

    namespace Easing
    {
        /** True for the curves that fold at compile time; the rest need cmath, which is not constexpr yet. */
        NODISCARD constexpr bool IsConstantEvaluable(EEaseTransition Transition)
        {
            switch (Transition)
            {
            case EEaseTransition::Linear:
            case EEaseTransition::Quad:
            case EEaseTransition::Cubic:
            case EEaseTransition::Quart:
            case EEaseTransition::Quint:
            case EEaseTransition::Back:
                return true;
            default:
                return false;
            }
        }

        namespace Private
        {
            NODISCARD constexpr float PowIn(float X, int32 Power)
            {
                float Result = X;
                for (int32 i = 1; i < Power; ++i)
                {
                    Result *= X;
                }
                return Result;
            }

            NODISCARD inline float BounceOut(float T)
            {
                constexpr float N = 7.5625f;
                constexpr float D = 2.75f;

                if (T < 1.0f / D) { return N * T * T; }
                if (T < 2.0f / D) { T -= 1.5f / D;  return N * T * T + 0.75f; }
                if (T < 2.5f / D) { T -= 2.25f / D; return N * T * T + 0.9375f; }

                T -= 2.625f / D;
                return N * T * T + 0.984375f;
            }

            NODISCARD inline float SpringOut(float T)
            {
                const float S = 1.0f - T;
                return (Math::Sin(T * Math::Pi<float>() * (0.2f + 2.5f * T * T * T))
                        * Math::Pow(S, 2.2f) + T) * (1.0f + 1.2f * S);
            }

            // Every transition is authored as its In form; Out, InOut and OutIn are mirrored off it.
            NODISCARD constexpr float EaseIn(EEaseTransition Transition, float T)
            {
                switch (Transition)
                {
                case EEaseTransition::Linear: return T;
                case EEaseTransition::Quad:   return PowIn(T, 2);
                case EEaseTransition::Cubic:  return PowIn(T, 3);
                case EEaseTransition::Quart:  return PowIn(T, 4);
                case EEaseTransition::Quint:  return PowIn(T, 5);

                case EEaseTransition::Back:
                {
                    constexpr float S = 1.70158f;
                    return T * T * ((S + 1.0f) * T - S);
                }

                case EEaseTransition::Sine:   return 1.0f - Math::Cos(T * Math::HalfPi<float>());
                case EEaseTransition::Expo:   return (T <= 0.0f) ? 0.0f : Math::Pow(2.0f, 10.0f * (T - 1.0f));
                case EEaseTransition::Circ:   return 1.0f - Math::Sqrt(Math::Max(0.0f, 1.0f - T * T));

                case EEaseTransition::Elastic:
                {
                    if (T <= 0.0f) { return 0.0f; }
                    if (T >= 1.0f) { return 1.0f; }

                    constexpr float Period = 0.3f;
                    constexpr float Shift  = Period * 0.25f;
                    const float     X      = T - 1.0f;
                    return -Math::Pow(2.0f, 10.0f * X)
                         * Math::Sin((X - Shift) * Math::TwoPi<float>() / Period);
                }

                case EEaseTransition::Bounce: return 1.0f - BounceOut(1.0f - T);
                case EEaseTransition::Spring: return 1.0f - SpringOut(1.0f - T);
                }
                return T;
            }
        }

        // Back, Elastic, Bounce and Spring leave the unit range on purpose, so clamp the result if it matters.
        NODISCARD constexpr float Evaluate(EEaseTransition Transition, EEaseType Ease, float Alpha)
        {
            const float T = Math::Clamp(Alpha, 0.0f, 1.0f);

            switch (Ease)
            {
            case EEaseType::In:
                return Private::EaseIn(Transition, T);

            case EEaseType::Out:
                return 1.0f - Private::EaseIn(Transition, 1.0f - T);

            case EEaseType::InOut:
                return (T < 0.5f)
                     ? 0.5f * Private::EaseIn(Transition, T * 2.0f)
                     : 1.0f - 0.5f * Private::EaseIn(Transition, 2.0f - T * 2.0f);

            case EEaseType::OutIn:
                return (T < 0.5f)
                     ? 0.5f * (1.0f - Private::EaseIn(Transition, 1.0f - T * 2.0f))
                     : 0.5f + 0.5f * Private::EaseIn(Transition, T * 2.0f - 1.0f);
            }
            return T;
        }

        NODISCARD constexpr const char* ToString(EEaseTransition Transition)
        {
            switch (Transition)
            {
            case EEaseTransition::Linear:  return "Linear";
            case EEaseTransition::Sine:    return "Sine";
            case EEaseTransition::Quad:    return "Quad";
            case EEaseTransition::Cubic:   return "Cubic";
            case EEaseTransition::Quart:   return "Quart";
            case EEaseTransition::Quint:   return "Quint";
            case EEaseTransition::Expo:    return "Expo";
            case EEaseTransition::Circ:    return "Circ";
            case EEaseTransition::Back:    return "Back";
            case EEaseTransition::Elastic: return "Elastic";
            case EEaseTransition::Bounce:  return "Bounce";
            case EEaseTransition::Spring:  return "Spring";
            }
            return "Unknown";
        }

        NODISCARD constexpr const char* ToString(EEaseType Ease)
        {
            switch (Ease)
            {
            case EEaseType::In:    return "In";
            case EEaseType::Out:   return "Out";
            case EEaseType::InOut: return "InOut";
            case EEaseType::OutIn: return "OutIn";
            }
            return "Unknown";
        }
    }
}
