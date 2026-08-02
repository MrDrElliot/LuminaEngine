#pragma once

#include <entt/entt.hpp>
#include <Core/Templates/LuminaTemplate.h>

namespace Lumina
{
	class FDeferredActionRegistry
	{
	public:

		template<typename T, typename ... TArgs>
		T& EnqueueAction(TArgs&&... Args)
		{
			entt::entity Entity = Registry.create();
			return Registry.emplace<T>(Entity, Forward<TArgs>(Args)...);
		}

		template<typename ... Ts, typename TFunc>
		void ProcessAllOf(TFunc&& Func)
		{
			Registry.view<Ts...>().each(Forward<TFunc>(Func));
			Registry.clear<Ts...>();
		}


	private:

		entt::registry Registry;
	};
}