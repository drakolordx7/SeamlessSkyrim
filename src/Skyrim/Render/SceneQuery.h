#pragma once

// What the renderers ask of the portal code about the shapes they draw. Implemented and registered
// by Portal::Manager, so that the render code depends on this interface and not on the manager.
namespace Render
{
	class SceneQuery
	{
	public:
		// The shape belongs to a space merged into the world, and to the player's own.
		[[nodiscard]] virtual bool IsMergedShape(const RE::BSGeometry* a_shape) const = 0;
		[[nodiscard]] virtual bool IsPlayerShape(const RE::BSGeometry* a_shape) const = 0;
		[[nodiscard]] virtual const std::vector<RE::TESObjectCELL*>& RemoteCells() const = 0;

	protected:
		~SceneQuery() = default;
	};

	inline const SceneQuery* registeredScene = nullptr;

	inline void RegisterScene(const SceneQuery& a_scene)
	{
		registeredScene = &a_scene;
	}

	[[nodiscard]] inline const SceneQuery& Scene()
	{
		return *registeredScene;
	}
}
