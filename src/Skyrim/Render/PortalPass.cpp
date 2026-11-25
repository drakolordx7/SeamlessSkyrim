#include "Skyrim/Render/PortalPass.h"

#include "Skyrim/Diag/Telemetry.h"
#include "Skyrim/Doors/Geometry.h"
#include "Skyrim/Doors/Pair.h"
#include "Skyrim/Portal/Gates.h"
#include "Skyrim/Portal/LightScene.h"
#include "Skyrim/Portal/PortalManager.h"
#include "Skyrim/Render/Backend.h"
#include "Skyrim/Render/D3D.h"
#include "Skyrim/Render/SpaceLighting.h"

namespace Render::PortalPass
{
	namespace
	{
		using Portal::Active;
		using Render::ClipPlane;
		using SpaceLighting::SkyColors;

		// 0x400 = RENDER_FLAG::kUpdateViewport.
		// 0x4: without 0x4 or 0x80 one of the shaders' SetupGeometry (ID 107528) switches the depth mode
		// to "test EQUAL, no write", which only works after a depth prepass. The portal pass has none,
		// the mode then stuck for the rest of the pass and nothing was drawn whenever such an object was
		// in view (most views into the Sleeping Giant Inn: the doorway was black).
		// 0x200: the menu-3D renderer (ID 52727) passes it too. Without 0x200 or 0x80 the accumulator's
		// post-accumulation stage (ID 106584) runs a world-only step under some conditions and then
		// binds the world's main colour target for the rest of the pass, so the portal's blended and
		// effect geometry was drawn straight into the main picture.
		// 0x400: the viewport is the whole target. A picture for the doorway on screen leaves it out and
		// adds 0x1 (the main view's jitter) instead: see LiveRenderFlags.
		constexpr std::uint32_t kRenderFlags = 0x400 | 0x200 | 0x4;

		// The picture shown in the doorway is drawn like the main view: at its render resolution (an
		// upscaler or dynamic resolution draws the scene into the top-left part of its target) and with
		// its subpixel jitter, so the upscaler resolves the doorway with the scene around it. Drawn at
		// the full target size without jitter and point-sampled into the smaller scene, it showed
		// staircased edges and shimmering foliage the upscaler couldn't smooth.
		constexpr std::uint32_t LiveRenderFlags()
		{
			return (kRenderFlags & ~0x400u) | 0x1;
		}

		RE::NiPointer<RE::NiCamera> portalCamera;
		// Ref-counted owner: the culling context also holds a reference, and when it drops it the
		// accumulator would otherwise be freed.
		RE::NiPointer<RE::BSShaderAccumulator> portalAccumulator;
		float                                  defaultNear = 0.0f;

		// (AE 1.7.104, seen in ID 52727); CommonLib's wrappers pass a different object. SetColorTarget's
		// function ignores it on both runtimes (docs/ENGINE.md).
		void* RenderTargetManagerObject()
		{
			static REL::Relocation<std::uintptr_t> object{ REL::RelocationID(524970, 411451) };
			return reinterpret_cast<void*>(object.address());
		}

		void* RendererObject()
		{
			static REL::Relocation<std::uintptr_t> object{ REL::RelocationID(524907, 411393) };
			return reinterpret_cast<void*>(object.address());
		}

		void SetColorTarget(std::uint32_t a_slot, std::uint32_t a_target, std::uint32_t a_mode, bool a_updateViewport)
		{
			using func_t = void (*)(void*, std::uint32_t, std::uint32_t, std::uint32_t, bool);
			static REL::Relocation<func_t> func{ REL::RelocationID(75646, 77453) };
			func(RenderTargetManagerObject(), a_slot, a_target, a_mode, a_updateViewport);
		}

		void SetDepthTarget(std::uint32_t a_target, std::uint32_t a_mode, std::uint32_t a_slice)
		{
			using func_t = void (*)(void*, std::uint32_t, std::uint32_t, std::uint32_t, bool);
			static REL::Relocation<func_t> func{ REL::RelocationID(75647, 77454) };
			func(RenderTargetManagerObject(), a_target, a_mode, a_slice, false);
		}

		// Empties what a cull put into the accumulator. Without this, rendering every frame piled new
		// passes onto the previous frame's stale ones and crashed in the batch renderer. Mirrors the
		// cleanup Main::RenderPlayerView / RenderWorld do for the main accumulator.
		void ResetAccumulator(RE::BSShaderAccumulator* a_accumulator)
		{
			static REL::Relocation<void (*)(RE::BSShaderAccumulator*)>                clearItems{ REL::RelocationID(99952, 106597) };  // NiBackToFrontAccumulator::ClearItemsList
			static REL::Relocation<void (*)(RE::BSShaderAccumulator*, std::int32_t)> clearGroup{ REL::RelocationID(99955, 106600) };
			clearItems(a_accumulator);
			clearGroup(a_accumulator, 6);
			clearGroup(a_accumulator, 5);
		}

		// SetupAndDrawPass call in the pass loop of BSBatchRenderer::RenderBatches (ID 107642); CS
		// already redirects this call, so ours wraps theirs. Safety net for a crash that is not fully
		// understood yet: the portal pass has drawn render passes that were already returned to the
		// pool (shader gone), which crashed in the hooks further down. Skip those and say so.
		std::atomic_bool inPortalSubmit = false;
		std::uint32_t    skippedFreedPasses = 0;
		std::uint32_t    freedPassesThisSubmit = 0;
		// Passes drawn by the current portal submit. The batch renderer walks a group's passes along
		// BSRenderPass::passGroupNext (read after each draw, ID 107642 +0x294); a pass met twice means
		// the chain runs in a circle, and the game froze with the main thread drawing it for ever.
		std::unordered_set<const RE::BSRenderPass*> drawnThisSubmit;
		std::uint32_t                               cutCycles = 0;
		std::uint32_t                               portalFrame = 0;  // portal frames drawn for the current remote space
		bool                                        reportedPassCycle = false;

		// Reads a pointer without trusting the address it is read from.
		bool ReadPointer(const void* a_address, std::uintptr_t& a_value)
		{
			__try {
				a_value = *static_cast<const std::uintptr_t*>(a_address);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// Every real shader is one of the game's few singletons: once one has been seen with a vtable
		// in SkyrimSE.exe it is known, and anything else is checked without trusting it.
		bool ValidShader(const RE::BSShader* a_shader)
		{
			static const auto [begin, end] = [] {
				const auto& module = REL::Module::get();
				return std::pair{ module.base(), module.base() + module.segment(REL::Segment::rdata).offset() + module.segment(REL::Segment::rdata).size() };
			}();
			static std::vector<const RE::BSShader*> known;
			if (!a_shader) {
				return false;
			}
			if (std::ranges::find(known, a_shader) != known.end()) {
				return true;
			}
			std::uintptr_t vtable = 0;
			if (!ReadPointer(a_shader, vtable) || vtable < begin || vtable >= end) {
				return false;
			}
			if (known.size() < 64) {
				known.push_back(a_shader);
			}
			return true;
		}

		// A pass reached through a link, which may point at a pass returned to the pool: nothing of it
		// is trusted, not even that its address is mapped.
		bool PassLooksValid(const RE::BSRenderPass* a_pass)
		{
			std::uintptr_t shader = 0;
			return ReadPointer(&a_pass->shader, shader) && ValidShader(reinterpret_cast<const RE::BSShader*>(shader));
		}

		// The far door's shapes (see Draw): never drawn by the portal pass, whatever brings
		// them there, in case culling its root isn't enough.
		std::unordered_set<const void*> portalHiddenShapes;
		std::uint32_t                   portalHiddenDrawn = 0;

		// The far door's plane for the portal pass being drawn (unit normal away from the portal camera,
		// offset): what lies on the camera's side of it can't be seen through the doorway.
		std::optional<ClipPlane> farDoorClip;

		// The render backend cuts the portal pass at that plane per pixel, but only for some shaders
		// (the patched Community Shaders in its Lighting and Utility shaders; the native backend also
		// in effect shaders). The others (glow cards such as the city's window glows, particles, grass)
		// weren't cut: between the portal camera and the far doorway they were drawn into the picture,
		// more of them the closer the player got to the door. Shapes drawn with those that lie wholly
		// on the camera's side are left out (a cut would have removed all of them anyway).
		bool UncutShapeBeforeFarDoor(const RE::BSRenderPass* a_pass)
		{
			if (!farDoorClip || !a_pass->shader || !a_pass->geometry) {
				return false;
			}
			const auto type = a_pass->shader->shaderType.get();
			if (::Render::Backend::CutsPortalShader(type)) {
				return false;
			}
			const auto& bound = a_pass->geometry->worldBound;
			if (farDoorClip->Distance(ToCore(bound.center)) + bound.radius >= 0.0f) {
				return false;
			}
			static std::unordered_set<const RE::BSGeometry*> reported;
			if (reported.size() < 40 && reported.insert(a_pass->geometry).second) {
				logger::info("portal: '{}' (shader type {}) left out of the portal pass: before the far door, where its shader isn't cut", a_pass->geometry->name.c_str(),
					static_cast<int>(type));
			}
			return true;
		}

		struct DrawPassHook
		{
			static void Thunk(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags)
			{
				// A pass whose memory went back to the pool may still name a "shader" that is garbage
				// by now (its vtable 0: crashed walking out of the Sleeping Giant with the street
				// behind the door; or a pointer into nothing: crashed reading it).
				if (inPortalSubmit && a_pass && !PassLooksValid(a_pass)) {
					if (skippedFreedPasses++ < 200) {
						// Nothing of the pass is read: it may be memory reused for anything.
						logger::warn("portal: skipped a freed render pass ({:X}, technique {:X}) in portal frame {}", reinterpret_cast<std::uintptr_t>(a_pass), a_technique, portalFrame);
					}
					// Only a group's first pass gets here (the chains into freed passes are cut before
					// them, below), and the engine follows its link next. Freed passes can link back to
					// each other, and the loop then never ends (the game froze right after a crossing,
					// the main thread spinning here). Writing into a pass that may be someone else's by
					// now is the last resort against that hang, after more of them than any real list
					// holds.
					if (++freedPassesThisSubmit > 256) {
						a_pass->next = nullptr;
						a_pass->passGroupNext = nullptr;
						if (!std::exchange(reportedPassCycle, true)) {
							logger::error("portal: render pass list loops through freed passes; cut");
						}
					}
					return;
				}
				if (inPortalSubmit && a_pass && (portalHiddenShapes.contains(a_pass->geometry) || UncutShapeBeforeFarDoor(a_pass))) {
					portalHiddenDrawn += portalHiddenShapes.contains(a_pass->geometry) ? 1 : 0;
					CutBeforeFreed(a_pass);
					return;
				}
				if (inPortalSubmit && a_pass && !drawnThisSubmit.insert(a_pass).second) {
					a_pass->passGroupNext = nullptr;
					if (cutCycles++ < 20) {
						logger::error("portal: render pass {:X} drawn twice in one portal pass (the group's chain loops); chain cut", reinterpret_cast<std::uintptr_t>(a_pass));
					}
					return;
				}
				// Water (an exterior's river seen from inside) depends on the world pass's water state
				// (reflections, the water's own render targets), which this pass doesn't have. Left out
				// for now.
				if (inPortalSubmit && a_pass && a_pass->shader && a_pass->shader->shaderType.get() == RE::BSShader::Type::Water) {
					CutBeforeFreed(a_pass);
					return;
				}
				func(a_pass, a_technique, a_alphaTest, a_renderFlags);
				if (inPortalSubmit && a_pass) {
					CutBeforeFreed(a_pass);
				}
			}

			// The engine follows passGroupNext once this returns. Pointing into a freed pass, the
			// chain is ended here, on a pass known to be live, instead of on the freed one: what
			// follows it is drawn by the second portal pass (counted as freed, see Frame.cpp).
			static void CutBeforeFreed(RE::BSRenderPass* a_pass)
			{
				auto* next = a_pass->passGroupNext;
				if (next && !PassLooksValid(next)) {
					a_pass->passGroupNext = nullptr;
					++freedPassesThisSubmit;
					if (skippedFreedPasses++ < 200) {
						logger::warn("portal: chain cut before a freed render pass ({:X}) in portal frame {}", reinterpret_cast<std::uintptr_t>(next), portalFrame);
					}
				}
			}
			static inline REL::Relocation<decltype(Thunk)> func;
		};

		// Effect shaders that read the scene depth (soft particles and the like) unbind the depth
		// buffer while they draw and afterwards rebind "the world's depth target" (ID 107522 + 0x6A
		// asks ID 77457 which one that is). Inside the portal pass that put the main view's depth
		// buffer in place of the portal's for everything drawn after the first such effect.
		struct WorldDepthTargetHook
		{
			static std::uint32_t Thunk(void* a_renderTargetManager)
			{
				return inPortalSubmit ? static_cast<std::uint32_t>(kDepthTarget) : func(a_renderTargetManager);
			}
			static inline REL::Relocation<decltype(Thunk)> func;
		};

		// "Are there passes in this technique range?" as asked by the accumulator's draw routine
		// (ID 106583 + 0x3B4) for the range 0x5C00006D..70: the refraction-style effects (heat haze
		// over a fire pit). If so, the routine rebinds the *world's* render targets to draw them and
		// leaves them bound. Inside the portal pass that put parts of the interior straight into the
		// main picture; the portal does without those effects instead.
		struct RefractionPassesHook
		{
			static bool Thunk(RE::BSBatchRenderer* a_batch, std::uint32_t a_first, std::uint32_t a_last)
			{
				return inPortalSubmit ? false : func(a_batch, a_first, a_last);
			}
			static inline REL::Relocation<decltype(Thunk)> func;
		};

		// The weather's precipitation is one particle shape (Sky::precip, under the world scene's
		// "Weather" node) that the engine keeps centred on the camera. Rain writes no depth, so the
		// doorway's picture, put over the main view after the world is drawn, covered the rain in front
		// of a city gate too: it rained up to the gateway's outline and not in it. Both sides of a gate
		// share the weather and the coordinates, so the main view's own rain is drawn into the picture,
		// with the main camera and against the main view's depth: in the doorway it is then exactly the
		// rain the picture covers. (The far side's rain drawn from the portal camera was only the far,
		// thin streaks: its near plane stands at the far gate.)
		void DrawMainPrecipitation(RE::ShadowSceneNode* a_root)
		{
			// Not cut at the far door: this rain is the main view's, in the main view's coordinates.
			::Render::Backend::EndPortalPass();
			::Render::Backend::SetPortalClip(std::nullopt);
			auto* sun = a_root->GetRuntimeData().sunLight;
			auto* camera = RE::Main::WorldRootCamera();
			::Render::Backend::BeginPortalPass(sun ? skyrim_cast<RE::NiDirectionalLight*>(sun->light.get()) : nullptr, false, camera);
			auto* precip = RE::Sky::GetSingleton()->precip;
			ResetAccumulator(portalAccumulator.get());
			auto& data = portalAccumulator->GetRuntimeData();
			data.activeShadowSceneNode = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
			data.eyePosition = camera->world.translate;
			for (auto* shape : { precip->currentPrecip.get(), precip->lastPrecip.get() }) {
				if (shape && !shape->GetAppCulled()) {
					RE::BSCullingProcess::CullingContext context;
					context.accumulator.reset(portalAccumulator.get());
					context.camera.reset(camera);
					context.scene.reset(shape);
					RE::BSCullingProcess::Process(context);
				}
			}
			SetDepthTarget(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN, RE::BSGraphics::SRTM_NO_CLEAR, 0);
			inPortalSubmit = true;
			// In the live picture's viewport, pixel for pixel over the main view's depth.
			RE::BSGraphics::Renderer::SubmitAccumulator(camera, portalAccumulator.get(), LiveRenderFlags());
			inPortalSubmit = false;
			SetDepthTarget(kDepthTarget, RE::BSGraphics::SRTM_NO_CLEAR, 0);
		}

		struct SkyPipeline
		{
			ID3D11VertexShader*      vertexShader = nullptr;
			ID3D11PixelShader*       pixelShader = nullptr;
			ID3D11Buffer*            constants = nullptr;
			ID3D11DepthStencilState* depthState = nullptr;
			ID3D11RasterizerState*   rasterState = nullptr;
			ID3D11BlendState*        blendState = nullptr;
		} sky;

		struct alignas(16) SkyConstants
		{
			float forward[4];
			float right[4];  // scaled by the frustum's half width at distance 1
			float up[4];     // scaled by its half height
			float upper[4];
			float horizon[4];
			float lower[4];
			float size[4];
		};


		// Our own shaders and states: one that can't be made is our bug, not a state to draw around.
		void EnsureSkyPipeline(ID3D11Device* a_device)
		{
			if (sky.vertexShader) {
				return;
			}
			static const char source[] = R"(
				cbuffer Sky : register(b0) { float4 Forward; float4 Right; float4 Up; float4 Upper; float4 Horizon; float4 Lower; float4 Size; };
				float4 VS(uint id : SV_VertexID) : SV_Position { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }
				float4 PS(float4 position : SV_Position) : SV_Target {
					float2 ndc = float2(position.x / Size.x * 2 - 1, 1 - position.y / Size.y * 2);
					float3 direction = normalize(Forward.xyz + ndc.x * Right.xyz + ndc.y * Up.xyz);
					float  elevation = direction.z;
					float3 color = elevation >= 0 ? lerp(Horizon.rgb, Upper.rgb, saturate(pow(elevation, 0.6))) : lerp(Horizon.rgb, Lower.rgb, saturate(-elevation * 4));
					return float4(color, 1);
				}
			)";
			std::string errors;
			ID3DBlob*   vs = D3D::Compile(source, "SeamlessPortalSky", "VS", "vs_5_0", 0, &errors);
			ID3DBlob*   ps = D3D::Compile(source, "SeamlessPortalSky", "PS", "ps_5_0", 0, &errors);
			if (!vs || !ps) {
				logger::critical("portal: SeamlessPortalSky failed: {}", errors);
			}
			SEAMLESS_ASSERT(vs && ps);
			D3D11_BUFFER_DESC        buffer{ sizeof(SkyConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
			D3D11_DEPTH_STENCIL_DESC depth{};  // no depth test or write
			D3D11_RASTERIZER_DESC    raster{};
			raster.FillMode = D3D11_FILL_SOLID;
			raster.CullMode = D3D11_CULL_NONE;
			D3D11_BLEND_DESC blend{};
			blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			const bool made = SUCCEEDED(a_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &sky.vertexShader)) &&
			                  SUCCEEDED(a_device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &sky.pixelShader)) &&
			                  SUCCEEDED(a_device->CreateBuffer(&buffer, nullptr, &sky.constants)) && SUCCEEDED(a_device->CreateDepthStencilState(&depth, &sky.depthState)) &&
			                  SUCCEEDED(a_device->CreateRasterizerState(&raster, &sky.rasterState)) && SUCCEEDED(a_device->CreateBlendState(&blend, &sky.blendState));
			SEAMLESS_ASSERT(made);
			vs->Release();
			ps->Release();
		}
		// Into the top-left a_width x a_height of the target, where the picture is drawn.
		void DrawSky(ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a_target, const RE::NiCamera* a_camera, const SkyColors& a_colors, float a_width, float a_height)
		{
			EnsureSkyPipeline(D3D::Device());

			// NiCamera: looks along local +X, up is +Y, right is +Z.
			const auto& m = a_camera->world.rotate;
			const auto& frustum = a_camera->GetRuntimeData2().viewFrustum;
			SkyConstants c{};
			for (int row = 0; row < 3; ++row) {
				c.forward[row] = m.entry[row][0];
				c.up[row] = m.entry[row][1] * frustum.fTop;
				c.right[row] = m.entry[row][2] * frustum.fRight;
			}
			const auto put = [](float* a_out, const RE::NiColor& a_color) {
				a_out[0] = a_color.red;
				a_out[1] = a_color.green;
				a_out[2] = a_color.blue;
			};
			put(c.upper, a_colors.upper);
			put(c.horizon, a_colors.horizon);
			put(c.lower, a_colors.lower);
			c.size[0] = a_width;
			c.size[1] = a_height;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(a_context->Map(sky.constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				return;
			}
			std::memcpy(mapped.pData, &c, sizeof(c));
			a_context->Unmap(sky.constants, 0);

			const D3D::SavedState saved(a_context);
			const D3D11_VIEWPORT  viewport{ 0, 0, a_width, a_height, 0, 1 };
			a_context->OMSetRenderTargets(1, &a_target, nullptr);
			a_context->OMSetDepthStencilState(sky.depthState, 0);
			a_context->OMSetBlendState(sky.blendState, nullptr, 0xFFFFFFFF);
			a_context->RSSetState(sky.rasterState);
			a_context->RSSetViewports(1, &viewport);
			a_context->VSSetShader(sky.vertexShader, nullptr, 0);
			a_context->PSSetShader(sky.pixelShader, nullptr, 0);
			a_context->GSSetShader(nullptr, nullptr, 0);
			a_context->HSSetShader(nullptr, nullptr, 0);
			a_context->DSSetShader(nullptr, nullptr, 0);
			a_context->IASetInputLayout(nullptr);
			a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			a_context->PSSetConstantBuffers(0, 1, &sky.constants);
			a_context->Draw(3, 0);
		}


		RE::NiPointer<RE::NiCamera> cullCamera;

		// The portal camera's clip distances for the doorway on screen, for its quad's motion vectors.
		float portalNear = 15.0f;
		float portalFar = 300000.0f;

		// Two sets: the pass draws into the one not on screen, and the doorway shows the other until
		// the new picture is done (see Draw). A doorway picture's capture draws into the
		// hidden set while the doorway keeps showing the last live picture.
		OwnTargets portalTargetSets[2];
		int        shownTargets = 0;

		template <class T>
		void ReleaseView(T*& a_object)
		{
			if (a_object) {
				reinterpret_cast<IUnknown*>(a_object)->Release();
				a_object = nullptr;
			}
		}

		void ReleaseDepth(RE::BSGraphics::DepthStencilData& a_depth)
		{
			for (auto& view : a_depth.views) {
				ReleaseView(view);
			}
			for (auto& view : a_depth.readOnlyViews) {
				ReleaseView(view);
			}
			ReleaseView(a_depth.depthSRV);
			ReleaseView(a_depth.stencilSRV);
			ReleaseView(a_depth.texture);
		}

		D3D11_TEXTURE2D_DESC DescOf(REX::W32::ID3D11Texture2D* a_texture)
		{
			D3D11_TEXTURE2D_DESC desc{};
			reinterpret_cast<ID3D11Texture2D*>(a_texture)->GetDesc(&desc);
			return desc;
		}

		bool SameTexture(const D3D11_TEXTURE2D_DESC& a_left, const D3D11_TEXTURE2D_DESC& a_right)
		{
			return a_left.Width == a_right.Width && a_left.Height == a_right.Height && a_left.MipLevels == a_right.MipLevels && a_left.ArraySize == a_right.ArraySize &&
			       a_left.Format == a_right.Format && a_left.SampleDesc.Count == a_right.SampleDesc.Count && a_left.BindFlags == a_right.BindFlags;
		}

		// A view of ours like the engine's view a_like (same description), or none if it has none.
		template <class View, class Desc, class Create>
		void CloneView(REX::W32::ID3D11Texture2D* a_texture, auto* a_like, auto*& a_out, Create a_create)
		{
			if (!a_like) {
				return;
			}
			Desc desc{};
			reinterpret_cast<View*>(a_like)->GetDesc(&desc);
			View* view = nullptr;
			// Identical to a view the engine made of an identical texture: only failing for want of
			// video memory, which the game wouldn't survive either.
			const HRESULT created = a_create(reinterpret_cast<ID3D11Texture2D*>(a_texture), &desc, &view);
			SEAMLESS_ASSERT(SUCCEEDED(created));
			a_out = reinterpret_cast<std::remove_reference_t<decltype(a_out)>>(view);
		}

		// A depth-stencil target of ours made like the engine's a_like: the same texture, the same views
		// (one per slice of an array). Made anew when the engine's has changed.
		void EnsureOwnDepth(RE::BSGraphics::DepthStencilData& a_ours, const RE::BSGraphics::DepthStencilData& a_like)
		{
			SEAMLESS_ASSERT(a_like.texture);
			const auto desc = DescOf(a_like.texture);
			if (a_ours.texture && SameTexture(DescOf(a_ours.texture), desc)) {
				return;
			}
			ReleaseDepth(a_ours);
			auto*            device = D3D::Device();
			ID3D11Texture2D* texture = nullptr;
			const HRESULT    created = device->CreateTexture2D(&desc, nullptr, &texture);
			SEAMLESS_ASSERT(SUCCEEDED(created));
			a_ours.texture = reinterpret_cast<REX::W32::ID3D11Texture2D*>(texture);
			const auto srv = [&](ID3D11Texture2D* t, const D3D11_SHADER_RESOURCE_VIEW_DESC* d, ID3D11ShaderResourceView** v) { return device->CreateShaderResourceView(t, d, v); };
			const auto dsv = [&](ID3D11Texture2D* t, const D3D11_DEPTH_STENCIL_VIEW_DESC* d, ID3D11DepthStencilView** v) { return device->CreateDepthStencilView(t, d, v); };
			for (std::size_t i = 0; i < std::size(a_like.views); ++i) {
				CloneView<ID3D11DepthStencilView, D3D11_DEPTH_STENCIL_VIEW_DESC>(a_ours.texture, a_like.views[i], a_ours.views[i], dsv);
				CloneView<ID3D11DepthStencilView, D3D11_DEPTH_STENCIL_VIEW_DESC>(a_ours.texture, a_like.readOnlyViews[i], a_ours.readOnlyViews[i], dsv);
			}
			CloneView<ID3D11ShaderResourceView, D3D11_SHADER_RESOURCE_VIEW_DESC>(a_ours.texture, a_like.depthSRV, a_ours.depthSRV, srv);
			CloneView<ID3D11ShaderResourceView, D3D11_SHADER_RESOURCE_VIEW_DESC>(a_ours.texture, a_like.stencilSRV, a_ours.stencilSRV, srv);
		}

		// Made anew whenever the engine's targets have changed (it recreates them on a resolution change).
		void EnsureOwnTargets(OwnTargets& a_set)
		{
			auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
			// The colour is made like the main scene's, not like the pause-menu background whose table
			// entry it takes: that one holds 8 bits per channel, and the world at this point of the
			// frame is still in linear high dynamic range (tone mapped later, with the portal in it).
			// The portal's darks came out in visible bands and its lights clipped.
			const auto& engineColor = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
			const auto& engineDepth = renderer->GetDepthStencilData().depthStencils[kDepthTarget];
			SEAMLESS_ASSERT(engineColor.texture && engineDepth.texture);
			const auto colorDesc = DescOf(engineColor.texture);
			const auto depthDesc = DescOf(engineDepth.texture);
			if (a_set.color.texture && SameTexture(DescOf(a_set.color.texture), colorDesc) && a_set.depth.texture && SameTexture(DescOf(a_set.depth.texture), depthDesc)) {
				return;
			}
			ReleaseView(a_set.color.RTV);
			ReleaseView(a_set.color.SRV);
			ReleaseView(a_set.color.UAV);
			ReleaseView(a_set.color.texture);
			auto*            device = D3D::Device();
			ID3D11Texture2D* colorTexture = nullptr;
			const HRESULT    colorCreated = device->CreateTexture2D(&colorDesc, nullptr, &colorTexture);
			SEAMLESS_ASSERT(SUCCEEDED(colorCreated));
			a_set.color.texture = reinterpret_cast<REX::W32::ID3D11Texture2D*>(colorTexture);

			const auto rtv = [&](ID3D11Texture2D* t, const D3D11_RENDER_TARGET_VIEW_DESC* d, ID3D11RenderTargetView** v) { return device->CreateRenderTargetView(t, d, v); };
			const auto srv = [&](ID3D11Texture2D* t, const D3D11_SHADER_RESOURCE_VIEW_DESC* d, ID3D11ShaderResourceView** v) { return device->CreateShaderResourceView(t, d, v); };
			const auto uav = [&](ID3D11Texture2D* t, const D3D11_UNORDERED_ACCESS_VIEW_DESC* d, ID3D11UnorderedAccessView** v) { return device->CreateUnorderedAccessView(t, d, v); };
			CloneView<ID3D11RenderTargetView, D3D11_RENDER_TARGET_VIEW_DESC>(a_set.color.texture, engineColor.RTV, a_set.color.RTV, rtv);
			CloneView<ID3D11ShaderResourceView, D3D11_SHADER_RESOURCE_VIEW_DESC>(a_set.color.texture, engineColor.SRV, a_set.color.SRV, srv);
			CloneView<ID3D11UnorderedAccessView, D3D11_UNORDERED_ACCESS_VIEW_DESC>(a_set.color.texture, engineColor.UAV, a_set.color.UAV, uav);
			EnsureOwnDepth(a_set.depth, engineDepth);
			logger::info("portal: own render targets made ({}x{} colour format {}, depth format {})", colorDesc.Width, colorDesc.Height, static_cast<int>(colorDesc.Format),
				static_cast<int>(depthDesc.Format));
		}

		// The portal pass had no sun shadows: through a city gate everything the other side's sun
		// reaches was lit, also what its cliffs and walls shade, and a low sun's specular glinted on the
		// needles of trees standing in a cliff's shadow. The other side's light scene has a sun shadow
		// light of its own (made with the scene, ID 106320); it is run through the engine's sequence for
		// the world's (ID 107132 aims and culls, ID 107137 accumulates, ID 107133 renders), with the
		// portal camera and into shadow map arrays of our own: the world's are still read after the
		// portal pass (Community Shaders keeps a soft copy of them for the rest of the frame).
		RE::BSGraphics::DepthStencilData portalShadowMaps[2];
		constexpr RE::RENDER_TARGETS_DEPTHSTENCIL::RENDER_TARGET_DEPTHSTENCIL kShadowMapTargets[2]{
			RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM,                   // the cascades (BSShadowDirectionalLight::Render)
			RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM  // drawn too in the engine's usual shadow mode (ID 391108)
		};

		void RenderPortalShadows(RE::ShadowSceneNode* a_root, RE::NiCamera* a_camera)
		{
			auto* light = a_root->GetRuntimeData().sunShadowDirLight;
			auto& table = RE::BSGraphics::Renderer::GetSingleton()->GetDepthStencilData().depthStencils;
			for (std::size_t i = 0; i < std::size(kShadowMapTargets); ++i) {
				EnsureOwnDepth(portalShadowMaps[i], table[kShadowMapTargets[i]]);
			}
			// Cascades fitted to the portal camera, along the sun ApplyRemoteLighting aimed.
			light->UpdateCamera(a_camera);

			// The culling jobs take the scene's top nodes, shared out over one list per job (the world's
			// are made by ID 106323); a light has a culling process per job.
			auto&                                                    lightData = light->GetRuntimeData();
			RE::BSTArray<RE::BSTArray<RE::NiPointer<RE::NiAVObject>>> jobs;
			jobs.resize(light->GetShadowDirectionalLightRuntimeData().fullFrustumCullingProcessArray.size());
			std::uint32_t next = 0;
			for (auto& child : a_root->GetChildren()) {
				if (child) {
					jobs[next++ % jobs.size()].push_back(child);
				}
			}
			// The world's culls a last list on its own (from the main culling process); the scene has none.
			RE::BSTArray<RE::NiPointer<RE::NiAVObject>> none;
			for (auto& descriptor : lightData.shadowmapDescriptors) {
				// As the portal's own accumulator (see Draw): passes are made against a light scene.
				descriptor.shaderAccumulator->GetRuntimeData().activeShadowSceneNode = a_root;
			}
			using Cull = void (*)(RE::BSShadowDirectionalLight*, RE::BSTArray<RE::BSTArray<RE::NiPointer<RE::NiAVObject>>>&, RE::BSTArray<RE::NiPointer<RE::NiAVObject>>&);
			static REL::Relocation<Cull> cull{ REL::RelocationID(101498, 108492) };
			cull(light, jobs, none);
			std::uint32_t shadowLights = 0;
			light->Accumulate(shadowLights, 0, nullptr);

			RE::BSGraphics::DepthStencilData saved[std::size(kShadowMapTargets)];
			for (std::size_t i = 0; i < std::size(kShadowMapTargets); ++i) {
				saved[i] = table[kShadowMapTargets[i]];
				table[kShadowMapTargets[i]] = portalShadowMaps[i];
			}
			std::uint32_t index = 0;
			light->Render(index);
			for (std::size_t i = 0; i < std::size(kShadowMapTargets); ++i) {
				table[kShadowMapTargets[i]] = saved[i];
			}
			for (auto& descriptor : lightData.shadowmapDescriptors) {
				ResetAccumulator(descriptor.shaderAccumulator.get());
			}
			::Render::Backend::SetPortalShadows(light, portalShadowMaps[0].depthSRV, portalShadowMaps[1].depthSRV);
		}

		// Doors standing in the far door's doorway besides the far door itself: a second, ordinary door
		// in the same frame (Riverwood Trader has one; from outside, the merged room's
		// HideDoorsInDoorway hides them). Its back face lies on the clip plane at the far door (see
		// the far door clip in Draw), so whether it was cut away or drawn changed with the smallest
		// camera movement: a closed door flashed up in the doorway, close to it. Looked for once per
		// far door.
		std::vector<RE::NiAVObject*> DoorsBesideFarDoor(RE::TESObjectREFR* a_linked)
		{
			static RE::FormID              scannedFor = 0;
			static std::vector<RE::FormID> found;
			if (a_linked->GetFormID() != scannedFor) {
				scannedFor = a_linked->GetFormID();
				found.clear();
				const auto center = Portal::Doors::Center(a_linked);
				auto*      cell = a_linked->GetParentCell();
				auto*      world = cell && !cell->IsInteriorCell() ? cell->GetRuntimeData().worldSpace : nullptr;
				const auto visit = [&](RE::TESObjectCELL* a_cell) {
					if (!a_cell) {
						return;
					}
					a_cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
						auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
						if (a_ref != a_linked && base && base->Is(RE::FormType::Door) && a_ref->GetPosition().GetDistance(center) < 120.0f) {
							found.push_back(a_ref->GetFormID());
						}
						return RE::BSContainer::ForEachResult::kContinue;
					});
				};
				visit(cell);
				if (world && world->persistentCell != cell) {
					visit(world->persistentCell);  // load doors are persistent references
				}
				for (const auto id : found) {
					logger::info("portal: door {:08X} stands in the far doorway of {:08X}; left out of the portal pass", id, scannedFor);
				}
			}
			std::vector<RE::NiAVObject*> objects;
			for (const auto id : found) {
				auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
				if (auto* object = ref ? ref->Get3D() : nullptr) {
					objects.push_back(object);
				}
			}
			return objects;
		}

		// The far door sits right at the clip distance, so it and the doors standing in its doorway are
		// hidden for the pass. Not the far door when it is the one swung open (PortalDoor::Update): its
		// leaves stand in the space behind, and a city gate's doorway stood empty.
		class HiddenFarDoors
		{
		public:
			explicit HiddenFarDoors(RE::TESObjectREFR* a_linked)
			{
				portalHiddenShapes.clear();
				if (!Portal::Doors::IsSwung(a_linked->GetFormID())) {
					Hide(a_linked->Get3D());
				}
				for (auto* object : DoorsBesideFarDoor(a_linked)) {
					Hide(object);
				}
			}

			~HiddenFarDoors()
			{
				for (const auto& [object, culled] : hidden) {
					object->SetAppCulled(culled);
				}
			}

			HiddenFarDoors(const HiddenFarDoors&) = delete;
			HiddenFarDoors& operator=(const HiddenFarDoors&) = delete;

		private:
			void Hide(RE::NiAVObject* a_object)
			{
				hidden.emplace_back(a_object, a_object->GetAppCulled());
				a_object->SetAppCulled(true);
				RE::BSVisit::TraverseScenegraphGeometries(a_object, [](RE::BSGeometry* a_shape) {
					portalHiddenShapes.insert(a_shape);
					return RE::BSVisit::BSVisitControl::kContinue;
				});
			}

			std::vector<std::pair<RE::NiAVObject*, bool>> hidden;
		};

		// The doorway picture is of the room without its people: kept for good (and maybe shipped),
		// it showed whoever stood there when it was taken, frozen and blurred, where they no longer
		// were when the real room faded in. The returned ones are to be shown again afterwards.
		std::vector<RE::NiAVObject*> HideActorsForCapture(const std::vector<RE::TESObjectCELL*>& a_cells)
		{
			std::vector<RE::NiAVObject*> hidden;
			for (auto* cell : a_cells) {
				cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
					auto* actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
					auto* object = actor ? actor->Get3D() : nullptr;
					if (object && object->GetFlags().none(RE::NiAVObject::Flag::kNotVisible)) {
						object->GetFlags().set(RE::NiAVObject::Flag::kNotVisible);
						hidden.push_back(object);
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
			}
			return hidden;
		}

		void ShowActorsAgain(const std::vector<RE::NiAVObject*>& a_hidden)
		{
			for (auto* object : a_hidden) {
				object->GetFlags().reset(RE::NiAVObject::Flag::kNotVisible);
			}
		}

		std::optional<CaptureDrawn> captureDrawn;

	}

	OwnTargets& DrawingTargets()
	{
		return portalTargetSets[1 - shownTargets];
	}

	OwnTargets& ShownTargets()
	{
		return portalTargetSets[shownTargets];
	}

	void EnsureOwnTargets()
	{
		for (auto& set : portalTargetSets) {
			EnsureOwnTargets(set);
		}
	}

	OwnTargetsInPlace::OwnTargetsInPlace(const OwnTargets& a_set)
	{
		EnsureOwnTargets();
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		color = &renderer->GetRuntimeData().renderTargets[kColorTarget];
		depth = &renderer->GetDepthStencilData().depthStencils[kDepthTarget];
		savedColor = *color;
		savedDepth = *depth;
		color->texture = a_set.color.texture;
		color->RTV = a_set.color.RTV;
		color->SRV = a_set.color.SRV;
		color->UAV = a_set.color.UAV;
		*depth = a_set.depth;
	}

	OwnTargetsInPlace::~OwnTargetsInPlace()
	{
		*color = savedColor;
		*depth = savedDepth;
	}

	std::array<RE::NiPoint3, 4> DoorwayQuad(const Active& a_portal, const RE::NiPoint3& a_viewer)
	{
		const bool gate = Portal::Gates::IsGate(a_portal);
		return Portal::Doors::Quad(Portal::Doors::Door(a_portal.sourceDoor), a_viewer, { .behindPanel = !gate, .wholeMesh = gate });
	}

	std::optional<ScreenRect> DoorwayOnScreen(const Active& a_portal)
	{
		auto*      camera = RE::Main::WorldRootCamera();
		ScreenRect rect{ 1.0f, 0.0f, 1.0f, 0.0f };
		for (const auto& corner : DoorwayQuad(a_portal, camera->world.translate)) {
			float x = 0, y = 0, z = 0;
			camera->WorldPtToScreenPt3(corner, x, y, z, 1e-5f);
			if (z <= 0.0f) {
				// A corner behind the camera (standing in the doorway): the projection of the rest
				// says nothing reliable; treat the whole screen as the doorway.
				return ScreenRect{ 0.0f, 1.0f, 0.0f, 1.0f };
			}
			rect = { std::min(rect[0], x), std::max(rect[1], x), std::min(rect[2], y), std::max(rect[3], y) };
		}
		if (rect[1] < 0.0f || rect[0] > 1.0f || rect[3] < 0.0f || rect[2] > 1.0f) {
			return std::nullopt;
		}
		constexpr float kMargin = 0.02f;
		return ScreenRect{ std::clamp(rect[0] - kMargin, 0.0f, 1.0f), std::clamp(rect[1] + kMargin, 0.0f, 1.0f), std::clamp(rect[2] - kMargin, 0.0f, 1.0f),
			std::clamp(rect[3] + kMargin, 0.0f, 1.0f) };
	}

	std::vector<RE::NiAVObject*> ShowActorsForCull(const std::vector<RE::TESObjectCELL*>& a_cells)
	{
		std::vector<RE::NiAVObject*> shown;
		for (auto* cell : a_cells) {
			cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
				auto* actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
				auto* object = actor ? actor->Get3D() : nullptr;
				if (object && !object->GetAppCulled() && object->GetFlags().any(RE::NiAVObject::Flag::kNotVisible)) {
					object->GetFlags().reset(RE::NiAVObject::Flag::kNotVisible);
					shown.push_back(object);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}
		return shown;
	}

	void HideActorsAgain(const std::vector<RE::NiAVObject*>& a_shown)
	{
		for (auto* object : a_shown) {
			object->GetFlags().set(RE::NiAVObject::Flag::kNotVisible);
		}
	}

	const std::optional<CaptureDrawn>& CaptureDrawnThisFrame()
	{
		return captureDrawn;
	}

	void BeginFrame()
	{
		captureDrawn.reset();
	}

	float NearClip()
	{
		return portalNear;
	}

	float FarClip()
	{
		return portalFar;
	}

	std::uint32_t FreedPassesThisSubmit()
	{
		return freedPassesThisSubmit;
	}

	json Draw(const Active& a_portal, const ScreenRect& a_doorway, const std::optional<Portal::Snapshot::CaptureCamera>& a_capture, bool a_fullTarget)
	{
		auto* root = Portal::LightScene::Get();
		auto* mainCamera = RE::Main::WorldRootCamera();
		if (!mainCamera) {
			throw std::runtime_error("no main camera");
		}
		if (!portalCamera) {
			portalCamera.reset(static_cast<RE::NiCamera*>(mainCamera->Clone()));
		}
		// Frustum, viewport and projection from the main camera every frame: taken once, a change
		// of field of view or aspect (a zoom, the FOV console command, a resized window) put the
		// portal image out of line with the doorway, which is composited pixel for pixel.
		portalCamera->GetRuntimeData2() = mainCamera->GetRuntimeData2();
		defaultNear = portalCamera->GetRuntimeData2().viewFrustum.fNear;
		// A fresh accumulator per remote cell: reusing the previous cell's crashed the batch
		// renderer on its leftover passes. The old one is kept alive, like the old light scenes.
		static std::uint32_t                                          generation = UINT32_MAX;
		static std::vector<RE::NiPointer<RE::BSShaderAccumulator>> retiredAccumulators;
		if (!portalAccumulator || generation != a_portal.generation) {
			if (portalAccumulator) {
				// Kept like the retired light scenes (LightScene::Renew): freeing those crashed, and
				// nothing says the passes cached on shapes don't name their accumulator too.
				retiredAccumulators.push_back(std::move(portalAccumulator));
			}
			portalFrame = 0;
			portalAccumulator.reset(RE::BSShaderAccumulator::Create(0x63));  // engine-wide ctor argument
			generation = a_portal.generation;
		}
		const auto mapping = Portal::Doors::Compute(a_portal);
		const bool capturing = a_capture.has_value();
		portalCamera->local = mapping.toLinked * (capturing ? a_capture->transform : mainCamera->world);
		// A gate's mapping is scaled (Doors::Compute) for where the camera stands; what it sees from
		// there doesn't change with its own scale.
		portalCamera->local.scale = 1.0f;
		if (capturing) {
			auto& frustum = portalCamera->GetRuntimeData2().viewFrustum;
			frustum.fLeft = a_capture->left;
			frustum.fRight = a_capture->right;
			frustum.fBottom = a_capture->bottom;
			frustum.fTop = a_capture->top;
		}
		RE::NiUpdateData update;
		portalCamera->Update(update);

		// Poor man's clip plane: push the near plane up to the linked door, so the walls and the
		// door between the mirrored camera and the doorway don't block the view. Exact only when
		// looking straight at the door (as the capture camera does); an oblique projection would
		// make it exact at any angle.
		auto*              linked = Portal::Doors::Door(a_portal.linkedDoor);
		const auto&        rotate = portalCamera->world.rotate;
		const RE::NiPoint3 forward{ rotate.entry[0][0], rotate.entry[1][0], rotate.entry[2][0] };  // NiCamera looks along local +X
		// A gate's mapping is scaled: the capture camera stands that much further from the far gate.
		const float        toDoor = capturing ? a_capture->toDoorway * mapping.toLinked.scale : (Portal::Doors::Center(linked) - portalCamera->world.translate).Dot(forward);
		// The capture camera cuts just past the door plane: its
		// picture reaches beyond the doorway, where the room's front wall and the door's frame
		// stood within the 20 units kept for the doorway quad, slivers at the plane that the
		// stand-in drew out into vertical streaks seen from aside. The street's side has its own
		// wall and frame anyway.
		constexpr float kCaptureCut = 12.0f;
		portalCamera->GetRuntimeData2().viewFrustum.fNear = capturing ? toDoor + kCaptureCut : std::max(defaultNear, toDoor - 20.0f);
		// The doorway on screen still shows the last picture, read with the clip distances it was
		// drawn with.
		if (capturing) {
			// In the player's space's units, as the picture keeps its distances: the depth read with
			// them gives the far side's distances divided by the mapping's scale.
			const float scale = mapping.toLinked.scale;
			captureDrawn = CaptureDrawn{ a_capture->door, !a_portal.cell->IsInteriorCell(), Portal::Gates::IsGate(a_portal), portalCamera->GetRuntimeData2().viewFrustum.fNear / scale,
				portalCamera->GetRuntimeData2().viewFrustum.fFar / scale };
		} else {
			portalNear = portalCamera->GetRuntimeData2().viewFrustum.fNear;
			portalFar = portalCamera->GetRuntimeData2().viewFrustum.fFar;
		}

		// The accumulator draws with the portal's own light scene (the root): with no light scene
		// GetRenderPasses crashed while culling, and borrowing the main view's crashed as soon as
		// the player moved. The eye position is what ID 52727 copies from its camera.
		auto&       data = portalAccumulator->GetRuntimeData();
		const auto& mainData = RE::BSShaderAccumulator::GetMainAccumulator()->GetRuntimeData();
		data.activeShadowSceneNode = root;
		data.renderMode = mainData.renderMode;
		data.eyePosition = portalCamera->world.translate;

		// Everything below is restored at the end.
		auto&       shadowState = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
		const auto  savedColorTarget = shadowState.renderTargets[0];
		const auto  savedColorMode = shadowState.setRenderTargetMode[0];
		const auto  savedDepthTarget = shadowState.depthStencil;
		const auto  savedDepthSlice = shadowState.depthStencilSlice;
		const auto  savedDepthTargetMode = shadowState.setDepthStencilMode;
		const auto  savedPosAdjust = shadowState.posAdjust;
		const auto  savedPreviousPosAdjust = shadowState.previousPosAdjust;
		const auto  savedCameraData = shadowState.cameraData;
		auto* const savedAccumulator = RE::BSShaderAccumulator::GetCurrentAccumulator();
		static REL::Relocation<RE::NiCamera**>  currentCamera{ REL::RelocationID(527667, 414580) };  // set by SubmitAccumulator
		static REL::Relocation<std::uint32_t*> depthMode{ REL::RelocationID(524794, 388840) };       // cached DepthStencilDepthMode
		static REL::Relocation<std::uint32_t*> dirtyStates{ REL::RelocationID(524773, 388819) };
		static REL::Relocation<std::uint8_t*>  menuDrawGlobal{ REL::RelocationID(525013, 411494) };
		auto* const savedCamera = *currentCamera;
		const auto  savedDepthMode = *depthMode;
		const auto  savedMenuDrawGlobal = *menuDrawGlobal;
		const auto  mainAmbient = SpaceLighting::SaveAmbient();
		auto*       d3dContext = D3D::Context();
		const OwnTargetsInPlace targets(DrawingTargets());  // to the end of the function, past the restore below

		const HiddenFarDoors hiddenDoors(linked);

		Portal::LightScene::PrepareForView();
		ResetAccumulator(portalAccumulator.get());
		RE::BSCullingProcess::CullingContext context;
		context.accumulator.reset(portalAccumulator.get());
		// Culled with a camera whose frustum only covers the doorway: everything else would be
		// drawn and then hidden by the walls around the door anyway.
		if (!cullCamera) {
			cullCamera.reset(static_cast<RE::NiCamera*>(portalCamera->Clone()));
		}
		cullCamera->local = portalCamera->world;
		{
			cullCamera->GetRuntimeData2() = portalCamera->GetRuntimeData2();
			const auto& full = portalCamera->GetRuntimeData2().viewFrustum;
			auto&       narrow = cullCamera->GetRuntimeData2().viewFrustum;
			// The capture camera's image is all room: none of it is hidden by walls.
			constexpr ScreenRect whole{ 0.0f, 1.0f, 0.0f, 1.0f };
			const auto&          rect = capturing ? whole : a_doorway;
			narrow.fLeft = full.fLeft + (full.fRight - full.fLeft) * rect[0];
			narrow.fRight = full.fLeft + (full.fRight - full.fLeft) * rect[1];
			narrow.fBottom = full.fBottom + (full.fTop - full.fBottom) * rect[2];
			narrow.fTop = full.fBottom + (full.fTop - full.fBottom) * rect[3];
			RE::NiUpdateData cullUpdate;
			cullCamera->Update(cullUpdate);
		}
		context.camera.reset(cullCamera.get());
		context.scene.reset(root);
		// An interior built of rooms (room bounds: Jorrvaskr's living quarters, under rooms all of
		// it) hangs its contents under BSMultiBoundRoom nodes, and a room's cull (ID 75886) walks
		// into its children only when the context's portal graph lists the room, or when the cull
		// ignores multibounds. The portal scene has no graph (see RemoteCell::AttachOne), so such
		// a room drew nothing: a black doorway. Outdoors keeps the normal mode, where a
		// multibound around a group of statics still culls the whole group at once.
		if (a_portal.cell->IsInteriorCell()) {
			context.cullMode = RE::BSCullingProcess::BSCPCullingType::kIgnoreMultiBounds;
		}
		{
			// NPCs of a remote interior keep NiAVObject::kNotVisible on their 3D root, and the cull
			// skips them: in the portal the room was empty. Outdoors the flag was clear.
			const auto actors = capturing ? HideActorsForCapture(Portal::Manager::RemoteCells()) : ShowActorsForCull(Portal::Manager::RemoteCells());
			{
				const Diag::Scope scope("portal cull");
				RE::BSCullingProcess::Process(context);
			}
			if (capturing) {
				ShowActorsAgain(actors);
			} else {
				HideActorsAgain(actors);
			}
		}

		// The other side's sun and ambient, before its shadows are drawn with that sun.
		const auto background = SpaceLighting::ApplyRemoteLighting(root, a_portal.cell);
		const bool shadowed = !a_portal.cell->IsInteriorCell() && ::Render::Backend::CanShadowPortal();
		if (shadowed) {
			const Diag::Scope scope("portal shadows");
			Diag::GpuBegin("gpu portal shadows");
			RenderPortalShadows(root, portalCamera.get());
			Diag::GpuEnd("gpu portal shadows");
		}
		::Render::Backend::SetPortalWorldspace(a_portal.cell->IsInteriorCell() ? nullptr : a_portal.cell->GetRuntimeData().worldSpace);

		// Same sequence as the engine's menu-3D renderer (ID 52727): bind with clear, let the clear
		// happen now (the engine clears lazily, when the binding is applied), then bind without.
		SetColorTarget(0, kColorTarget, RE::BSGraphics::SRTM_CLEAR, true);
		SetDepthTarget(kDepthTarget, RE::BSGraphics::SRTM_CLEAR, 0);
		{
			static REL::Relocation<void (*)(void*)>                resetState{ REL::RelocationID(75570, 77371) };  // Renderer::ResetState
			static REL::Relocation<void (*)(void*, std::uint32_t)> applyState{ REL::RelocationID(75462, 77247) };
			resetState(RendererObject());
			applyState(RendererObject(), 0);
		}
		SetColorTarget(0, kColorTarget, RE::BSGraphics::SRTM_NO_CLEAR, true);
		SetDepthTarget(kDepthTarget, RE::BSGraphics::SRTM_NO_CLEAR, 0);
		// Binding the targets set the viewport the way the main view's is set, render scale included
		// (ID 77240); the full target's comes with kRenderFlags at the submit.
		const bool atRenderScale = !capturing && !a_fullTarget;
		{
			auto&      drawing = DrawingTargets();
			const auto desc = DescOf(drawing.color.texture);
			const auto scale = atRenderScale ? D3D::RenderScale() : std::array{ 1.0f, 1.0f };
			drawing.pictureWidth = desc.Width * scale[0];
			drawing.pictureHeight = desc.Height * scale[1];
		}
		// Start from the normal depth mode instead of whatever the previous pass ended in, and set
		// the global ID 52727 sets around its own draw (the accumulator's draw routine, ID 106583,
		// otherwise may switch to "test EQUAL").
		*depthMode = static_cast<std::uint32_t>(RE::BSGraphics::DepthStencilDepthMode::kTestWrite);
		*dirtyStates |= 0x4;  // DIRTY_DEPTH_MODE
		*menuDrawGlobal = 1;
		// Set by Main::RenderWaterEffects when the main view has water in it; the post-accumulation
		// stage (ID 106584) then rebinds the world's depth buffer for its blended passes, which
		// inside the portal pass would test and draw them against the main view's depth.
		static REL::Relocation<std::uint32_t*> waterMode{ REL::RelocationID(513267, 391008) };
		const auto                             savedWaterMode = *waterMode;
		*waterMode = 0;

		if (background) {
			if (auto* view = reinterpret_cast<ID3D11RenderTargetView*>(RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().renderTargets[kColorTarget].RTV)) {
				DrawSky(d3dContext, view, portalCamera.get(), *background, DrawingTargets().pictureWidth, DrawingTargets().pictureHeight);
			}
		}
		{
			// The far door's plane, facing away from the portal camera: nothing on the camera's side
			// of it can be seen through the doorway (see SeamlessPortalClips in the fork). Its face
			// on the camera's side, so the door's own frame isn't cut.
			RE::NiPoint3 through = mapping.toLinked.rotate * mapping.inward;
			through.z = 0.0f;
			through.Unitize();
			const auto face = Portal::Doors::Center(linked) - through * Portal::Doors::Size(linked).halfDepth;
			farDoorClip = ClipPlane{ ToCore(through), face.Dot(through) };
			::Render::Backend::SetPortalClip(farDoorClip);
		}
		{
			auto* sun = root->GetRuntimeData().sunLight;
			::Render::Backend::BeginPortalPass(sun ? skyrim_cast<RE::NiDirectionalLight*>(sun->light.get()) : nullptr, a_portal.cell->IsInteriorCell(), portalCamera.get());
		}

		portalFrame += 1;
		freedPassesThisSubmit = 0;
		drawnThisSubmit.clear();
		// Only the doorway's part of the picture is ever shown, yet each shape the doorway's cull keeps
		// was shaded across the whole picture: at Solitude's gate 24 times the doorway's pixels. The
		// doorway's rectangle (with its margin) bounds the quad that shows the picture, at render scale
		// and jitter alike. The main view's rain drawn into the picture below is cut the same way. A
		// capture's picture is all of it.
		// Switched off again on leaving, also by an exception (a door unloaded mid-frame, caught in
		// the draw hook): left on, the rest of the frame was cut to the doorway's rectangle.
		struct Scissor
		{
			decltype(shadowState) state;
			bool                  on;
			void Off()
			{
				if (std::exchange(on, false)) {
					state.rasterStateScissorMode = 0;
					state.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RASTER_CULL_MODE);
				}
			}
			~Scissor() { Off(); }
		} scissor{ shadowState, !capturing && !a_fullTarget };
		if (scissor.on) {
			const float     width = DrawingTargets().pictureWidth, height = DrawingTargets().pictureHeight;
			const D3D11_RECT rect{ static_cast<LONG>(std::floor(a_doorway[0] * width)), static_cast<LONG>(std::floor((1.0f - a_doorway[3]) * height)),
				static_cast<LONG>(std::ceil(a_doorway[1] * width)), static_cast<LONG>(std::ceil((1.0f - a_doorway[2]) * height)) };
			d3dContext->RSSetScissorRects(1, &rect);
			shadowState.rasterStateScissorMode = 1;
			shadowState.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RASTER_CULL_MODE);
		}
		inPortalSubmit = true;
		{
			const Diag::Scope scope("portal submit");
			Diag::GpuBegin("gpu portal submit");
			RE::BSGraphics::Renderer::SubmitAccumulator(portalCamera.get(), portalAccumulator.get(), atRenderScale ? LiveRenderFlags() : kRenderFlags);
			Diag::GpuEnd("gpu portal submit");
		}
		inPortalSubmit = false;
		Diag::AddCount("portal passes", static_cast<double>(drawnThisSubmit.size()));
		// Not for the main view's rain drawn into the picture next (its own pass, see
		// DrawMainPrecipitation); EndPortalPass puts the world's shadows back.
		if (shadowed) {
			::Render::Backend::SetPortalShadows(nullptr, nullptr, nullptr);
		}
		// The weather's rain falls through a city gate too. Not in a doorway picture: it is kept,
		// and would show its streaks standing still in any weather.
		if (atRenderScale && !a_portal.cell->IsInteriorCell() && !RE::PlayerCharacter::GetSingleton()->GetParentCell()->IsInteriorCell()) {
			DrawMainPrecipitation(root);
		}
		scissor.Off();
		// A capture isn't for the doorway: the last picture stays on screen this frame. Otherwise the
		// new one goes on screen at once; a picture held back for drawing fewer passes than usual
		// stood still in the doorway while the view turned (the doorway's cull is narrowed to its
		// size on screen, so turning away legitimately drew a quarter of the passes).
		if (!capturing) {
			shownTargets = 1 - shownTargets;
		}
		if (portalHiddenDrawn) {
			static std::uint32_t reported = 0;
			if (reported++ < 50) {
				logger::warn("portal: the far door's shapes came up {} time(s) in portal frame {} although culled; left out", portalHiddenDrawn, portalFrame);
			}
			portalHiddenDrawn = 0;
		}
		::Render::Backend::EndPortalPass();
		::Render::Backend::SetPortalClip(std::nullopt);


		// SubmitAccumulator loads the portal camera into the renderer's camera state, including the
		// "previous frame" matrices TAA and CS reproject with; left there, the whole image ghosted.
		shadowState.posAdjust = savedPosAdjust;
		shadowState.previousPosAdjust = savedPreviousPosAdjust;
		shadowState.cameraData = savedCameraData;
		*currentCamera = savedCamera;
		RE::BSShaderAccumulator::SetCurrentAccumulator(savedAccumulator);
		*menuDrawGlobal = savedMenuDrawGlobal;
		*waterMode = savedWaterMode;
		*depthMode = savedDepthMode;
		*dirtyStates |= 0x4;
		SpaceLighting::RestoreAmbient(mainAmbient);
		ResetAccumulator(portalAccumulator.get());
		SetColorTarget(0, savedColorTarget, static_cast<std::uint32_t>(savedColorMode), true);
		SetDepthTarget(savedDepthTarget, static_cast<std::uint32_t>(savedDepthTargetMode), savedDepthSlice);
		{
			// Our targets are still bound until the engine next applies its state; the doorway quad
			// reads them as textures, which D3D refuses while they are bound for output.
			ID3D11RenderTargetView* noTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			d3dContext->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, noTargets, nullptr);
		}

		// Diagnostics, for the bridge's portal_status: only there in agent sessions, and a walk over
		// the cell's references plus a JSON document every frame is wasted on anyone else.
		if (!Diag::AgentMode()) {
			return json::object();
		}
		const auto& p = portalCamera->world.translate;
		// Where the remote cell's NPCs are in the portal image (0..1, z < 0 = behind).
		json onScreen = json::array();
		a_portal.cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
			if (auto* actor = a_ref ? a_ref->As<RE::Actor>() : nullptr; actor && actor->Get3D()) {
				float x = 0, y = 0, z = 0;
				portalCamera->WorldPtToScreenPt3(actor->Get3D()->worldBound.center, x, y, z, 1e-5f);
				onScreen.push_back({ { "name", actor->GetDisplayFullName() }, { "screen", { x, y, z } } });
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		return {
			{ "actorsOnScreen", onScreen },
			{ "portalCamera", { p.x, p.y, p.z } },
			{ "portalForward", { portalCamera->world.rotate.entry[0][0], portalCamera->world.rotate.entry[1][0], portalCamera->world.rotate.entry[2][0] } },
			{ "mainForward", { mainCamera->world.rotate.entry[0][0], mainCamera->world.rotate.entry[1][0], mainCamera->world.rotate.entry[2][0] } },
			{ "linkedDoorCenter", [&] { const auto c = Portal::Doors::Center(linked); return json{ c.x, c.y, c.z }; }() },
			{ "near", portalCamera->GetRuntimeData2().viewFrustum.fNear },
			{ "worldTargets", { static_cast<std::uint32_t>(savedColorTarget), static_cast<std::uint32_t>(savedColorMode), savedDepthTarget, static_cast<std::uint32_t>(savedDepthTargetMode) } },
		};
	}

	void HookDrawPass(std::uintptr_t a_site)
	{
		DrawPassHook::func = SKSE::GetTrampoline().write_call<5>(a_site, DrawPassHook::Thunk);
	}

	void HookRefractionQuery(std::uintptr_t a_site)
	{
		RefractionPassesHook::func = SKSE::GetTrampoline().write_call<5>(a_site, RefractionPassesHook::Thunk);
	}

	void HookWorldDepthTarget(std::uintptr_t a_site)
	{
		WorldDepthTargetHook::func = SKSE::GetTrampoline().write_call<5>(a_site, WorldDepthTargetHook::Thunk);
	}

	void Prewarm(ID3D11Device* a_device)
	{
		EnsureSkyPipeline(a_device);
	}
}
