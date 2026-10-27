#pragma once

#include "WeatherLayer.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "WeatherPreset.generated.h"

namespace Lumina
{
    REFLECT(ToolTip = "A complete weather type built from layers, such as a drizzle, a storm or a clear day.")
    class RUNTIME_API CWeatherPreset : public CObject
    {
        GENERATED_BODY()

    public:

        bool IsAsset() const override { return true; }

        // Later layers override earlier ones of the same kind.
        FWeatherParams Resolve() const;

        PROPERTY(Editable, Category = "Weather", ToolTip = "Rain, snow, cloud and wind layers that make up this weather.")
        TVector<TStrongObjectPtr<CWeatherLayer>> Layers;
    };
}
