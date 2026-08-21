#include "RuntimePCH.h"
#include "Color.h"
#include "Core/Math/Random.h"

namespace Lumina
{
    constexpr FColor FColor::Red        = FColor(1.0f, 0.0f, 0.0f);
    constexpr FColor FColor::Green      = FColor(0.0f, 1.0f, 0.0f);
    constexpr FColor FColor::Blue       = FColor(0.0f, 0.0f, 1.0f);
    constexpr FColor FColor::Yellow     = FColor(1.0f, 1.0f, 0.0f);
    constexpr FColor FColor::White      = FColor(1.0f, 1.0f, 1.0f);
    constexpr FColor FColor::Black      = FColor(0.0f, 0.0f, 0.0f);

    // A shared unsynchronized generator corrupted whenever two threads asked for a random color.

    FColor FColor::MakeRandom(float alpha)
    {
        FRandomStream& Random = Math::ThreadRandomStream();
        return FColor(Random.NextFloat(), Random.NextFloat(), Random.NextFloat(), alpha);
    }

    FColor FColor::MakeRandomWithAlpha()
    {
        FRandomStream& Random = Math::ThreadRandomStream();
        return FColor(Random.NextFloat(), Random.NextFloat(), Random.NextFloat(), Random.NextFloat());
    }

    FColor FColor::MakeRandomVibrant(float alpha)
    {
        FRandomStream& Random = Math::ThreadRandomStream();
        const float Hue        = Random.NextFloat();
        const float Saturation = Random.RandRange(0.7f, 1.0f);
        const float Lightness  = Random.RandRange(0.4f, 0.6f);

        return HSLtoRGB(Hue, Saturation, Lightness, alpha);
    }

    FColor FColor::MakeRandomPastel(float alpha)
    {
        FRandomStream& Random = Math::ThreadRandomStream();
        const float Hue        = Random.NextFloat();
        const float Saturation = Random.RandRange(0.2f, 0.5f);
        const float Lightness  = Random.RandRange(0.7f, 0.9f);

        return HSLtoRGB(Hue, Saturation, Lightness, alpha);
    }
}
