#pragma once

#include "Skyrim/Pictures/Hlsl.h"

#include <d3d11.h>

// The doorway pictures' shaders and states: the capture (a doorway's rectangle of the frame, or of
// the portal pass, averaged into a picture; or a depth image's layer, a view from inside the
// doorway), its compression to BC6H (a compute shader) and the draw (the picture in relief in the doorway, the depth image's layers marched). The constants are the shaders' cbuffers laid out as C++.
namespace Pictures::Gpu
{
	// cbuffer Capture. Clip space corners: the picture's bottom, bottom across, top.
	struct CaptureConstants
	{
		float corner[3][4];
		// xy: the scene's viewport in pixels; zw: a viewport pixel's size in the texture (the main
		// scene's viewport is the top-left part of its texture at a render scale below 1; the portal
		// pass's image always fills its target).
		float sizes[4];
		float out[4];  // xy: the picture's size
		float inverseViewProj[16];
		float wColumn[4];  // a camera-relative point's distance along the view axis: dot(p, xyz) + w
		float origin[4];   // the doorway's bottom corner, camera-relative
		float inward[4];   // the doorway's normal, away from the eye
		// x, y: the portal pass's near and far; z: 1 = Depth is the portal pass's; w: the capture
		// camera's distance to the doorway (looking along its normal), 0 = the player's view
		float portal[4];
	};

	// cbuffer Draw.
	struct DrawConstants
	{
		float                viewProj[16];  // as drawn (jittered)
		float                viewProjUnjittered[16];
		float                previousViewProj[16];  // unjittered, relative to the previous eye
		float                eyeDelta[4];           // this frame's eye - the previous frame's
		Hlsl::FogConstants   fog;                   // as the engine's FogParam
		float                plain[4];              // rgb: the colour without a picture; w: 1 = the picture
		float                params[4];             // x: alpha; yz: a texel's size
		float                origin[4];             // xyz: the doorway's bottom corner, camera-relative; w: 1 = the picture has depth
		float                across[4];             // xyz: its bottom edge; w: 1 / its length squared
		float                up[4];                 // xyz: its side going up; w: 1 / its length squared
		float                inward[4];             // xyz: its normal, away from the eye; w: how far behind it the picture was taken from (negative)
		float                takenFrom[4];          // xyz: where, camera-relative; w: how far behind the doorway the plain colour stands
		float                look[4];               // x: Adaptation::Kind; y: daylight; z: 1 = the outdoors behind; w: the picture's light now against when taken
		float                extent[4];             // the part of the door's plane the picture covers
		float                gate[4];               // x: 1 = a city gate (the same sky on both sides)
		float                layerInfo[4];          // x: the depth image's layers (0 = the image tier alone); yz: a layer's size in texels
		float                axisAcross[4];         // xyz: the doorway's bottom edge, unit; w: its width in units
		float                axisUp[4];             // xyz: its side going up, unit; w: its height in units
		float                layerPos[3][4];        // xyz: each view's place in the doorway's units
	};

	static_assert(sizeof(CaptureConstants) == 52 * sizeof(float));
	static_assert(sizeof(DrawConstants) == 128 * sizeof(float));

	// Made on first use or by Prewarm; false when it could not be made, for good.
	bool EnsurePipeline(ID3D11Device* a_device);
	[[nodiscard]] bool Failed();

	// The targets pictures are taken into, at the size they are taken at now (made anew when it
	// changes): half-float RGBA with its mips (colour; the distance in alpha) and the distance again as
	// R16_FLOAT. Sizes are whole compression blocks.
	bool                           EnsureTarget(ID3D11Device* a_device, UINT a_width, UINT a_height);
	[[nodiscard]] ID3D11Texture2D* Target();

	// Makes the picture just captured (Capture, into the targets of EnsureTarget's size) into a_colour
	// (BC6H, its mip chain, slice a_slice of its array) and a_distance (R16_FLOAT, slice a_slice): the
	// colour's mips are generated here and each is compressed on the GPU (a compute shader) and copied
	// in. Nothing is waited for. The compute state is put back as it was.
	bool Compress(ID3D11DeviceContext* a_context, ID3D11Texture2D* a_colour, ID3D11Texture2D* a_distance, UINT a_slice);

	// Draws the capture into the target: a_scene and a_depth read, the target a_width x a_height.
	void Capture(ID3D11DeviceContext* a_context, const CaptureConstants& a_constants, ID3D11ShaderResourceView* a_scene, ID3D11ShaderResourceView* a_depth, UINT a_width, UINT a_height);

	// Draws the doorway's strip (corners bottom, bottom across, top, top across, camera-relative)
	// into a_scene (and a_motion when it is opaque) against a_depthView.
	struct DrawTargets
	{
		ID3D11RenderTargetView* scene;
		ID3D11RenderTargetView* motion;
		ID3D11DepthStencilView* depth;
		float                   width;
		float                   height;
	};
	// The picture's textures (each may be none): its colour and distances, and the depth image's
	// array textures of the same.
	struct DrawTextures
	{
		ID3D11ShaderResourceView* picture = nullptr;
		ID3D11ShaderResourceView* pictureDistance = nullptr;
		ID3D11ShaderResourceView* layers = nullptr;
		ID3D11ShaderResourceView* layerDistance = nullptr;
	};
	void Draw(ID3D11DeviceContext* a_context, const DrawConstants& a_constants, const float (&a_vertices)[4][5], const DrawTargets& a_targets, const DrawTextures& a_textures, bool a_opaque);
}
