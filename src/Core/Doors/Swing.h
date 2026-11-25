#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// How far open a door's leaves are over a swing, and the state machine that drives one swing. A
// door the plugin swings moves as the game moves it: the curves are the door's own opening and
// closing animations, sampled.
namespace Core::Doors
{
	inline constexpr std::size_t kCurveSamples = 64;

	// How far open (0 shut .. 1 open) a door stands along one of its animations, sampled evenly
	// over its length.
	struct SwingCurve
	{
		float              seconds = 0.0f;
		std::vector<float> amount;

		[[nodiscard]] float At(float a_t) const
		{
			const float at = std::clamp(a_t, 0.0f, 1.0f) * static_cast<float>(amount.size() - 1);
			const auto  i = std::min(static_cast<std::size_t>(at), amount.size() - 2);
			const float f = at - static_cast<float>(i);
			return amount[i] + (amount[i + 1] - amount[i]) * f;
		}
	};

	// A door that has no animation: eased both ways over the length of a common door's.
	[[nodiscard]] SwingCurve StillCurve(bool a_opening);

	// The curve of an animation a_seconds long, from a_amountAt(t) for t in 0..1.
	template <class F>
	[[nodiscard]] SwingCurve SampleCurve(float a_seconds, F&& a_amountAt)
	{
		SwingCurve curve{ a_seconds, std::vector<float>(kCurveSamples) };
		for (std::size_t i = 0; i < kCurveSamples; ++i) {
			curve.amount[i] = a_amountAt(static_cast<float>(i) / static_cast<float>(kCurveSamples - 1));
		}
		return curve;
	}

	struct SwingCurves
	{
		SwingCurve opening;
		SwingCurve shutting;

		// a_t is how far open the swing has got, so shutting runs it from 1 down to 0: the "Close"
		// animation is played from its end backwards in a_t, forwards in time.
		[[nodiscard]] float Amount(float a_t, bool a_opening) const { return a_opening ? opening.At(a_t) : shutting.At(1.0f - a_t); }
		[[nodiscard]] float Seconds(bool a_opening) const { return (a_opening ? opening : shutting).seconds; }
	};

	// The progress of one door's swing.
	class SwingState
	{
	public:
		// 0..1, how far the swing has got.
		[[nodiscard]] float Amount() const { return amount; }
		[[nodiscard]] bool  Opening() const { return opening; }
		[[nodiscard]] bool  Shut() const { return amount <= 0.0f; }

		void SetProgress(float a_amount, bool a_opening)
		{
			amount = a_amount;
			opening = a_opening;
		}
		// Open at once: a door that stands open is never seen opening.
		void SnapOpen() { amount = 1.0f; }
		void Reset() { *this = {}; }

		// One step towards a_wanted (open or shut). The progress means a different opening on the
		// opening curve than on the shutting one, so turned round half way it is taken from where
		// the other direction's curve is as far open: kept as it was, the door jumped. True when
		// the door started out shut.
		bool Step(const SwingCurves& a_curves, bool a_wanted, float a_delta);

	private:
		float amount = 0.0f;
		bool  opening = false;
	};

	// Doors an owner has let go of, left where they stand for a moment so that the next owner of the
	// same door takes it over from there instead of swinging it open again from shut.
	class LetGoBook
	{
	public:
		static constexpr float kSeconds = 3.0f;

		void LetGo(std::uint32_t a_door, const SwingState& a_state);
		// a_state starts from where the door was let go of, if it was just now, else shut. True if it was.
		bool TakeOver(std::uint32_t a_door, SwingState& a_state);
		// The owner holds a_door again: not to be put back shut when the let-go expires.
		void Reclaim(std::uint32_t a_door);
		// The doors whose time ran out, forgotten here: the caller puts them back.
		[[nodiscard]] std::vector<std::uint32_t> Expire(float a_delta);
		// Every door held, forgotten here.
		[[nodiscard]] std::vector<std::uint32_t> TakeAll();

	private:
		struct Entry
		{
			std::uint32_t door;
			float         amount;
			bool          opening;
			float         seconds;  // since it was let go of
		};
		std::vector<Entry> entries;
	};
}
