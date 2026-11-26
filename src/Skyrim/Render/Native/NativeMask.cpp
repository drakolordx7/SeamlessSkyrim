#include "Skyrim/Render/Native/Native.h"

#include "Skyrim/Render/D3D.h"
#include "Core/Render/DoorwayPlanes.h"
#include "Skyrim/Engine/CoreConvert.h"
#include "Skyrim/Render/Native/Pass.h"
#include "Skyrim/Render/SceneQuery.h"

#include <bit>

#include <d3d11shader.h>

#pragma comment(lib, "dxguid.lib")

// The doorway mask without changing the game's shaders.
//
// The game's own shaders can't be changed to discard pixels without replacing them, and ENB
// recognises the game's shaders to apply its own effects, so a replaced shader may lose those.
// Instead, for the draws that need it, a geometry shader is put behind the engine's vertex shader:
// it passes every vertex on unchanged and adds clip distances (SV_ClipDistance), which the
// rasterizer cuts at per pixel. The cut is exact: a plane's distance is linear in world space and
// clip distances are interpolated perspective-correctly.
//
// The rule (X, see Core/Render/DoorwayPlanes.h). A shape of the space the camera is not
// in is kept inside X (one draw, up to six clip distances). A shape of the camera's own space is kept
// outside X; that isn't convex, so it is drawn in up to six disjoint convex pieces (not P0; P0 and
// not P1; P0, P1 and not P2; ...). Shapes whose bounds lie wholly on one side are drawn once,
// unchanged or not at all. Through a city gate X has a sixth plane, where its far gate stands.
//
// The geometry shader has to match the vertex shader's outputs exactly, so one is generated per
// output signature from the vertex shader's bytecode (BSGraphics::VertexShader keeps it), compiled
// with d3dcompiler, and checked against the vertex shader by reflection before it is used.
namespace
{
	using Render::Native::LightingSwap;

	namespace Pass = Render::Native::Pass;
	using Render::ClipPlane;
	using Core::Doorway::Cut;
	using Core::Doorway::Door;
	using Core::Doorway::Plane;

	// Set from the portal code (main thread, as is rendering).
	std::mutex                stateLock;
	std::optional<Door>       door;                // under stateLock
	float                     gatePassage = 0.0f;  // under stateLock; SetGatePassage
	std::optional<ClipPlane>  portalClip;          // under stateLock
	std::atomic<bool>         portalPass = false;
	std::atomic<bool>         precipitationWhole = false;  // PortalPass's own draw of the weather's rain into a picture
	bool                      ready = false;

	// One draw's cut, worked out at the shader's per-draw setup.
	struct ClipPiece
	{
		float planes[Core::Doorway::kMaxPlanes][4];  // clip space; unused ones are (0,0,0,1): distance w > 0
	};

	struct Job
	{
		bool      active = false;
		bool      skip = false;  // nothing of the shape shows
		int       pieces = 0;
		ClipPiece piece[Core::Doorway::kMaxPlanes];
	} job;

	struct Stats
	{
		std::uint64_t passesCut = 0;
		std::uint64_t passesLeftOut = 0;
		std::uint64_t passesWhole = 0;  // of the other space, wholly seen through the doorway
		std::uint64_t drawsCut = 0;
		std::uint64_t piecesDrawn = 0;
		std::uint64_t drawsUncut = 0;  // a cut was wanted but no geometry shader fits
		std::uint64_t portalPasses = 0;
	} stats;

	// The main view's (or portal pass's) row-vector view-projection matrix, camera-relative, and
	// its inverse; recomputed when the matrix or the eye changes.
	struct ClipTransform
	{
		float  matrix[16]{};
		float  eye[3]{};
		double inverse[16]{};
		bool   valid = false;
	} clipTransform;

	// The renderer's current camera (main view, or the portal camera inside the portal pass):
	// SubmitAccumulator loads it into the shadow state, and the vertex shaders project with it.
	bool UpdateClipTransform()
	{
		auto&      shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
		const auto view = shadow.cameraData.getEye();
		const auto eye = shadow.posAdjust.getEye();
		const auto* m = reinterpret_cast<const float*>(&view.viewProjMat);
		const float e[3] = { eye.x, eye.y, eye.z };
		if (clipTransform.valid && std::memcmp(clipTransform.matrix, m, sizeof(clipTransform.matrix)) == 0 && std::memcmp(clipTransform.eye, e, sizeof(e)) == 0) {
			return true;
		}
		std::memcpy(clipTransform.matrix, m, sizeof(clipTransform.matrix));
		std::memcpy(clipTransform.eye, e, sizeof(e));
		double md[16];
		for (int i = 0; i < 16; ++i) {
			md[i] = m[i];
		}
		clipTransform.valid = Render::D3D::Invert(md, clipTransform.inverse);
		return clipTransform.valid;
	}

	// Vertex shaders project camera-relative positions v = P - eye with row vectors:
	// clip = M^T (v, 1). A world plane L becomes (n, n.eye + d) on v, and q = M^-1 L satisfies
	// q . clip = L . (v, 1).
	void ToClipSpace(const Plane& a_plane, float a_out[4])
	{
		const double l[4] = { a_plane.n[0], a_plane.n[1], a_plane.n[2],
			a_plane.n[0] * clipTransform.eye[0] + a_plane.n[1] * clipTransform.eye[1] + a_plane.n[2] * clipTransform.eye[2] + a_plane.d };
		for (int row = 0; row < 4; ++row) {
			double sum = 0.0;
			for (int column = 0; column < 4; ++column) {
				sum += clipTransform.inverse[row * 4 + column] * l[column];
			}
			a_out[row] = static_cast<float>(sum);
		}
	}

	void SetPiece(ClipPiece& a_piece, const Plane* a_planes, int a_count)
	{
		for (int i = 0; i < Core::Doorway::kMaxPlanes; ++i) {
			if (i < a_count) {
				ToClipSpace(a_planes[i], a_piece.planes[i]);
			} else {
				a_piece.planes[i][0] = a_piece.planes[i][1] = a_piece.planes[i][2] = 0.0f;
				a_piece.planes[i][3] = 1.0f;
			}
		}
	}

	// The camera's eye in world coordinates, as the vertex shaders see it.
	RE::NiPoint3 Eye()
	{
		const auto eye = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData().posAdjust.getEye();
		return { eye.x, eye.y, eye.z };
	}

	void Apply(const Cut& a_cut)
	{
		switch (a_cut.kind) {
		case Cut::Kind::kWhole:
			return;
		case Cut::Kind::kWholeSeen:
			++stats.passesWhole;
			return;
		case Cut::Kind::kLeftOut:
			job.active = job.skip = true;
			++stats.passesLeftOut;
			return;
		case Cut::Kind::kPieces:
			if (!UpdateClipTransform()) {
				return;
			}
			job.active = true;
			job.skip = a_cut.count == 0;
			job.pieces = a_cut.count;
			for (int i = 0; i < a_cut.count; ++i) {
				SetPiece(job.piece[i], a_cut.pieces[i].planes, a_cut.pieces[i].count);
			}
			++stats.passesCut;
			return;
		}
	}

	void PlanPortalPass(const RE::BSRenderPass* a_pass)
	{
		std::optional<ClipPlane> clip;
		{
			std::scoped_lock lock(stateLock);
			clip = portalClip;
		}
		if (!clip) {
			return;
		}
		const auto& bound = a_pass->geometry->worldBound;
		Apply(Core::Doorway::CutAt(Core::Doorway::KeptSide(*clip), ToCore(bound.center), bound.radius));
	}

	void PlanMerged(const RE::BSRenderPass* a_pass)
	{
		std::optional<Door> d;
		float               passage;
		{
			std::scoped_lock lock(stateLock);
			d = door;
			passage = gatePassage;
		}
		if (!d || !Pass::InMainWorldPass() || Render::Scene().IsPlayerShape(a_pass->geometry)) {
			return;
		}
		const auto& bound = a_pass->geometry->worldBound;
		Apply(Core::Doorway::CutThroughDoorway(*d, ToCore(Eye()), passage, Render::Scene().IsMergedShape(a_pass->geometry), ToCore(bound.center), bound.radius));
	}

	void BeginPass(const RE::BSRenderPass* a_pass)
	{
		job.active = false;
		job.skip = false;
		job.pieces = 0;
		if (!ready || !a_pass || !a_pass->geometry) {
			return;
		}
		const bool portalScenePass = Pass::InPortalScene();
		if (portalPass && portalScenePass) {
			PlanPortalPass(a_pass);
		} else if (!portalScenePass && !precipitationWhole) {
			PlanMerged(a_pass);
		}
	}

	void EndPass()
	{
		job.active = false;
	}

	struct Element
	{
		std::string                name;
		UINT                       index = 0;
		UINT                       reg = 0;
		BYTE                       mask = 0;
		D3D_REGISTER_COMPONENT_TYPE type = D3D_REGISTER_COMPONENT_UNKNOWN;
		D3D_NAME                   systemValue = D3D_NAME_UNDEFINED;

		bool operator==(const Element& a_other) const
		{
			return _stricmp(name.c_str(), a_other.name.c_str()) == 0 && index == a_other.index && reg == a_other.reg && mask == a_other.mask && type == a_other.type;
		}
	};

	std::optional<std::vector<Element>> Signature(const void* a_bytecode, std::size_t a_size, bool a_output)
	{
		ID3D11ShaderReflection* reflection = nullptr;
		if (FAILED(::D3DReflect(a_bytecode, a_size, IID_ID3D11ShaderReflection, reinterpret_cast<void**>(&reflection))) || !reflection) {
			return std::nullopt;
		}
		D3D11_SHADER_DESC desc{};
		reflection->GetDesc(&desc);
		std::vector<Element> elements;
		const UINT           count = a_output ? desc.OutputParameters : desc.InputParameters;
		for (UINT i = 0; i < count; ++i) {
			D3D11_SIGNATURE_PARAMETER_DESC parameter{};
			if (FAILED(a_output ? reflection->GetOutputParameterDesc(i, &parameter) : reflection->GetInputParameterDesc(i, &parameter))) {
				reflection->Release();
				return std::nullopt;
			}
			if (parameter.Stream != 0) {
				continue;
			}
			elements.push_back({ parameter.SemanticName, parameter.SemanticIndex, parameter.Register, parameter.Mask, parameter.ComponentType, parameter.SystemValueType });
		}
		reflection->Release();
		return elements;
	}

	std::string Key(const std::vector<Element>& a_elements)
	{
		std::string key;
		for (const auto& e : a_elements) {
			key += std::format("{}{}:{}:{:X}:{};", e.name, e.index, e.reg, e.mask, static_cast<int>(e.type));
		}
		return key;
	}

	// HLSL for a pass-through geometry shader over these outputs, or nothing if they can't be
	// passed through (already clipping, or nothing to clip with).
	std::optional<std::string> GeometryShaderSource(std::vector<Element> a_elements)
	{
		std::ranges::sort(a_elements, [](const Element& a, const Element& b) {
			return a.reg != b.reg ? a.reg < b.reg : std::countr_zero(static_cast<unsigned>(a.mask)) < std::countr_zero(static_cast<unsigned>(b.mask));
		});
		std::string members;
		std::string copies;
		int         position = -1;
		for (int i = 0; i < static_cast<int>(a_elements.size()); ++i) {
			const auto& e = a_elements[i];
			const int   components = std::popcount(static_cast<unsigned>(e.mask));
			if (components == 0) {
				return std::nullopt;
			}
			const char* scalar = e.type == D3D_REGISTER_COMPONENT_UINT32 ? "uint" : e.type == D3D_REGISTER_COMPONENT_SINT32 ? "int" : e.type == D3D_REGISTER_COMPONENT_FLOAT32 ? "float" : nullptr;
			if (!scalar) {
				return std::nullopt;
			}
			std::string semantic;
			switch (e.systemValue) {
			case D3D_NAME_UNDEFINED:
				semantic = std::format("{}{}", e.name, e.index);
				break;
			case D3D_NAME_POSITION:
				semantic = "SV_Position";
				position = i;
				break;
			default:
				return std::nullopt;  // clip/cull distances or other system values: leave alone
			}
			members += std::format("\t{}{} f{} : {};\n", scalar, components == 1 ? std::string() : std::to_string(components), i, semantic);
			copies += std::format("\t\to.f{0} = input[i].f{0};\n", i);
		}
		if (position < 0) {
			return std::nullopt;
		}
		return std::format(
			"cbuffer SeamlessClip : register(b0) {{ float4 SeamlessPlanes[6]; }};\n"
			"struct V {{\n{0}}};\n"
			"struct G {{\n{0}\tfloat4 seamlessClip0 : SV_ClipDistance0;\n\tfloat2 seamlessClip1 : SV_ClipDistance1;\n}};\n"
			"[maxvertexcount(3)]\n"
			"void main(triangle V input[3], inout TriangleStream<G> stream) {{\n"
			"\t[unroll] for (uint i = 0; i < 3; ++i) {{\n"
			"\t\tG o;\n{1}"
			"\t\tfloat4 p = input[i].f{2};\n"
			"\t\to.seamlessClip0 = float4(dot(SeamlessPlanes[0], p), dot(SeamlessPlanes[1], p), dot(SeamlessPlanes[2], p), dot(SeamlessPlanes[3], p));\n"
			"\t\to.seamlessClip1 = float2(dot(SeamlessPlanes[4], p), dot(SeamlessPlanes[5], p));\n"
			"\t\tstream.Append(o);\n"
			"\t}}\n"
			"}}\n",
			members, copies, position);
	}

	struct ShaderCache
	{
		std::unordered_map<const void*, ID3D11GeometryShader*>      byVertexShader;  // nullptr: no cut possible
		std::unordered_map<std::string, ID3D11GeometryShader*>      bySignature;
		std::uint32_t                                               compiled = 0;
		std::uint32_t                                               failed = 0;
		double                                                      compileMs = 0.0;
	} shaders;

	ID3D11GeometryShader* Build(const std::vector<Element>& a_outputs)
	{
		const auto source = GeometryShaderSource(a_outputs);
		if (!source) {
			return nullptr;
		}
		const auto  started = std::chrono::steady_clock::now();
		std::string errors;
		auto*       code = Render::D3D::Compile(*source, "SeamlessClipGS", "main", "gs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, &errors);
		shaders.compileMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		if (!code) {
			if (shaders.failed++ < 10) {
				logger::warn("native mask: geometry shader didn't compile: {}\n{}", errors, *source);
			}
			return nullptr;
		}
		// It must take exactly what the vertex shader gives, and give the pixel shader the same in
		// the same places; the compiler's packing is assumed to repeat the original's.
		const auto inputs = Signature(code->GetBufferPointer(), code->GetBufferSize(), false);
		const auto outputs = Signature(code->GetBufferPointer(), code->GetBufferSize(), true);
		auto       sorted = a_outputs;
		const auto order = [](const Element& a, const Element& b) { return a.reg != b.reg ? a.reg < b.reg : a.mask < b.mask; };
		std::ranges::sort(sorted, order);
		bool matches = inputs && outputs && inputs->size() == sorted.size() && outputs->size() >= sorted.size();
		if (matches) {
			auto in = *inputs;
			std::ranges::sort(in, order);
			auto out = std::vector<Element>(outputs->begin(), outputs->end());
			std::erase_if(out, [](const Element& e) { return e.systemValue == D3D_NAME_CLIP_DISTANCE; });
			std::ranges::sort(out, order);
			matches = in == sorted && out == sorted;
		}
		ID3D11GeometryShader* shader = nullptr;
		if (!matches) {
			if (shaders.failed++ < 10) {
				logger::warn("native mask: geometry shader's signature doesn't match its vertex shader's (packing differs); those shapes aren't cut:\n{}", *source);
			}
		} else if (FAILED(Render::D3D::Device()->CreateGeometryShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader))) {
			shader = nullptr;
			++shaders.failed;
		} else {
			++shaders.compiled;
		}
		code->Release();
		return shader;
	}

	ID3D11GeometryShader* GeometryShaderFor(const RE::BSGraphics::VertexShader* a_vertexShader)
	{
		if (!a_vertexShader) {
			return nullptr;
		}
		if (const auto it = shaders.byVertexShader.find(a_vertexShader); it != shaders.byVertexShader.end()) {
			return it->second;
		}
		ID3D11GeometryShader* shader = nullptr;
		const auto*           bytecode = a_vertexShader->rawBytecode;
		const auto            size = a_vertexShader->byteCodeSize;
		// The engine keeps the bytecode right behind the shader; DXBC starts with its magic.
		if (size > 4 && std::memcmp(bytecode, "DXBC", 4) == 0) {
			if (const auto outputs = Signature(bytecode, size, true)) {
				const auto key = Key(*outputs);
				if (const auto it = shaders.bySignature.find(key); it != shaders.bySignature.end()) {
					shader = it->second;
				} else {
					shader = Build(*outputs);
					shaders.bySignature.emplace(key, shader);
				}
			}
		}
		shaders.byVertexShader.emplace(a_vertexShader, shader);
		return shader;
	}

	ID3D11Buffer* planeBuffer = nullptr;

	bool EnsurePlaneBuffer()
	{
		if (planeBuffer) {
			return true;
		}
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = sizeof(ClipPiece);
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		return SUCCEEDED(Render::D3D::Device()->CreateBuffer(&desc, nullptr, &planeBuffer));
	}

	using DrawIndexed_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
	using Draw_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
	using DrawIndexedInstanced_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
	using DrawInstanced_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);

	void STDMETHODCALLTYPE DrawIndexedHook(ID3D11DeviceContext*, UINT, UINT, INT);
	void STDMETHODCALLTYPE DrawHook(ID3D11DeviceContext*, UINT, UINT);
	void STDMETHODCALLTYPE DrawIndexedInstancedHook(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
	void STDMETHODCALLTYPE DrawInstancedHook(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);

	// ID3D11DeviceContext slots: DrawIndexed 12, Draw 13, DrawIndexedInstanced 20, DrawInstanced 21.
	constexpr std::size_t kDrawIndexed = 12, kDraw = 13, kDrawIndexedInstanced = 20, kDrawInstanced = 21;

	// The immediate context's function table lives in the context object itself and the runtime
	// writes it again (a hook put into it once at startup was gone by the time the game was loaded,
	// after 17 draws). So the table is checked, and the hooks put back, before every draw that
	// needs them; what each table held before is kept per table, in case the runtime switches
	// between several.
	struct Table
	{
		void**                 slots = nullptr;
		DrawIndexed_t          drawIndexed = nullptr;
		Draw_t                 draw = nullptr;
		DrawIndexedInstanced_t drawIndexedInstanced = nullptr;
		DrawInstanced_t        drawInstanced = nullptr;
	};
	std::vector<Table> tables;
	std::uint32_t      rehooked = 0;  // diagnostics: times a table had lost the hooks

	Table* TableOf(ID3D11DeviceContext* a_context)
	{
		auto** slots = *reinterpret_cast<void***>(a_context);
		for (auto& table : tables) {
			if (table.slots == slots) {
				return &table;
			}
		}
		return nullptr;
	}

	template <class T>
	void Put(void** a_slots, std::size_t a_index, T a_hook, T& a_original)
	{
		if (a_slots[a_index] == reinterpret_cast<void*>(a_hook)) {
			return;
		}
		a_original = reinterpret_cast<T>(a_slots[a_index]);
		REL::safe_write(reinterpret_cast<std::uintptr_t>(&a_slots[a_index]), reinterpret_cast<std::uintptr_t>(a_hook));
	}

	void EnsureHooks(ID3D11DeviceContext* a_context)
	{
		if (!a_context) {
			return;
		}
		auto** slots = *reinterpret_cast<void***>(a_context);
		if (slots[kDrawIndexed] == reinterpret_cast<void*>(&DrawIndexedHook) && slots[kDraw] == reinterpret_cast<void*>(&DrawHook) &&
			slots[kDrawIndexedInstanced] == reinterpret_cast<void*>(&DrawIndexedInstancedHook) && slots[kDrawInstanced] == reinterpret_cast<void*>(&DrawInstancedHook)) {
			return;
		}
		auto* table = TableOf(a_context);
		if (!table) {
			table = &tables.emplace_back();
			table->slots = slots;
		} else {
			++rehooked;
		}
		Put(slots, kDrawIndexed, &DrawIndexedHook, table->drawIndexed);
		Put(slots, kDraw, &DrawHook, table->draw);
		Put(slots, kDrawIndexedInstanced, &DrawIndexedInstancedHook, table->drawIndexedInstanced);
		Put(slots, kDrawInstanced, &DrawInstancedHook, table->drawInstanced);
	}

	// The context the engine draws with: its draw functions (e.g. ID 77282) read this global.
	ID3D11DeviceContext* DrawingContext()
	{
		static REL::Relocation<ID3D11DeviceContext**> context{ REL::RelocationID(524771, 411391) };
		return *context.get();
	}

	std::uint64_t drawCalls = 0;       // diagnostics: every draw through the hooks
	std::uint64_t drawCallsInJob = 0;  // ... of them while a cut was planned

	template <class F>
	void Masked(ID3D11DeviceContext* a_context, F&& a_draw)
	{
		++drawCalls;
		drawCallsInJob += job.active ? 1 : 0;
		if (!job.active) {
			a_draw();
			return;
		}
		if (job.skip) {
			return;
		}
		D3D11_PRIMITIVE_TOPOLOGY topology{};
		a_context->IAGetPrimitiveTopology(&topology);
		const bool triangles = topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST || topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
		auto*      vertexShader = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData().currentVertexShader;
		auto*      geometryShader = triangles ? GeometryShaderFor(vertexShader) : nullptr;
		if (!geometryShader || !EnsurePlaneBuffer()) {
			++stats.drawsUncut;
			a_draw();
			return;
		}
		ID3D11GeometryShader* previousShader = nullptr;
		ID3D11Buffer*         previousBuffer = nullptr;
		a_context->GSGetShader(&previousShader, nullptr, nullptr);
		a_context->GSGetConstantBuffers(0, 1, &previousBuffer);
		a_context->GSSetShader(geometryShader, nullptr, 0);
		a_context->GSSetConstantBuffers(0, 1, &planeBuffer);
		for (int i = 0; i < job.pieces; ++i) {
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(a_context->Map(planeBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				continue;
			}
			std::memcpy(mapped.pData, &job.piece[i], sizeof(ClipPiece));
			a_context->Unmap(planeBuffer, 0);
			a_draw();
			++stats.piecesDrawn;
		}
		++stats.drawsCut;
		a_context->GSSetShader(previousShader, nullptr, 0);
		a_context->GSSetConstantBuffers(0, 1, &previousBuffer);
		if (previousShader) {
			previousShader->Release();
		}
		if (previousBuffer) {
			previousBuffer->Release();
		}
	}

	// A call through a table the hooks were put into, or (another mod having copied our pointer
	// into a table of its own) any table: then the first one's originals are the best guess.
	const Table& Originals(ID3D11DeviceContext* a_context)
	{
		const auto* table = TableOf(a_context);
		return table ? *table : tables.front();
	}

	void STDMETHODCALLTYPE DrawIndexedHook(ID3D11DeviceContext* a_context, UINT a_count, UINT a_start, INT a_base)
	{
		const auto original = Originals(a_context).drawIndexed;
		Masked(a_context, [&] { original(a_context, a_count, a_start, a_base); });
	}

	void STDMETHODCALLTYPE DrawHook(ID3D11DeviceContext* a_context, UINT a_count, UINT a_start)
	{
		const auto original = Originals(a_context).draw;
		Masked(a_context, [&] { original(a_context, a_count, a_start); });
	}

	void STDMETHODCALLTYPE DrawIndexedInstancedHook(ID3D11DeviceContext* a_context, UINT a_count, UINT a_instances, UINT a_start, INT a_base, UINT a_startInstance)
	{
		const auto original = Originals(a_context).drawIndexedInstanced;
		Masked(a_context, [&] { original(a_context, a_count, a_instances, a_start, a_base, a_startInstance); });
	}

	void STDMETHODCALLTYPE DrawInstancedHook(ID3D11DeviceContext* a_context, UINT a_count, UINT a_instances, UINT a_start, UINT a_startInstance)
	{
		const auto original = Originals(a_context).drawInstanced;
		Masked(a_context, [&] { original(a_context, a_count, a_instances, a_start, a_startInstance); });
	}

	// BSShader vfuncs 6 (SetupGeometry) and 7 (RestoreGeometry) bracket each pass's draw in the
	// batch renderer (SetupAndDrawPass, ID 107644): the cut is planned at the first and dropped at
	// the second, and the draw calls between them get it.
	template <int Shader>
	struct SetupGeometryHook
	{
		static void Thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, std::uint32_t a_flags)
		{
			BeginPass(a_pass);
			if (job.active) {
				EnsureHooks(DrawingContext());
			}
			if constexpr (Shader == 0) {
				const LightingSwap swap(a_shader, a_pass);
				func(a_shader, a_pass, a_flags);
			} else {
				func(a_shader, a_pass, a_flags);
			}
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	template <int Shader>
	struct RestoreGeometryHook
	{
		static void Thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, std::uint32_t a_flags)
		{
			func(a_shader, a_pass, a_flags);
			EndPass();
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	template <int Shader>
	void HookShader(REL::VariantID a_vtable)
	{
		REL::Relocation<std::uintptr_t> vtable{ a_vtable };
		SetupGeometryHook<Shader>::func = vtable.write_vfunc(0x6, SetupGeometryHook<Shader>::Thunk);
		RestoreGeometryHook<Shader>::func = vtable.write_vfunc(0x7, RestoreGeometryHook<Shader>::Thunk);
	}

}

namespace Render::Native
{
	bool Install()
	{
		auto* context = DrawingContext();
		if (!context) {
			logger::error("native renderer: no device context; seamless rendering off");
			return false;
		}
		// Everything the cut needs from the engine's structures, checked once.
		SEAMLESS_ASSERT(offsetof(RE::BSGraphics::VertexShader, rawBytecode) == 0x68);
		// With ENB the engine's context is ENB's; its table is hooked the same way.
		EnsureHooks(context);
		HookShader<0>(RE::VTABLE_BSLightingShader[0]);
		HookShader<1>(RE::VTABLE_BSUtilityShader[0]);
		HookShader<2>(RE::VTABLE_BSEffectShader[0]);
		// The weather's rain and snow (the particle shader's cube technique): cut at a merged doorway like
		// any shape. Other particles of the world and of a merged room are cut with them.
		HookShader<3>(RE::VTABLE_BSParticleShader[0]);
		Render::Native::InstallLightingHooks();
		ready = true;
		logger::info("native renderer: shader setup and draw hooks installed (context {:X})", reinterpret_cast<std::uintptr_t>(context));
		return true;
	}

	bool Ready()
	{
		return ready;
	}

	void SetDoor(const std::optional<DoorRect>& a_door)
	{
		std::scoped_lock lock(stateLock);
		door = a_door;
	}

	void SetGatePassage(float a_depth)
	{
		std::scoped_lock lock(stateLock);
		gatePassage = a_depth;
	}

	bool MergedDoorSet()
	{
		std::scoped_lock lock(stateLock);
		return door.has_value();
	}

	void SetPortalClip(const std::optional<ClipPlane>& a_plane)
	{
		std::scoped_lock lock(stateLock);
		portalClip = a_plane;
	}

	void SetPrecipitationWhole(bool a_whole)
	{
		precipitationWhole = a_whole;
	}

	void SetPortalPass(bool a_active)
	{
		portalPass = a_active;
		if (a_active) {
			++stats.portalPasses;
		}
		job.active = false;
	}

	json DescribeMask()
	{
		std::optional<Door> d;
		{
			std::scoped_lock lock(stateLock);
			d = door;
		}
		auto*  context = DrawingContext();
		auto** vtable = context ? *reinterpret_cast<void***>(context) : nullptr;
		return {
			{ "context", std::format("{:X}", reinterpret_cast<std::uintptr_t>(context)) },
			{ "contextVtable", std::format("{:X}", reinterpret_cast<std::uintptr_t>(vtable)) },
			{ "drawIndexedHooked", vtable && vtable[12] == reinterpret_cast<void*>(&DrawIndexedHook) },
			{ "functionTables", tables.size() },
			{ "rehooked", rehooked },
			{ "ready", ready },
			{ "doorSet", d.has_value() },
			{ "cameraMerged", d && d->cameraMerged },
			{ "portalPass", portalPass.load() },
			{ "passesCut", stats.passesCut },
			{ "passesLeftOut", stats.passesLeftOut },
			{ "passesWhole", stats.passesWhole },
			{ "drawsCut", stats.drawsCut },
			{ "piecesDrawn", stats.piecesDrawn },
			{ "drawsUncut", stats.drawsUncut },
			{ "portalPasses", stats.portalPasses },
			{ "drawCalls", drawCalls },
			{ "drawCallsInJob", drawCallsInJob },
			{ "geometryShaders", shaders.compiled },
			{ "geometryShaderFailures", shaders.failed },
			{ "vertexShadersSeen", shaders.byVertexShader.size() },
			{ "compileMs", shaders.compileMs },
		};
	}
}
