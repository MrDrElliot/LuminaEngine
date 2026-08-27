#include "BenchCommon.h"

namespace ECSBench
{
    namespace
    {
        // Both sides state their layout, so the sweep measures packed against paged whatever the threshold is.
        template<uint32 Bytes>
        struct TSized
        {
            static constexpr auto Layout = ECS::EComponentLayout::Packed;
            float M[Bytes / 4] = {};
        };

        // Forcing the layout rather than in-place delete keeps swap-and-pop on both sides.
        template<uint32 Bytes>
        struct TSizedPaged
        {
            static constexpr auto Layout = ECS::EComponentLayout::Paged;
            float M[Bytes / 4] = {};
        };

        struct FLayoutRow
        {
            uint32 Bytes = 0;
            double PackedEmplace = 0.0;
            double PagedEmplace  = 0.0;
            double PackedIterate = 0.0;
            double PagedIterate  = 0.0;
            double PackedGet     = 0.0;
            double PagedGet      = 0.0;
        };

        template<typename T>
        double MeasureEmplace(size_t EntityCount, size_t Passes)
        {
            return MeasureNanosPerOp(EntityCount, Passes, [&]
            {
                ECS::FRegistry Registry;
                for (size_t Index = 0; Index < EntityCount; ++Index)
                {
                    Registry.Emplace<T>(Registry.Create());
                }
            });
        }

        template<typename T>
        double MeasureIterate(size_t EntityCount, size_t Passes)
        {
            ECS::FRegistry Registry;
            for (size_t Index = 0; Index < EntityCount; ++Index)
            {
                Registry.Emplace<T>(Registry.Create());
            }

            float Sink = 0.0f;
            const double Nanos = MeasureNanosPerOp(EntityCount, Passes, [&]
            {
                float Total = 0.0f;
                Registry.View<T>().ForEach([&Total](ECS::FEntity, T& Value) { Total += Value.M[0]; });
                Sink = Total;
            });

            return Sink == 12345.0f ? Nanos + 1.0 : Nanos;
        }

        template<typename T>
        double MeasureRandomGet(size_t EntityCount, size_t Passes)
        {
            ECS::FRegistry Registry;
            TVector<ECS::FEntity> Entities;
            Entities.reserve(EntityCount);

            for (size_t Index = 0; Index < EntityCount; ++Index)
            {
                const ECS::FEntity Entity = Registry.Create();
                Registry.Emplace<T>(Entity);
                Entities.push_back(Entity);
            }

            FSplitMix Random(0x5EEDu);
            TVector<ECS::FEntity> Order;
            Order.reserve(EntityCount);
            for (size_t Index = 0; Index < EntityCount; ++Index)
            {
                Order.push_back(Entities[Random.NextBelow(static_cast<uint32>(EntityCount))]);
            }

            float Sink = 0.0f;
            const double Nanos = MeasureNanosPerOp(EntityCount, Passes, [&]
            {
                float Total = 0.0f;
                for (size_t Index = 0; Index < Order.size(); ++Index)
                {
                    Total += Registry.Get<T>(Order[Index]).M[0];
                }
                Sink = Total;
            });

            return Sink == 12345.0f ? Nanos + 1.0 : Nanos;
        }

        template<uint32 Bytes>
        FLayoutRow MeasureSize(size_t EntityCount, size_t Passes)
        {
            FLayoutRow Row;
            Row.Bytes = Bytes;
            Row.PackedEmplace = MeasureEmplace<TSized<Bytes>>(EntityCount, Passes);
            Row.PagedEmplace  = MeasureEmplace<TSizedPaged<Bytes>>(EntityCount, Passes);
            Row.PackedIterate = MeasureIterate<TSized<Bytes>>(EntityCount, Passes);
            Row.PagedIterate  = MeasureIterate<TSizedPaged<Bytes>>(EntityCount, Passes);
            Row.PackedGet     = MeasureRandomGet<TSized<Bytes>>(EntityCount, Passes);
            Row.PagedGet      = MeasureRandomGet<TSizedPaged<Bytes>>(EntityCount, Passes);
            return Row;
        }

        void PrintRow(const FLayoutRow& Row)
        {
            std::printf("%6u %9.3f %9.3f %8.2fx %9.3f %9.3f %8.2fx %9.3f %9.3f %8.2fx\n",
                Row.Bytes,
                Row.PackedEmplace, Row.PagedEmplace, Row.PackedEmplace / Row.PagedEmplace,
                Row.PackedIterate, Row.PagedIterate, Row.PackedIterate / Row.PagedIterate,
                Row.PackedGet, Row.PagedGet, Row.PackedGet / Row.PagedGet);
        }
    }

    void RunLayoutSweepCases(size_t EntityCount, size_t Passes)
    {
        std::printf("\nLayout sweep, packed against paged, nanoseconds per op. Ratio above 1.00 favors paged.\n\n");
        std::printf("%6s %9s %9s %9s %9s %9s %9s %9s %9s %9s\n",
            "bytes", "emp pack", "emp page", "ratio", "itr pack", "itr page", "ratio", "get pack", "get page", "ratio");
        std::printf("%s\n", "-------------------------------------------------------------------------------------------------");

        PrintRow(MeasureSize<8>(EntityCount, Passes));
        PrintRow(MeasureSize<16>(EntityCount, Passes));
        PrintRow(MeasureSize<24>(EntityCount, Passes));
        PrintRow(MeasureSize<32>(EntityCount, Passes));
        PrintRow(MeasureSize<48>(EntityCount, Passes));
        PrintRow(MeasureSize<64>(EntityCount, Passes));
        PrintRow(MeasureSize<128>(EntityCount, Passes));
    }
}
