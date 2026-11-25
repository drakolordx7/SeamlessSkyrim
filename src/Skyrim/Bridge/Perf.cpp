#include "Skyrim/Bridge/Perf.h"

#include <d3d11.h>

namespace
{
	constexpr std::size_t kFrames = 300;

	// One value per frame for the last kFrames frames; `seen` tells frames it had none apart.
	struct Series
	{
		std::string                name;
		double                     current = 0.0;
		bool                       touched = false;
		std::array<float, kFrames> values{};
		std::array<bool, kFrames>  seen{};
	};

	std::atomic_bool    on = false;
	std::mutex          lock;
	std::vector<Series> cpu;  // ms
	std::vector<Series> counts;
	std::vector<Series> gpu;  // ms, and thousands of invocations
	std::size_t         frame = 0;

	Series& Find(std::vector<Series>& a_list, std::string_view a_name)
	{
		for (auto& s : a_list) {
			if (s.name == a_name) {
				return s;
			}
		}
		return a_list.emplace_back(Series{ std::string(a_name) });
	}

	std::int64_t Ticks()
	{
		LARGE_INTEGER t;
		::QueryPerformanceCounter(&t);
		return t.QuadPart;
	}

	double TicksToMs(std::int64_t a_ticks)
	{
		static const double perMs = [] {
			LARGE_INTEGER f;
			::QueryPerformanceFrequency(&f);
			return static_cast<double>(f.QuadPart) / 1000.0;
		}();
		return static_cast<double>(a_ticks) / perMs;
	}

	// GPU: a ring of frames, each with a disjoint query and, per section, two timestamps and the
	// pipeline statistics. A frame's results are read when its slot comes round again, by then
	// long finished on the GPU, so nothing waits.
	constexpr std::size_t kSlots = 8;
	struct Section
	{
		std::string  name;
		ID3D11Query* begin;
		ID3D11Query* end;
		ID3D11Query* stats;
		bool         begun = false;
		bool         ended = false;
	};
	struct Slot
	{
		ID3D11Query*         disjoint = nullptr;
		bool                 open = false;
		std::vector<Section> sections;
	};
	std::array<Slot, kSlots> slots;
	std::size_t              slotIndex = 0;

	ID3D11DeviceContext* Context()
	{
		return reinterpret_cast<ID3D11DeviceContext*>(RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context);
	}

	ID3D11Query* MakeQuery(D3D11_QUERY a_type)
	{
		ID3D11Device* device = nullptr;
		Context()->GetDevice(&device);
		const D3D11_QUERY_DESC desc{ a_type, 0 };
		ID3D11Query*           query = nullptr;
		device->CreateQuery(&desc, &query);
		device->Release();
		return query;
	}

	Section& SectionOf(Slot& a_slot, const char* a_name)
	{
		for (auto& s : a_slot.sections) {
			if (s.name == a_name) {
				return s;
			}
		}
		return a_slot.sections.emplace_back(Section{ a_name, MakeQuery(D3D11_QUERY_TIMESTAMP), MakeQuery(D3D11_QUERY_TIMESTAMP), MakeQuery(D3D11_QUERY_PIPELINE_STATISTICS) });
	}

	void Put(std::string_view a_name, std::size_t a_at, double a_value)
	{
		auto& s = Find(gpu, a_name);
		s.values[a_at] = static_cast<float>(a_value);
		s.seen[a_at] = true;
	}

	// Under the lock.
	void ReadBack(Slot& a_slot, std::size_t a_at)
	{
		auto* context = Context();
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
		const bool timed = context->GetData(a_slot.disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && !disjoint.Disjoint;
		for (auto& s : a_slot.sections) {
			if (s.begun && s.ended) {
				UINT64 b = 0, e = 0;
				if (timed && context->GetData(s.begin, &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
					context->GetData(s.end, &e, sizeof(e), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
					Put(s.name, a_at, static_cast<double>(e - b) * 1000.0 / static_cast<double>(disjoint.Frequency));
				}
				D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
				if (context->GetData(s.stats, &stats, sizeof(stats), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
					Put(s.name + " kPixelShaded", a_at, stats.PSInvocations / 1000.0);
				}
			}
			s.begun = false;
			s.ended = false;
		}
	}

	json Summary(const std::vector<Series>& a_list, std::size_t a_frames)
	{
		const auto round = [](double a_value) { return std::round(a_value * 1000.0) / 1000.0; };
		json       out = json::object();
		for (const auto& s : a_list) {
			std::vector<float> v;
			for (std::size_t i = 0; i < a_frames; ++i) {
				if (s.seen[i]) {
					v.push_back(s.values[i]);
				}
			}
			if (v.empty()) {
				continue;
			}
			std::ranges::sort(v);
			const double sum = std::accumulate(v.begin(), v.end(), 0.0);
			out[s.name] = { { "frames", v.size() }, { "mean", round(sum / v.size()) }, { "median", round(v[v.size() / 2]) }, { "p90", round(v[v.size() * 9 / 10]) },
				{ "max", round(v.back()) } };
		}
		return out;
	}
}

namespace Perf
{
	bool On()
	{
		return on;
	}

	void AddCpu(const char* a_name, double a_ms)
	{
		if (!on) {
			return;
		}
		std::scoped_lock l(lock);
		auto&            s = Find(cpu, a_name);
		s.current += a_ms;
		s.touched = true;
	}

	void AddCount(const char* a_name, double a_value)
	{
		if (!on) {
			return;
		}
		std::scoped_lock l(lock);
		auto&            s = Find(counts, a_name);
		s.current += a_value;
		s.touched = true;
	}

	std::int64_t ScopeBegin()
	{
		return on ? Ticks() : 0;
	}

	void ScopeEnd(const char* a_name, std::int64_t a_start)
	{
		if (a_start) {
			AddCpu(a_name, TicksToMs(Ticks() - a_start));
		}
	}

	Scope::Scope(const char* a_name) :
		name(a_name), start(ScopeBegin())
	{}

	Scope::~Scope()
	{
		ScopeEnd(name, start);
	}

	void GpuBegin(const char* a_name)
	{
		auto& slot = slots[slotIndex];
		if (!slot.open) {
			return;
		}
		auto& s = SectionOf(slot, a_name);
		if (!s.begun) {
			Context()->End(s.begin);
			Context()->Begin(s.stats);
			s.begun = true;
		}
	}

	void GpuEnd(const char* a_name)
	{
		auto& slot = slots[slotIndex];
		if (!slot.open) {
			return;
		}
		auto& s = SectionOf(slot, a_name);
		if (s.begun && !s.ended) {
			Context()->End(s.stats);
			Context()->End(s.end);
			s.ended = true;
		}
	}

	void FrameEnd()
	{
		auto& current = slots[slotIndex];
		if (current.open) {
			GpuEnd("gpu frame");
			Context()->End(current.disjoint);
			current.open = false;
		}
		static std::int64_t lastEnd = 0;
		const auto          now = Ticks();
		const auto          since = std::exchange(lastEnd, now);
		if (!on) {
			return;
		}
		std::scoped_lock l(lock);
		if (since) {
			auto& s = Find(cpu, "frame");
			s.current = TicksToMs(now - since);
			s.touched = true;
		}
		const auto at = frame++ % kFrames;
		for (auto* list : { &cpu, &counts }) {
			for (auto& s : *list) {
				s.values[at] = static_cast<float>(s.current);
				s.seen[at] = s.touched;
				s.current = 0.0;
				s.touched = false;
			}
		}
		for (auto& s : gpu) {
			s.seen[at] = false;
		}
		slotIndex = (slotIndex + 1) % kSlots;
		auto& next = slots[slotIndex];
		if (next.disjoint) {
			// From kSlots frames ago; recorded with this frame, which only shifts the window.
			ReadBack(next, at);
		} else {
			next.disjoint = MakeQuery(D3D11_QUERY_TIMESTAMP_DISJOINT);
		}
		Context()->Begin(next.disjoint);
		next.open = true;
		GpuBegin("gpu frame");
	}

	json Command(const json& a_args)
	{
		if (a_args.contains("on")) {
			on = a_args.at("on").get<bool>();
		}
		std::scoped_lock l(lock);
		const auto       frames = std::min(frame, kFrames);
		json             out{ { "on", on.load() }, { "frames", frames }, { "cpu", Summary(cpu, frames) }, { "counts", Summary(counts, frames) }, { "gpu", Summary(gpu, frames) } };
		// A moment (a crossing) frame by frame rather than summed up: the CPU scopes and counts of
		// the last `last` frames, oldest first.
		if (const auto last = std::min<std::size_t>(a_args.value("last", 0), frames)) {
			json list = json::array();
			for (std::size_t i = frame - last; i < frame; ++i) {
				json row = json::object();
				for (const auto* series : { &cpu, &counts }) {
					for (const auto& s : *series) {
						if (s.seen[i % kFrames]) {
							row[s.name] = std::round(s.values[i % kFrames] * 100.0) / 100.0;
						}
					}
				}
				list.push_back(std::move(row));
			}
			out["last"] = std::move(list);
		}
		if (a_args.value("reset", false)) {
			for (auto* list : { &cpu, &counts, &gpu }) {
				for (auto& s : *list) {
					s.seen.fill(false);
				}
			}
			frame = 0;
		}
		return out;
	}
}
