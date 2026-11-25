#include "Skyrim/Render/SpaceLighting.h"

#include "Skyrim/Engine/Fog.h"
#include "Skyrim/Render/Backend.h"

namespace Render::SpaceLighting
{
	namespace
	{
		// Whether the interior takes a_what from its lighting template instead of its own data.
		bool Inherits(RE::TESObjectCELL* a_cell, RE::INTERIOR_DATA::Inherit a_what)
		{
			return a_cell->GetRuntimeData().lightingTemplate && a_cell->GetLighting()->lightingTemplateInheritanceFlags.any(a_what);
		}

		const RE::INTERIOR_DATA& Interior(RE::TESObjectCELL* a_cell, RE::INTERIOR_DATA::Inherit a_what)
		{
			return Inherits(a_cell, a_what) ? a_cell->GetRuntimeData().lightingTemplate->data : *a_cell->GetLighting();
		}

		// A space's own colours, through the engine (and Community Shaders' hook on it, which adjusts
		// them as it does for the engine's own calls).
		void ApplyAmbient(const AmbientState& a_state)
		{
			static REL::Relocation<void (*)(const float*, const float*, float)> setAmbient{ REL::RelocationID(98989, 105643) };
			setAmbient(&a_state.colors[0][0][0], a_state.tint, a_state.fresnel);
		}

		std::pair<std::byte*, std::byte*> AmbientMemory()
		{
			static const auto memory = [] {
				auto* inputs = reinterpret_cast<std::byte*>(REL::RelocationID(527705, 414646).address());
				auto* built = reinterpret_cast<std::byte*>(REL::RelocationID(513243, 390984).address());
				// The layout read off the function (AE 1.7.104): both blocks contiguous.
				SEAMLESS_ASSERT(REL::RelocationID(527716, 414657).address() - REL::RelocationID(527705, 414646).address() == 0x44);
				SEAMLESS_ASSERT(REL::RelocationID(513259, 391000).address() - REL::RelocationID(513243, 390984).address() == 0x40);
				return std::pair{ inputs, built };
			}();
			return memory;
		}

		// The directional ambient transform ID 105643 builds from six colours, worked out the same way
		// without calling it: a room merged into the world got its own by setting the engine's global
		// ambient to the room's for a moment, through Community Shaders' hook, and the outdoors
		// flashed white when a room was merged in. K is the engine's globals ID 410515..410517.
		Core::Transform AmbientTransform(const AmbientState& a_state)
		{
			static REL::Relocation<const float*> offsets{ REL::RelocationID(523935, 410515) };
			SEAMLESS_ASSERT(REL::RelocationID(523937, 410517).address() - REL::RelocationID(523935, 410515).address() == 8);  // three floats in a row
			return Core::Lighting::AmbientTransform(a_state, offsets.get());
		}

	}

	RE::NiTransform& EngineAmbientTransform()
	{
		return *reinterpret_cast<RE::NiTransform*>(AmbientMemory().second);
	}

	AmbientSnapshot SaveAmbient()
	{
		const auto [inputs, built] = AmbientMemory();
		AmbientSnapshot snapshot;
		std::memcpy(snapshot.inputs.data(), inputs, snapshot.inputs.size());
		std::memcpy(snapshot.built.data(), built, snapshot.built.size());
		return snapshot;
	}

	void RestoreAmbient(const AmbientSnapshot& a_snapshot)
	{
		const auto [inputs, built] = AmbientMemory();
		std::memcpy(inputs, a_snapshot.inputs.data(), a_snapshot.inputs.size());
		std::memcpy(built, a_snapshot.built.data(), a_snapshot.built.size());
	}

	namespace
	{
		// The cell's directional ambient (DALC), from its lighting template when it inherits ambient.
		std::optional<AmbientState> CellAmbient(RE::TESObjectCELL* a_cell)
		{
			auto* own = a_cell ? a_cell->GetLighting() : nullptr;
			if (!own) {
				return std::nullopt;
			}
			const auto& dalc = Inherits(a_cell, RE::INTERIOR_DATA::Inherit::kAmbientColor) ? a_cell->GetRuntimeData().lightingTemplate->directionalAmbientLightingColors :
			                                                                                 own->directionalAmbientLightingColors;

			auto toFloat = [](const RE::Color& a_color, float* a_out) {
				a_out[0] = a_color.red / 255.0f;
				a_out[1] = a_color.green / 255.0f;
				a_out[2] = a_color.blue / 255.0f;
			};
			AmbientState state{};
			const RE::BGSDirectionalAmbientLightingColors::Directional::MaxMin<RE::Color>* axes[3] = { &dalc.directional.x, &dalc.directional.y, &dalc.directional.z };
			for (int axis = 0; axis < 3; ++axis) {
				toFloat(axes[axis]->max, state.colors[axis][0]);
				toFloat(axes[axis]->min, state.colors[axis][1]);
			}
			toFloat(dalc.specular, state.tint);
			state.fresnel = dalc.fresnelPower;
			return state;
		}

		void SetLightDirection(RE::NiLight* a_light, RE::NiPoint3 a_direction)
		{
			// A directional light shines along its local +X.
			a_direction.Unitize();
			RE::NiPoint3 side = a_direction.UnitCross({ 0.0f, 0.0f, 1.0f });
			if (side.Length() < 0.5f) {
				side = { 1.0f, 0.0f, 0.0f };
			}
			const RE::NiPoint3 up = side.UnitCross(a_direction);
			auto&              m = a_light->local.rotate;
			for (int row = 0; row < 3; ++row) {
				m.entry[row][0] = a_direction[row];
				m.entry[row][1] = side[row];
				m.entry[row][2] = up[row];
			}
			RE::NiUpdateData update;
			a_light->Update(update);
		}

	}
}

namespace Render::SunDirections
{
	namespace
	{
		// Interiors (sky in interior mode): rotation Rz(XY) * Ry(Z), the angles in degrees from the
		// cell's own getters (ID 18929 / 18931: its lighting data, or its lighting template's when it
		// inherits the rotation).
		RE::NiPoint3 FormulaDirection(RE::TESObjectCELL* a_cell)
		{
			static REL::Relocation<std::uint32_t (*)(RE::TESObjectCELL*)> rotationXY{ REL::RelocationID(18487, 18929) };
			static REL::Relocation<std::uint32_t (*)(RE::TESObjectCELL*)> rotationZ{ REL::RelocationID(18488, 18931) };
			const float xy = RE::deg_to_rad(static_cast<float>(rotationXY(a_cell)));
			const float z = RE::deg_to_rad(static_cast<float>(rotationZ(a_cell)));
			return { std::cos(xy) * std::cos(z), -std::sin(xy) * std::cos(z), std::sin(z) };
		}

		// Interiors' directional light as the engine set it (the world light scene's sun) while the
		// player was in them, by cell, for this session (Observe).
		std::unordered_map<RE::FormID, RE::NiPoint3> seenDirections;
	}

	RE::NiPoint3 OfCell(RE::TESObjectCELL* a_cell)
	{
		const auto it = seenDirections.find(a_cell->GetFormID());
		return it != seenDirections.end() ? it->second : FormulaDirection(a_cell);
	}

	// Outdoors (sky in exterior mode): the sun's place on its arc, from the hour against the
	// climate's sunrise and sunset (their middles, widened by half of fSunAlphaTransTime on each
	// side): from east (+1) to west (-1) by day, and back by night. The light travels along
	// (-fSunDirXExtreme x, -fSunDirXExtreme * fSunYExtreme / fSunXExtreme, fSunZExtreme), the
	// settings read where Sun::Update reads them. a_synced: Sky Sync's direction (below), which is
	// the light outdoors.
	RE::NiPoint3 Outdoors(bool* a_synced)
	{
		auto* sky = RE::Sky::GetSingleton();
		auto* calendar = RE::Calendar::GetSingleton();
		SEAMLESS_ASSERT(sky && calendar);  // only asked for with a game running
		static REL::Relocation<const float*> alphaTransTime{ REL::RelocationID(502619, 370247) };
		static REL::Relocation<const float*> xExtreme{ REL::RelocationID(502621, 370250) };
		static REL::Relocation<const float*> yExtreme{ REL::RelocationID(502623, 370253) };
		static REL::Relocation<const float*> zExtreme{ REL::RelocationID(502625, 370256) };
		static REL::Relocation<const float*> dirXExtreme{ REL::RelocationID(502627, 370259) };
		// The engine reads the sky's own hour, which may stand still while the sky is in interior
		// mode; the calendar's is the same hour, always current.
		const float hour = calendar->GetHour();
		const float day = RE::Calendar::GetHoursPerDay();
		const float widen = 0.5f * *alphaTransTime;
		const float rise = 0.5f * (sky->GetSunriseBegin() + sky->GetSunriseEnd()) - widen;
		const float set = 0.5f * (sky->GetSunsetBegin() + sky->GetSunsetEnd()) + widen;
		const float x = Core::Lighting::SunArc(hour, day, rise, set);
		// Community Shaders' Sky Sync turns the light toward the sun's place in the sky (the
		// position Sun::Update gives it, from the same x) instead: ~13 degrees steeper by day than
		// the light above, and the street's shadows seen from indoors fell shorter than outside.
		if (a_synced) {
			*a_synced = false;
		}
		const float position[3] = { x * *xExtreme, *yExtreme, std::abs(*xExtreme) - std::abs(x * *xExtreme) };
		float       towardSun[3]{};
		if (::Render::Backend::Current().OutdoorSunDirection(hour, position, towardSun)) {
			if (a_synced) {
				*a_synced = true;
			}
			return RE::NiPoint3{ -towardSun[0], -towardSun[1], -towardSun[2] };
		}
		RE::NiPoint3 direction{ -*dirXExtreme * x, -*dirXExtreme * *yExtreme / *xExtreme, *zExtreme };
		direction.Unitize();
		return direction;
	}

	void Observe()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cell = player ? player->GetParentCell() : nullptr;
		auto* scene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
		auto* sun = scene ? scene->GetRuntimeData().sunLight : nullptr;
		auto* light = sun ? sun->light.get() : nullptr;
		if (!cell || !light) {
			return;
		}
		static RE::FormID                            current = 0;
		static bool                                  checked = false;
		static std::chrono::steady_clock::time_point since;
		const auto                                   now = std::chrono::steady_clock::now();
		if (cell->GetFormID() != current) {
			current = cell->GetFormID();
			checked = false;
			since = now;
			return;
		}
		if (checked || now - since < std::chrono::seconds(1)) {
			return;
		}
		checked = true;
		const auto&        r = light->world.rotate;
		const RE::NiPoint3 engine{ r.entry[0][0], r.entry[1][0], r.entry[2][0] };
		const auto         ours = cell->IsInteriorCell() ? FormulaDirection(cell) : Outdoors();
		if (cell->IsInteriorCell()) {
			seenDirections.insert_or_assign(current, engine);
		}
		const float        apart = RE::rad_to_deg(std::acos(std::clamp(engine.Dot(ours), -1.0f, 1.0f)));
		static int         reported = 0;
		if (apart > 2.0f && reported++ < 20) {
			logger::warn("portal: sun direction in {:08X} is ({:.3f},{:.3f},{:.3f}), computed ({:.3f},{:.3f},{:.3f}): {:.1f} degrees apart", current, engine.x, engine.y,
				engine.z, ours.x, ours.y, ours.z, apart);
		} else if (apart <= 2.0f) {
			static bool interiorConfirmed = false, exteriorConfirmed = false;
			if (!std::exchange(cell->IsInteriorCell() ? interiorConfirmed : exteriorConfirmed, true)) {
				logger::info("portal: computed sun direction matches the engine's {} ({:.2f} degrees apart)", cell->IsInteriorCell() ? "indoors" : "outdoors", apart);
			}
		}
	}
}

namespace Render::SpaceLighting
{
	namespace
	{
		void ApplyCellDirectional(RE::ShadowSceneNode* a_scene, RE::TESObjectCELL* a_cell)
		{
			auto* own = a_cell ? a_cell->GetLighting() : nullptr;
			auto* sun = a_scene->GetRuntimeData().sunLight;
			auto* light = sun ? sun->light.get() : nullptr;
			if (!own || !light) {
				return;
			}
			const auto& color = Interior(a_cell, RE::INTERIOR_DATA::Inherit::kDirectionalColor).directional;
			auto&       runtime = light->GetLightRuntimeData();
			runtime.diffuse = { color.red / 255.0f, color.green / 255.0f, color.blue / 255.0f };
			runtime.fade = Interior(a_cell, RE::INTERIOR_DATA::Inherit::kDirectionalFade).directionalFade;
			SetLightDirection(light, SunDirections::OfCell(a_cell));
		}

	}

	std::optional<Outdoors> WeatherLighting()
	{
		auto* sky = RE::Sky::GetSingleton();
		auto* current = sky ? sky->currentWeather : nullptr;
		if (!current) {
			return std::nullopt;
		}
		auto*                         last = sky->lastWeather;
		RE::Sky::COLOR_BLEND          blend{};
		RE::TESWeather::ColorTime     time1{};
		RE::TESWeather::ColorTime     time2{};
		sky->FillColorBlend(blend, current, sky->currentWeatherPct, time1, time2);
		const auto color = [&](RE::TESWeather::ColorType a_type) {
			RE::Sky::COLOR_BLEND colors = blend;
			sky->FillColorBlendColors(colors, current, last, a_type, time1, time2);
			if (!last) {
				colors.RGBVal[3] = RE::Color();
			}
			RE::NiColor result;
			sky->SetColor(result, &colors, 0.0f);
			return result;
		};
		Outdoors result;
		result.sun = color(RE::TESWeather::ColorTypes::kSunlight);
		result.sky = { color(RE::TESWeather::ColorTypes::kSkyUpper), color(RE::TESWeather::ColorTypes::kHorizon), color(RE::TESWeather::ColorTypes::kSkyLower) };

		// The directional ambient the same way (ID 26252, which also hands it to the renderer, so it
		// isn't called): current weather at the two times, then the previous weather's.
		const auto mix = [&](auto a_get) {
			float out[3] = { 0.0f, 0.0f, 0.0f };
			const auto add = [&](RE::TESWeather* a_weather, RE::TESWeather::ColorTime a_time, float a_weight) {
				const RE::Color& c = a_get(a_weather->directionalAmbientLightingColors[a_time]);
				out[0] += c.red / 255.0f * a_weight;
				out[1] += c.green / 255.0f * a_weight;
				out[2] += c.blue / 255.0f * a_weight;
			};
			add(current, time1, blend.blend[0]);
			add(current, time2, blend.blend[1]);
			if (last) {
				add(last, time1, blend.blend[2]);
				add(last, time2, blend.blend[3]);
			}
			return std::array{ out[0], out[1], out[2] };
		};
		using DALC = RE::BGSDirectionalAmbientLightingColors;
		const auto store = [](const std::array<float, 3>& a_from, float* a_to) { std::copy(a_from.begin(), a_from.end(), a_to); };
		store(mix([](const DALC& d) -> const RE::Color& { return d.directional.x.max; }), result.ambient.colors[0][0]);
		store(mix([](const DALC& d) -> const RE::Color& { return d.directional.x.min; }), result.ambient.colors[0][1]);
		store(mix([](const DALC& d) -> const RE::Color& { return d.directional.y.max; }), result.ambient.colors[1][0]);
		store(mix([](const DALC& d) -> const RE::Color& { return d.directional.y.min; }), result.ambient.colors[1][1]);
		store(mix([](const DALC& d) -> const RE::Color& { return d.directional.z.max; }), result.ambient.colors[2][0]);
		store(mix([](const DALC& d) -> const RE::Color& { return d.directional.z.min; }), result.ambient.colors[2][1]);
		store(mix([](const DALC& d) -> const RE::Color& { return d.specular; }), result.ambient.tint);
		result.ambient.fresnel = current->directionalAmbientLightingColors[time1].fresnelPower * blend.blend[0] +
		                         current->directionalAmbientLightingColors[time2].fresnelPower * blend.blend[1] +
		                         (last ? last->directionalAmbientLightingColors[time1].fresnelPower * blend.blend[2] +
		                                     last->directionalAmbientLightingColors[time2].fresnelPower * blend.blend[3] :
		                                 0.0f);

		// The fog's distances from each weather's day or night values, by the same weights.
		result.fogNearColor = color(RE::TESWeather::ColorTypes::kFogNear);
		result.fogFarColor = color(RE::TESWeather::ColorTypes::kFogFar);
		result.fogNear = result.fogFar = result.fogPower = result.fogMax = 0.0f;
		const auto addFog = [&](RE::TESWeather* a_weather, RE::TESWeather::ColorTime a_time, float a_weight) {
			const auto& fog = a_weather->fogData;
			const bool  night = a_time == RE::TESWeather::ColorTimes::kNight;
			result.fogNear += (night ? fog.nightNear : fog.dayNear) * a_weight;
			result.fogFar += (night ? fog.nightFar : fog.dayFar) * a_weight;
			result.fogPower += (night ? fog.nightPower : fog.dayPower) * a_weight;
			result.fogMax += (night ? fog.nightMax : fog.dayMax) * a_weight;
		};
		addFog(current, time1, blend.blend[0]);
		addFog(current, time2, blend.blend[1]);
		if (last) {
			addFog(last, time1, blend.blend[2]);
			addFog(last, time2, blend.blend[3]);
		}
		return result;
	}

	namespace
	{
		struct OutdoorSnapshot
		{
			RE::NiColor         sun;
			float               sunFade = 1.0f;
			RE::NiPoint3        sunDirection;
			AmbientSnapshot     ambient{};
			SkyColors           sky{};
			float               gameHour = 0.0f;
			RE::TESWorldSpace*  world = nullptr;
			bool                valid = false;
		};
		OutdoorSnapshot outdoorSnapshot;

		const OutdoorSnapshot* RecentOutdoors(RE::TESObjectCELL* a_cell)
		{
			auto* sky = RE::Sky::GetSingleton();
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* here = player ? player->GetParentCell() : nullptr;
			const bool outdoorsNow = here && !here->IsInteriorCell();
			if (!outdoorSnapshot.valid || !sky || !a_cell || (!outdoorsNow && a_cell->GetRuntimeData().worldSpace != outdoorSnapshot.world)) {
				return nullptr;
			}
			float age = sky->currentGameHour - outdoorSnapshot.gameHour;
			if (age < 0.0f) {
				age += 24.0f;
			}
			return age < 0.5f ? &outdoorSnapshot : nullptr;
		}

	}

	void CaptureOutdoors()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cell = player ? player->GetParentCell() : nullptr;
		auto* scene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
		auto* sky = RE::Sky::GetSingleton();
		if (!cell || cell->IsInteriorCell() || !scene || !sky) {
			return;
		}
		auto* sun = scene->GetRuntimeData().sunLight;
		auto* light = sun ? sun->light.get() : nullptr;
		if (!light) {
			return;
		}
		OutdoorSnapshot snapshot;
		snapshot.sun = light->GetLightRuntimeData().diffuse;
		snapshot.sunFade = light->GetLightRuntimeData().fade;
		const auto& r = light->world.rotate;
		snapshot.sunDirection = { r.entry[0][0], r.entry[1][0], r.entry[2][0] };  // along its local +X
		snapshot.ambient = SaveAmbient();
		using Color = RE::TESWeather::ColorTypes;
		snapshot.sky = { sky->skyColor[Color::kSkyUpper], sky->skyColor[Color::kHorizon], sky->skyColor[Color::kSkyLower] };
		snapshot.gameHour = sky->currentGameHour;
		snapshot.world = cell->GetRuntimeData().worldSpace;
		snapshot.valid = true;
		outdoorSnapshot = snapshot;
	}

	namespace
	{
		// The way the light travels outdoors: Sky Sync's direction when it gives one, which moves with
		// the hour; otherwise the snapshot's, which stands still from the moment the player came in
		// (half an hour old at most); otherwise the computed one. The same for the portal pass and the
		// merged street, or the shadows jump at the hand-over.
		RE::NiPoint3 OutdoorLightDirection(const OutdoorSnapshot* a_recent)
		{
			bool       synced = false;
			const auto computed = SunDirections::Outdoors(&synced);
			return synced || !a_recent ? computed : a_recent->sunDirection;
		}

		// A space's light for a room merged into the world: its ambient turned from the space's own
		// coordinates into the world's by a_rotation. ambient(n_world) = M * n_cell + t with
		// n_cell = R^T n_world, so M' = M R^T.
		Render::SpaceLight Light(const RE::NiColor& a_sun, const RE::NiPoint3& a_towardSun, const Core::Transform& a_ambient, const RE::NiMatrix3& a_rotation)
		{
			Render::SpaceLight light;
			light.sun = ToCore(a_sun);
			light.towardSun = ToCore(a_towardSun);
			light.ambientRotate = a_ambient.rotate * ToCore(a_rotation).Transpose();
			light.ambientTranslate = a_ambient.translate;
			return light;
		}
	}

	std::optional<SkyColors> ApplyRemoteLighting(RE::ShadowSceneNode* a_scene, RE::TESObjectCELL* a_cell)
	{
		std::optional<AmbientState> ambient;
		std::optional<SkyColors>    background;
		auto*                       sun = a_scene->GetRuntimeData().sunLight;
		auto*                       light = sun ? sun->light.get() : nullptr;
		if (a_cell->IsInteriorCell()) {
			ApplyCellDirectional(a_scene, a_cell);
			ambient = CellAmbient(a_cell);
		} else if (const auto* recent = RecentOutdoors(a_cell)) {
			if (light) {
				// Without the image space's HDR sunlight scale (2.5 outdoors) the world's sun gets: with
				// it, and no shadows on the street, the view out of a dark room was far too bright.
				light->GetLightRuntimeData().diffuse = recent->sun;
				light->GetLightRuntimeData().fade = recent->sunFade;
				SetLightDirection(light, OutdoorLightDirection(recent));
			}
			// The engine's own state, as it was: not through the engine's function again.
			RestoreAmbient(recent->ambient);
			return recent->sky;
		} else if (const auto outdoors = WeatherLighting()) {
			if (light) {
				light->GetLightRuntimeData().diffuse = outdoors->sun;
				light->GetLightRuntimeData().fade = 1.0f;
				SetLightDirection(light, SunDirections::Outdoors());
			}
			ambient = outdoors->ambient;
			background = outdoors->sky;
		}
		if (ambient) {
			ApplyAmbient(*ambient);
		}
		return background;
	}

	std::optional<Render::SpaceLight> MergedLighting(RE::TESObjectCELL* a_cell, const RE::NiMatrix3& a_rotation)
	{
		if (a_cell && !a_cell->IsInteriorCell()) {
			// The street merged into an interior: lit as the engine lit it the last time the player
			// was out (the sky is in interior mode now and computes no sun for it).
			// The colours from the weather, worked out as the sky does (equal to the engine's own to
			// 1e-7, compared outdoors): the snapshot alone left the street unlit, as dark as the room
			// in broad daylight, after loading a game indoors or half an hour of game time inside.
			const auto weather = WeatherLighting();
			if (!weather) {
				return std::nullopt;
			}
			const auto sunDirection = OutdoorLightDirection(RecentOutdoors(a_cell));
			return Light(weather->sun, a_rotation * (sunDirection * -1.0f), AmbientTransform(weather->ambient), a_rotation);
		}
		auto*      own = a_cell && a_cell->IsInteriorCell() ? a_cell->GetLighting() : nullptr;
		const auto ambient = CellAmbient(a_cell);
		if (!own || !ambient) {
			return std::nullopt;
		}
		const auto& color = Interior(a_cell, RE::INTERIOR_DATA::Inherit::kDirectionalColor).directional;
		const float fade = Interior(a_cell, RE::INTERIOR_DATA::Inherit::kDirectionalFade).directionalFade;
		// Same direction as ApplyCellDirectional (the way the light travels), turned around.
		return Light({ color.red / 255.0f * fade, color.green / 255.0f * fade, color.blue / 255.0f * fade }, a_rotation * -SunDirections::OfCell(a_cell), AmbientTransform(*ambient),
			a_rotation);
	}

	std::optional<Render::SpaceFog> MergedFog()
	{
		const auto weather = WeatherLighting();
		if (!weather || weather->fogFar <= weather->fogNear) {
			return std::nullopt;
		}
		const auto& w = *weather;
		return Render::SpaceFog{ w.fogNear, w.fogFar, w.fogPower, w.fogMax, ToCore(w.fogNearColor), ToCore(w.fogFarColor) };
	}

	std::optional<Render::SpaceFog> CellFog(RE::TESObjectCELL* a_cell)
	{
		auto* own = a_cell && a_cell->IsInteriorCell() ? a_cell->GetLighting() : nullptr;
		if (!own) {
			return std::nullopt;
		}
		using Inherit = RE::INTERIOR_DATA::Inherit;
		const float fogNear = Interior(a_cell, Inherit::kFogNear).fogNear;
		const float fogFar = Interior(a_cell, Inherit::kFogFar).fogFar;
		const auto& colors = Interior(a_cell, Inherit::kFogColor);
		// No fog at all (the engine's own "no fog" when both distances are 0): a most-it-covers of 0.
		const float most = fogFar > fogNear ? Interior(a_cell, Inherit::kFogMax).fogClamp : 0.0f;
		return Render::SpaceFog{ fogNear, std::max(fogFar, fogNear + 1.0f), Interior(a_cell, Inherit::kFogPower).fogPower, most,
			{ colors.fogColorNear.red / 255.0f, colors.fogColorNear.green / 255.0f, colors.fogColorNear.blue / 255.0f },
			{ colors.fogColorFar.red / 255.0f, colors.fogColorFar.green / 255.0f, colors.fogColorFar.blue / 255.0f } };
	}

	std::optional<RE::NiColor> OutdoorAmbient(const RE::NiPoint3& a_normal)
	{
		const auto weather = WeatherLighting();
		if (!weather) {
			return std::nullopt;
		}
		const auto ambient = AmbientTransform(weather->ambient);
		const auto color = ambient.rotate * ToCore(a_normal) + ambient.translate;
		return RE::NiColor{ std::max(color.x, 0.0f), std::max(color.y, 0.0f), std::max(color.z, 0.0f) };
	}

	json OutdoorLightingProbe()
	{
		const auto Rgb = [](const RE::NiColor& a_color) { return json::array({ a_color.red, a_color.green, a_color.blue }); };
		const auto Transform = [](const Core::Transform& a_t) {
			json rows = json::array();
			for (int row = 0; row < 3; ++row) {
				rows.push_back({ a_t.rotate.entry[row][0], a_t.rotate.entry[row][1], a_t.rotate.entry[row][2], a_t.translate[row] });
			}
			return rows;
		};
		json result;
		if (const auto weather = WeatherLighting()) {
			result["weather"] = { { "sun", Rgb(weather->sun) }, { "ambient", Transform(AmbientTransform(weather->ambient)) }, { "skyUpper", Rgb(weather->sky.upper) },
				{ "fog", { weather->fogNear, weather->fogFar, weather->fogPower, weather->fogMax } }, { "fogNearColor", Rgb(weather->fogNearColor) },
				{ "fogFarColor", Rgb(weather->fogFarColor) } };
			if (auto* sky = RE::Sky::GetSingleton()) {
				result["skyFog"] = { sky->fogNear, sky->fogFar, sky->fogPower, sky->fogClamp };
			}
		}
		if (outdoorSnapshot.valid) {
			RE::NiTransform built;
			std::memcpy(&built, outdoorSnapshot.ambient.built.data(), sizeof(RE::NiTransform));
			const auto ambient = ToCore(built);
			result["snapshot"] = { { "sun", Rgb(outdoorSnapshot.sun) }, { "fade", outdoorSnapshot.sunFade }, { "ambient", Transform(ambient) }, { "hour", outdoorSnapshot.gameHour },
				{ "direction", { outdoorSnapshot.sunDirection.x, outdoorSnapshot.sunDirection.y, outdoorSnapshot.sunDirection.z } } };
		}
		const auto computed = SunDirections::Outdoors();
		result["computedDirection"] = { computed.x, computed.y, computed.z };
		return result;
	}
}
