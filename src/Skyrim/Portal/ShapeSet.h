#pragma once

// A set of shapes, open-addressed: a merged room's shapes are asked about for every draw (is this
// one of them, MergedView::IsMergedShape), and std::unordered_set's nodes took ~1 ms a frame in cache
// misses with Understone Keep merged (~16000 shapes). Filled while the shapes are listed, a few
// roots per frame (MarkPass), so it is never built in one go.
namespace Portal
{
	class ShapeSet
	{
	public:
		void insert(const RE::BSGeometry* a_shape)
		{
			if ((used + 1) * 2 > table.size()) {
				Grow();
			}
			auto i = Index(a_shape);
			std::optional<std::size_t> freed;
			for (; table[i]; i = (i + 1) & Mask()) {
				if (table[i] == a_shape) {
					return;
				}
				if (table[i] == Tombstone() && !freed) {
					freed = i;
				}
			}
			if (freed) {
				i = *freed;
			} else {
				++used;
			}
			table[i] = a_shape;
			++count;
		}

		void erase(const RE::BSGeometry* a_shape)
		{
			if (const auto i = Find(a_shape)) {
				table[*i] = Tombstone();
				--count;
			}
		}

		[[nodiscard]] bool contains(const RE::BSGeometry* a_shape) const { return Find(a_shape).has_value(); }
		[[nodiscard]] std::size_t size() const { return count; }

		void clear()
		{
			table.assign(kMinSize, nullptr);
			used = count = 0;
		}

		class iterator
		{
		public:
			iterator(const std::vector<const RE::BSGeometry*>& a_table, std::size_t a_at) :
				table(&a_table), at(a_at) { Skip(); }
			const RE::BSGeometry* operator*() const { return (*table)[at]; }
			iterator& operator++()
			{
				++at;
				Skip();
				return *this;
			}
			bool operator==(const iterator&) const = default;

		private:
			void Skip()
			{
				while (at < table->size() && (!(*table)[at] || (*table)[at] == Tombstone())) {
					++at;
				}
			}
			const std::vector<const RE::BSGeometry*>* table;
			std::size_t                               at;
		};
		[[nodiscard]] iterator begin() const { return { table, 0 }; }
		[[nodiscard]] iterator end() const { return { table, table.size() }; }

	private:
		static constexpr std::size_t kMinSize = 16;  // a power of two, as every size

		static const RE::BSGeometry* Tombstone() { return reinterpret_cast<const RE::BSGeometry*>(std::uintptr_t{ 1 }); }
		[[nodiscard]] std::size_t    Mask() const { return table.size() - 1; }
		// Pointers' low bits are zeros: the high bits of a multiplicative hash.
		[[nodiscard]] std::size_t Index(const RE::BSGeometry* a_shape) const
		{
			return static_cast<std::size_t>((reinterpret_cast<std::uintptr_t>(a_shape) * 0x9E3779B97F4A7C15ull) >> (64 - std::countr_zero(table.size())));
		}

		[[nodiscard]] std::optional<std::size_t> Find(const RE::BSGeometry* a_shape) const
		{
			for (auto i = Index(a_shape); table[i]; i = (i + 1) & Mask()) {
				if (table[i] == a_shape) {
					return i;
				}
			}
			return std::nullopt;
		}

		void Grow()
		{
			auto old = std::move(table);
			table.assign(std::max(kMinSize, std::bit_ceil((count + 1) * 4)), nullptr);
			used = count = 0;
			for (const auto* shape : old) {
				if (shape && shape != Tombstone()) {
					insert(shape);
				}
			}
		}

		std::vector<const RE::BSGeometry*> table = std::vector<const RE::BSGeometry*>(kMinSize);
		std::size_t                        used = 0;  // filled or tombstones: what probing passes over
		std::size_t                        count = 0;
	};
}
