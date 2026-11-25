#include "Core/Crossing/Sequence.h"

namespace Core::Crossing
{
	void Sequence::FallBack(std::vector<Action>& a_out)
	{
		if (prewarmAsked) {
			a_out.push_back({ .kind = ActionKind::CancelPrewarm, .space = target.to });
		}
		a_out.push_back({ .kind = ActionKind::StartCover, .cover = Cover::EngineLoad });
		a_out.push_back({ .kind = ActionKind::VanillaDoor, .space = target.to, .door = target.door });
		Enter(Phase::Fallback);
	}

	void Sequence::BeginCross(std::vector<Action>& a_out)
	{
		a_out.push_back({ .kind = ActionKind::StartCover, .cover = Cover::DoorSwing });
		a_out.push_back({ .kind = ActionKind::Teleport, .space = target.to, .door = target.door });
		Enter(Phase::Cross);
	}

	std::vector<Action> Sequence::Handle(const Event& a_event)
	{
		std::vector<Action> out;
		switch (a_event.kind) {
		case EventKind::Begin:
			if (phase == Phase::Idle) {
				target = a_event.target;
				prewarmAsked = false;
				pendingCross = false;
				Enter(Phase::Approach);
			}
			break;
		case EventKind::Near:
			if (phase == Phase::Approach) {
				out.push_back({ .kind = ActionKind::PrewarmSpace, .space = target.to });
				prewarmAsked = true;
				Enter(Phase::Prewarm);
			}
			break;
		case EventKind::Prewarmed:
			if (phase == Phase::Prewarm) {
				out.push_back({ .kind = ActionKind::OpenDoor, .door = target.door });
				Enter(Phase::Open);
			}
			break;
		case EventKind::DoorOpened:
			if (phase == Phase::Open) {
				Enter(Phase::Commit);
				if (pendingCross) {
					BeginCross(out);
				}
			}
			break;
		case EventKind::AtThreshold:
			if (phase == Phase::Commit) {
				BeginCross(out);
			} else if (phase == Phase::Approach || phase == Phase::Prewarm || phase == Phase::Open) {
				// Walked up faster than the way was made ready: crosses as soon as it is.
				pendingCross = true;
			}
			break;
		case EventKind::Crossed:
			if (phase == Phase::Cross) {
				out.push_back({ .kind = ActionKind::EndCover });
				Enter(Phase::Settle);
			} else if (phase == Phase::Fallback) {
				out.push_back({ .kind = ActionKind::EndCover });
				Enter(Phase::Idle);
			}
			break;
		case EventKind::Settled:
			if (phase == Phase::Settle) {
				out.push_back({ .kind = ActionKind::ReleaseSpace, .space = target.from });
				Enter(Phase::Release);
			}
			break;
		case EventKind::Released:
			if (phase == Phase::Release) {
				Enter(Phase::Idle);
			}
			break;
		case EventKind::Abort:
			// Once the player is moved there is no going back; before that, nothing was committed.
			if (phase == Phase::Approach || phase == Phase::Prewarm || phase == Phase::Open || phase == Phase::Commit) {
				if (prewarmAsked) {
					out.push_back({ .kind = ActionKind::CancelPrewarm, .space = target.to });
				}
				Enter(Phase::Idle);
			}
			break;
		case EventKind::Failed:
			if (phase == Phase::Prewarm || phase == Phase::Open || phase == Phase::Cross) {
				FallBack(out);
			}
			break;
		}
		return out;
	}

	std::vector<Action> Sequence::Tick(float a_delta)
	{
		std::vector<Action> out;
		elapsed += a_delta;
		switch (phase) {
		case Phase::Prewarm:
			if (elapsed >= timeouts.prewarm) {
				FallBack(out);
			}
			break;
		case Phase::Open:
			if (elapsed >= timeouts.open) {
				FallBack(out);
			}
			break;
		case Phase::Commit:
			if (elapsed >= timeouts.commit) {
				out = Handle({ EventKind::Abort });
			}
			break;
		case Phase::Cross:
			if (elapsed >= timeouts.cross) {
				FallBack(out);
			}
			break;
		case Phase::Settle:
			if (elapsed >= timeouts.settle) {
				out = Handle({ EventKind::Settled });
			}
			break;
		case Phase::Release:
			if (elapsed >= timeouts.release) {
				Enter(Phase::Idle);
			}
			break;
		case Phase::Fallback:
			if (elapsed >= timeouts.fallback) {
				out.push_back({ .kind = ActionKind::EndCover });
				Enter(Phase::Idle);
			}
			break;
		default:
			break;
		}
		return out;
	}
}
