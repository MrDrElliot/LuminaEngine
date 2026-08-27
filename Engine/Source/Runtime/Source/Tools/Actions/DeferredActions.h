#pragma once

#include "World/ECS/Registry.h"


#include <Core/Templates/LuminaTemplate.h>

namespace Lumina
{
	class FDeferredActionRegistry
	{
	public:

		template<typename T, typename ... TArgs>
		T& EnqueueAction(TArgs&&... Args)
		{
			ECS::FEntity Entity = Registry.Create();
			return Registry.Emplace<T>(Entity, Forward<TArgs>(Args)...);
		}

		template<typename ... Ts, typename TFunc>
		void ProcessAllOf(TFunc&& Func)
		{
			Registry.View<Ts...>().ForEach(Forward<TFunc>(Func));
			Registry.ClearComponent<Ts...>();
		}


	private:

		ECS::FRegistry Registry;
	};
}
