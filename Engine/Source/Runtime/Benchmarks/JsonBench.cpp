#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "Core/Json/Json.h"

namespace LuminaJsonBench
{
    using Lumina::FJsonValue;
    using Lumina::FStringView;

    volatile Lumina::uint64 GSink = 0;

    FStringView View(const std::string& Text)
    {
        return FStringView(Text.data(), static_cast<Lumina::int32>(Text.size()));
    }

    std::string MakeEntityDocument(int Count)
    {
        std::string Out = "[";
        for (int Index = 0; Index < Count; ++Index)
        {
            if (Index)
            {
                Out += ",";
            }
            Out += "{\"Id\":" + std::to_string(Index * 7919)
                + ",\"Name\":\"Entity_" + std::to_string(Index) + "\""
                + ",\"Position\":[" + std::to_string(Index * 0.25) + "," + std::to_string(-Index * 1.5) + ",3.14159265358979]"
                + ",\"Enabled\":" + (Index % 3 ? "true" : "false")
                + ",\"Tags\":[\"Physics\",\"Render\",\"Gameplay.Status.Burning\"]"
                + ",\"Description\":\"A longer description with \\\"quotes\\\", a tab\\t and caf\xC3\xA9 in it, long enough to need the heap.\""
                + ",\"Parent\":null}";
        }
        return Out + "]";
    }

    std::string MakeRpcMessage(int Id)
    {
        return "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(Id)
            + ",\"method\":\"tools/call\",\"params\":{\"name\":\"entity.set_property\",\"arguments\":{\"Entity\":\"ent_1_"
            + std::to_string(Id % 97) + "\",\"Component\":\"STransformComponent\",\"Path\":\"Location\",\"Value\":[1.0,2.5,-3.75]}}}";
    }

    template <typename TBody>
    double BestSeconds(int Repeats, TBody&& Body)
    {
        double Best = 1e30;
        for (int Repeat = 0; Repeat < Repeats; ++Repeat)
        {
            const auto Start = std::chrono::steady_clock::now();
            Body();
            const double Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
            Best = Seconds < Best ? Seconds : Best;
        }
        return Best;
    }

    void Throughput(const char* Label, double Bytes, double Seconds)
    {
        std::printf("  %-36s %8.1f MB/s\n", Label, Bytes / Seconds / 1e6);
    }

    void MeasureDocument(const char* Label, const std::string& Text)
    {
        const double Bytes = static_cast<double>(Text.size());
        Throughput((std::string(Label) + " parse").c_str(), Bytes, BestSeconds(5, [&]
        {
            FJsonValue Value;
            GSink = GSink + (FJsonValue::Parse(View(Text), Value) ? static_cast<Lumina::uint64>(Value.Num()) : 0);
        }));

        FJsonValue Value;
        ASSERT_TRUE(FJsonValue::Parse(View(Text), Value));
        Throughput((std::string(Label) + " write").c_str(), Bytes, BestSeconds(5, [&] { GSink = GSink + static_cast<Lumina::uint64>(Value.ToString().Len()); }));
        Throughput((std::string(Label) + " write pretty").c_str(), Bytes, BestSeconds(5, [&] { GSink = GSink + static_cast<Lumina::uint64>(Value.ToString(4).Len()); }));
    }

    TEST(JsonBench, DocumentsMessagesAndLookups)
    {
        std::printf("JSON throughput (best of 5):\n");
        const std::string Entities = MakeEntityDocument(20000);
        MeasureDocument("entities 4MB", Entities);

        FJsonValue Parsed;
        ASSERT_TRUE(FJsonValue::Parse(View(Entities), Parsed));
        const Lumina::FString Pretty = Parsed.ToString(4);
        MeasureDocument("entities pretty", std::string(Pretty.GetData(), static_cast<size_t>(Pretty.Len())));

        std::vector<std::string> Rpc;
        double Bytes = 0;
        for (int Id = 0; Id < 20000; ++Id)
        {
            Rpc.push_back(MakeRpcMessage(Id));
            Bytes += static_cast<double>(Rpc.back().size());
        }
        Throughput("rpc messages parse+read+write", Bytes, BestSeconds(5, [&]
        {
            for (const std::string& Message : Rpc)
            {
                FJsonValue Value;
                if (FJsonValue::Parse(View(Message), Value))
                {
                    GSink = GSink + static_cast<Lumina::uint64>(Value["params"]["arguments"]["Value"][1].AsDouble() * 2);
                    GSink = GSink + static_cast<Lumina::uint64>(Value.ToString().Len());
                }
            }
        }));

        FJsonValue Wide = FJsonValue::MakeObject();
        std::vector<std::string> Keys;
        for (int Index = 0; Index < 48; ++Index)
        {
            Keys.push_back("Property_" + std::to_string(Index));
            Wide.Set(View(Keys.back()), Index);
        }
        const double Lookup = BestSeconds(5, [&]
        {
            for (int Round = 0; Round < 20000; ++Round)
            {
                for (const std::string& Key : Keys)
                {
                    GSink = GSink + static_cast<Lumina::uint64>(Wide.Find(View(Key))->AsInt64());
                }
            }
        });
        std::printf("  %-36s %8.1f ns\n", "lookup in a 48-key object", Lookup / (20000.0 * 48) * 1e9);
    }

    TEST(JsonBench, ParseCostPerValueKind)
    {
        const auto Time = [](const char* Label, const std::string& Text, int Items)
        {
            const double Seconds = BestSeconds(5, [&]
            {
                FJsonValue Value;
                GSink = GSink + (FJsonValue::Parse(View(Text), Value) ? 1u : 0u);
            });
            std::printf("  %-28s %7.1f ns/item  (%.1f MB/s)\n", Label, Seconds / Items * 1e9, static_cast<double>(Text.size()) / Seconds / 1e6);
        };

        const int Count = 200000;
        std::string Ints = "[", Doubles = "[", Short = "[", Long = "[", Objects = "[", Empty = "[";
        for (int Index = 0; Index < Count; ++Index)
        {
            const std::string Comma = Index ? "," : "";
            Ints += Comma + std::to_string(Index * 7919);
            Doubles += Comma + std::to_string(Index * 0.25);
            Short += Comma + "\"Entity_" + std::to_string(Index % 1000) + "\"";
            Long += Comma + "\"A longer description that needs the heap, number " + std::to_string(Index) + "\"";
            Objects += Comma + "{\"a\":1,\"bb\":true}";
            Empty += Comma + "[]";
        }
        std::printf("JSON parse cost per value kind (best of 5):\n");
        Time("ints", Ints + "]", Count);
        Time("doubles", Doubles + "]", Count);
        Time("short strings", Short + "]", Count);
        Time("long strings", Long + "]", Count);
        Time("small objects", Objects + "]", Count);
        Time("empty arrays", Empty + "]", Count);
    }
}
