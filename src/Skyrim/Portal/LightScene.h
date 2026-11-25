#pragma once

// A remote space's own light scene: a ShadowSceneNode that is also the private scene-graph root the
// space is attached under. Culling it from the portal camera assigns *its* lights to the space's
// geometry, and the native lighting reads the one registered in BSShaderManager::State slot Render::kPortalSceneSlot.
//
// Every space kept attached has a scene of its own (interiors share coordinates: two rooms in one
// scene would light each other), and only the shown one is registered.
namespace Portal::LightScene
{
	// A new scene to attach a space under, not registered (not drawn) until Show.
	[[nodiscard]] RE::NiPointer<RE::ShadowSceneNode> Create();

	// The scene registered in slot 3: the shown space's, or an empty one while none is shown.
	RE::ShadowSceneNode* Get();
	// a_scene registered in slot 3. nullptr: the empty one.
	void Show(RE::ShadowSceneNode* a_scene);
	[[nodiscard]] bool Shown(const RE::ShadowSceneNode* a_scene);

	// a_scene's space was released: its lights and object lists emptied, unregistered if shown,
	// and kept alive, never freed. Render passes cached on the released shapes keep pointing at
	// its sun light; freed after eight releases, a portal pass crashed in the lighting shader's
	// setup reading that light (BSLightingShader::SetupGeometry, ID 107300 + 0x595). Costs one light
	// scene per space attached.
	void Retire(RE::NiPointer<RE::ShadowSceneNode> a_scene);

	// a_scene's lights and object lists emptied.
	void Clear(RE::ShadowSceneNode* a_scene);

	// Any thread: a_top (the top of a scene graph) is one of ours, shown or not.
	[[nodiscard]] bool IsOurs(const RE::NiAVObject* a_top);

	// Moves the lights under a_cell3D (the remote cell) from the engine's light scenes into a_scene.
	// Cheap enough to run every frame, which also catches lights created after the attach.
	int Adopt(RE::ShadowSceneNode* a_scene, const RE::NiAVObject* a_cell3D);

	// The world scene's lights right now.
	[[nodiscard]] std::vector<RE::NiPointer<RE::BSLight>> WorldLights();

	// The engine culls and fades lights against the main camera only; for the portal view, and for a
	// room merged into the world (lit from this scene by the native lighting), every light of the
	// shown scene must count as visible and unfaded.
	void PrepareForView();

	json Describe(RE::ShadowSceneNode* a_scene, const RE::NiAVObject* a_cell3D);
	json DescribeAll(const RE::NiAVObject* a_cell3D);
}
