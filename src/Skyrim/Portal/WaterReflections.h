#pragma once

// A water reflection (TESWaterReflections, shared by every water at its height) keeps a plain
// pointer to the material of the water it was made for (+0x40), and TESWaterSystem::AddWater
// reads it when another water joins (ID 32179 +0x697). The game only adds water of the space the
// player is in, so the water a reflection was made for lives as long as the reflection. Here the
// street's distant water (LOD, StreetLOD) and the street's own water are added while the player
// is indoors, and join the room's reflection: the room's water went (Riften's jail, walked out
// of), the street's distant water kept the reflection, its material was freed, and attaching a
// street later (walking into the Bee and Barb) read the memory that had become a BSTriShape.
// Each reflection's material is kept alive here, through the shader property of a water that has
// it, until the reflection is gone.
namespace Portal::WaterReflections
{
	// Main thread, every frame.
	void Update();
}
