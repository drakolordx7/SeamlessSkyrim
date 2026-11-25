#pragma once

namespace Portal
{
	// Hair and cloth simulated by SMP (hdtSMP64 / FSMP) keep their last positions for a few frames
	// after a teleport, drawn stretched from the old place to the new one. SMP resets its
	// simulation when the loading screen closes, which a seamless trip never shows; it is told
	// the same way, with the menu events alone. Does nothing without SMP loaded.
	void ResetClothPhysics();
}
