#include "RuntimePCH.h"
#include "Color.h"
#include <random>

namespace Lumina
{
    constexpr FColor FColor::Red        = FColor(1.0f, 0.0f, 0.0f);
    constexpr FColor FColor::Green      = FColor(0.0f, 1.0f, 0.0f);
    constexpr FColor FColor::Blue       = FColor(0.0f, 0.0f, 1.0f);
    constexpr FColor FColor::Yellow     = FColor(1.0f, 1.0f, 0.0f);
    constexpr FColor FColor::White      = FColor(1.0f, 1.0f, 1.0f);
    constexpr FColor FColor::Black      = FColor(0.0f, 0.0f, 0.0f);

    FColor FColor::MakeRandom(float alpha)
    {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_real_distribution<float> dis(0.0f, 1.0f);
    
        return FColor(dis(gen), dis(gen), dis(gen), alpha);
    }

    FColor FColor::MakeRandomWithAlpha()
    {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_real_distribution<float> dis(0.0f, 1.0f);
    
        return FColor(dis(gen), dis(gen), dis(gen), dis(gen));
    }

    FColor FColor::MakeRandomVibrant(float alpha)
    {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_real_distribution<float> hue_dis(0.0f, 1.0f);
        static std::uniform_real_distribution<float> sat_dis(0.7f, 1.0f);
        static std::uniform_real_distribution<float> light_dis(0.4f, 0.6f);
    
        return HSLtoRGB(hue_dis(gen), sat_dis(gen), light_dis(gen), alpha);
    }

    FColor FColor::MakeRandomPastel(float alpha)
    {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_real_distribution<float> hue_dis(0.0f, 1.0f);
        static std::uniform_real_distribution<float> sat_dis(0.2f, 0.5f);
        static std::uniform_real_distribution<float> light_dis(0.7f, 0.9f);
    
        return HSLtoRGB(hue_dis(gen), sat_dis(gen), light_dis(gen), alpha);
    }
}
