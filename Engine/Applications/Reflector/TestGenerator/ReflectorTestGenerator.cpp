#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

#include "Reflector/Public/ReflectionGenerator.h"

namespace
{
    using namespace Lumina::Reflection;

    // Reports every FixtureTag property, then fails the way its Mode option names, so a test can drive each failure.
    class FFixtureGenerator final : public IReflectionGenerator
    {
    public:

        explicit FFixtureGenerator(const FGeneratorOptions& Options)
        {
            for (const auto& [Key, Value] : Options)
            {
                if (Key == "Mode")
                {
                    Mode = Value;
                }
            }
        }

        std::string_view GetName() const override { return "ReflectorTestGenerator"; }

        void ClaimSpecifiers(FSpecifierRegistry& Registry) override
        {
            Registry.Claim(ESpecifierTarget::Property, { "FixtureTag", ESpecifierForm::Flag, ESpecifierConsumer::Reflector, "Lists the property in the fixture's report." });
        }

        void GenerateModule(const FModuleContext& Context) override
        {
            const std::string FileName = Mode == "Clobber" ? "ReflectionUnity_0.gen.cpp" : Context.Module.Name() + ".fixture.txt";
            FCodeWriter& Report = Context.Outputs.Open(Context.Module.GeneratedDir() + "/" + FileName);

            for (const FHeaderView Header : Context.Module.Headers())
            {
                for (const FTypeView Type : Header.Types())
                {
                    for (const FPropertyView Property : Type.Properties())
                    {
                        if (!Property.HasMetadata("FixtureTag"))
                        {
                            continue;
                        }
                        Report.Line(Type.DisplayName() + "." + Property.Name());
                        if (Mode == "Error")
                        {
                            Context.Run.Diagnostics.Error(Property.Location(), "Fixture error on '%s'.", Property.Name().c_str());
                        }
                    }
                }
            }

            if (Mode == "Throw")
            {
                throw std::runtime_error("fixture failure");
            }
            if (Mode == "Crash")
            {
                volatile int* Null = nullptr;
                *Null = 1;
            }
            if (Mode == "Hang")
            {
                std::this_thread::sleep_for(std::chrono::hours(1));
            }
        }

    private:

        std::string Mode;
    };
}

LUMINA_REFLECTION_GENERATOR(FFixtureGenerator)
