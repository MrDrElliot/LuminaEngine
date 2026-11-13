#pragma once

#include <chrono>
#include <memory>
#include <vector>

#include "Reflector/ReflectorInput.h"
#include "Reflector/Diagnostics/LRTDiagnostics.h"
#include "Reflector/Public/ReflectionGenerator.h"

namespace Lumina::Reflection
{
    // Owns the run's generators and the registry they claim into, and drives them over one model.
    class FGeneratorRunner
    {
    public:

        // Starts with the built-in C++ and C# generators, in the order their fragments land in a shared file.
        FGeneratorRunner();

        // Loads a Reflection module's library, refusing one built against another API version and reporting it as an error.
        void Load(const FReflectorGeneratorInput& Input);

        const FSpecifierRegistry& GetSpecifiers() const { return Specifiers; }

        // Writes the built-in outputs first, then runs each loaded generator under Timeout and writes its outputs only if it succeeded.
        void Run(const FReflectionModel& Model, std::chrono::seconds Timeout);

    private:

        struct FLoadedGenerator
        {
            std::unique_ptr<IReflectionGenerator> Generator;
            std::string                           RulesFile;
        };

        void Claim(IReflectionGenerator& Generator, const FDiagLocation& Where);

        std::vector<std::unique_ptr<IReflectionGenerator>> Generators;
        std::vector<FLoadedGenerator>                      Loaded;
        FSpecifierRegistry                                 Specifiers;
    };
}
