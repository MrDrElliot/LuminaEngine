#include <gtest/gtest.h>

#include "Platform/GenericPlatform.h"
#include <RmlUi/Core/ConvolutionFilter.h>

#include <random>
#include <vector>

using namespace Rml;

namespace
{
    // The loop the filter used to run, kept as the reference its faster form has to match byte for byte.
    std::vector<byte> ReferenceRun(const std::vector<float>& Kernel, Vector2i KernelSize, FilterOperation Operation,
        Vector2i DestinationSize, const std::vector<byte>& Source, Vector2i SourceSize, Vector2i SourceOffset)
    {
        std::vector<byte> Destination((size_t)(DestinationSize.x * DestinationSize.y), 0);
        const Vector2i Radius = (KernelSize - Vector2i(1)) / 2;
        for (int Y = 0; Y < DestinationSize.y; ++Y)
        {
            for (int X = 0; X < DestinationSize.x; ++X)
            {
                float Opacity = 0.0f;
                for (int KY = 0; KY < KernelSize.y; ++KY)
                {
                    const int SY = Y - SourceOffset.y - Radius.y + KY;
                    for (int KX = 0; KX < KernelSize.x; ++KX)
                    {
                        const int SX = X - SourceOffset.x - Radius.x + KX;
                        if (SY >= 0 && SY < SourceSize.y && SX >= 0 && SX < SourceSize.x)
                        {
                            const float Pixel = float(Source[(size_t)(SY * SourceSize.x + SX)]) * Kernel[(size_t)(KY * KernelSize.x + KX)];
                            Opacity = Operation == FilterOperation::Sum ? Opacity + Pixel : (Opacity > Pixel ? Opacity : Pixel);
                        }
                    }
                }
                Destination[(size_t)(Y * DestinationSize.x + X)] = byte(Opacity < 255.0f ? Opacity : 255.0f);
            }
        }
        return Destination;
    }

    void ExpectMatchesReference(FilterOperation Operation, int Radius, uint32 Seed)
    {
        std::mt19937 Random(Seed);
        std::uniform_int_distribution<int> Byte(0, 255);
        std::uniform_real_distribution<float> Weight(0.0f, 0.3f);

        ConvolutionFilter Filter;
        ASSERT_TRUE(Filter.Initialise(Radius, Operation));
        const Vector2i KernelSize(Radius * 2 + 1);
        std::vector<float> Kernel((size_t)(KernelSize.x * KernelSize.y));
        for (int Y = 0; Y < KernelSize.y; ++Y)
        {
            for (int X = 0; X < KernelSize.x; ++X)
            {
                // Some zero taps, since skipping them is part of what is being checked.
                const float Value = (X + Y) % 3 == 0 ? 0.0f : Weight(Random);
                Filter[Y][X] = Value;
                Kernel[(size_t)(Y * KernelSize.x + X)] = Value;
            }
        }

        const Vector2i SourceSize(23, 31);
        std::vector<byte> Source((size_t)(SourceSize.x * SourceSize.y));
        for (byte& Pixel : Source)
        {
            Pixel = (byte)Byte(Random);
        }

        const Vector2i Offset(Radius, Radius);
        const Vector2i DestinationSize = SourceSize + Vector2i(Radius * 2);
        std::vector<byte> Destination((size_t)(DestinationSize.x * DestinationSize.y), 0);
        Filter.Run(Destination.data(), DestinationSize, DestinationSize.x, ColorFormat::A8, Source.data(), SourceSize, Offset, ColorFormat::A8);

        EXPECT_EQ(Destination, ReferenceRun(Kernel, KernelSize, Operation, DestinationSize, Source, SourceSize, Offset));
    }
}

TEST(RmlConvolutionFilter, DilationMatchesTheReferenceLoop)
{
    for (int Radius = 1; Radius <= 6; ++Radius)
    {
        ExpectMatchesReference(FilterOperation::Dilation, Radius, 17u + (uint32)Radius);
    }
}

TEST(RmlConvolutionFilter, SumMatchesTheReferenceLoop)
{
    for (int Radius = 1; Radius <= 6; ++Radius)
    {
        ExpectMatchesReference(FilterOperation::Sum, Radius, 91u + (uint32)Radius);
    }
}
