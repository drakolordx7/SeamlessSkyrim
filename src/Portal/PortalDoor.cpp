#include "Portal/PortalDoor.h"

#include "Doors/Geometry.h"
#include "Doors/Hold.h"
#include "Doors/Pair.h"
#include "Doors/Policy.h"
#include "Doors/Settings.h"
#include "Doors/Swing.h"
#include "Portal/Gates.h"
#include "Portal/PortalManager.h"
#include "Portal/StandIns.h"

namespace Portal::PortalDoor
{
	namespace
	{
		// The source door stands open while its portal is shown: the player can walk into the doorway.
		Doors::OpenState openDoor;
		Doors::Swing     openSwing;  // how far the panel has swung open
		RE::FormID       openDoorSwung = 0;  // the door Update swings: the source, or the linked door seen through it
		bool             doorWanted = false;  // Doors::Wanted
	}

	void SetWanted(bool a_wanted)
	{
		doorWanted = a_wanted;
	}

	bool Wanted()
	{
		return doorWanted;
	}

	void Update(RE::TESObjectREFR* a_door, const RE::NiPoint3& a_inward, float a_delta)
	{
		if (!a_door) {
			return;
		}
		// A door swings open into the space it opens into (Doors::SwingingDoor): the one the player
		// is in, off the portal image on its face (with the panel left in place, the two fought over
		// the same pixels), or the space behind, the linked door swung in the portal's picture and
		// this one hidden. A city gate too; its leaves were hidden once, and the gateway stood
		// empty.
		auto*      here = a_door->GetParentCell();
		const auto portal = Manager::CurrentPortal();
		const bool gate = Gates::IsGate(here, portal ? portal->cell : nullptr);
		auto*      linked = portal ? RE::TESForm::LookupByID<RE::TESObjectREFR>(portal->linkedDoor) : nullptr;
		const bool farSwings = linked && Portal::Doors::SwingingDoor(a_door, linked) == linked->GetFormID();
		auto*      swungDoor = farSwings ? linked : a_door;
		const auto swung = swungDoor->GetFormID();
		if (openDoor.door != a_door->GetFormID() || openDoorSwung != swung) {
			Portal::Doors::RestoreLeaves(openDoor.door);
			Portal::Doors::RestoreLeaves(openDoorSwung);
			StandIns::TakeOverDoor(openSwing, swung);
			openDoorSwung = swung;
		}
		Portal::Doors::Swing::Reclaim(swung);
		// Timed by the door that swings; the space behind, holding it, may still be loading.
		auto* timed = swungDoor->Get3D() ? swungDoor : a_door;
		// Only from shut: the picture that makes a door open from afar is taken while it swings open
		// the first time, and snapped open then it jumped.
		const bool wasShut = openSwing.Step(timed, doorWanted, a_delta);
		if (doorWanted && wasShut && linked && Portal::Doors::OpenFromAfar(a_door, linked)) {
			openSwing.SnapOpen();
		}
		if (openSwing.Shut()) {
			// Swung shut: an ordinary door again, solid, a gate shown. The portal isn't drawn on
			// it any more (Shut); its quad lies on the panel's face.
			Portal::Doors::RestoreLeaves(a_door->GetFormID());
			Portal::Doors::RestoreLeaves(openDoorSwung);
			Portal::Doors::Close(openDoor);
			Gates::ReleaseFar();
			return;
		}
		const auto settings = linked ? Portal::Doors::CorrectionDoor(a_door, linked)->GetFormID() : a_door->GetFormID();
		const bool hide = farSwings;
		if (openDoor.door == a_door->GetFormID() && openDoor.hidden != hide) {
			Portal::Doors::RestoreLeaves(openDoor.door);
		}
		Portal::Doors::Open(a_door, openDoor, { .hide = hide, .passable = gate });
		if (gate && linked) {
			Gates::HoldFar(*portal, linked, farSwings);
		} else {
			Gates::ReleaseFar();
		}
		if (farSwings) {
			// Into the space behind, in its own coordinates (the portal pass draws it where it is).
			if (const auto mapping = Portal::Doors::TryCompute(*portal)) {
				openSwing.Turn(timed, linked, mapping->toLinked.rotate * mapping->inward, settings);
			}
		} else if (!hide) {
			openSwing.Turn(timed, a_door, RE::NiPoint3{ -a_inward.x, -a_inward.y, 0.0f }, settings);
		} else {
			Portal::Doors::RestoreLeaves(a_door->GetFormID());
		}
	}

	void Close()
	{
		if (openDoor.door) {
			openSwing.LetGo(openDoorSwung ? openDoorSwung : openDoor.door);
		}
		openSwing.Reset();
		openDoorSwung = 0;
		Portal::Doors::Close(openDoor);
		Gates::ReleaseFar();
	}

	bool Holds(RE::FormID a_door)
	{
		return openDoor.door == a_door;
	}

	bool Shut()
	{
		return openSwing.Shut();
	}

	float Amount()
	{
		return openSwing.Amount();
	}

	RE::FormID Swung()
	{
		return openDoorSwung;
	}
}
