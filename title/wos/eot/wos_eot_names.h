// WebOfShadowsRecomp for the PS5 - Edge of Time's renderer hooks, named for Web of Shadows.
//
// Edge of Time's hooks (src/gpu/hooks/*.cpp) are written against its codegen's names:
// REX_FUNC(D3DDevice_SetTexture) is its override, __imp__D3DDevice_SetTexture the
// recompiled original. Web of Shadows' code is generated with plain sub_XXXXXXXX names.
// This header (force-included into the hook sources by wos/eot/wos_eot.cmake) renames:
//
//   - each hook to wos_eot_<name>, an ordinary function: wos/app/wos_ps5_native_renderer.cpp
//     owns the sub_XXXXXXXX overrides and calls these only when the native renderer is on,
//     so the emulated GPU path is untouched when it is off;
//   - each original to Web of Shadows' recompiled function (wos/NATIVE-RENDERER-NOTES.md, P1).
//
// SPDX-License-Identifier: MIT

#pragma once


// The hooks
#define D3DDevice_DrawVertices wos_eot_DrawVertices
#define D3DDevice_DrawIndexedVertices wos_eot_DrawIndexedVertices
#define D3DDevice_BeginVertices wos_eot_BeginVertices
#define D3DDevice_ClearF wos_eot_ClearF
#define D3DDevice_Resolve wos_eot_Resolve
#define D3DDevice_SetViewport wos_eot_SetViewport
#define D3DDevice_SetGammaRamp wos_eot_SetGammaRamp
#define D3DDevice_SetPWLGamma wos_eot_SetPWLGamma
#define D3DDevice_Swap wos_eot_Swap
#define D3DDevice_CreatePixelShader wos_eot_CreatePixelShader
#define D3DDevice_CreateVertexShader wos_eot_CreateVertexShader
#define XGRegisterVertexShader wos_eot_XGRegisterVertexShader
#define D3D_UnlockResource wos_eot_UnlockResource
#define D3DDevice_SetTexture wos_eot_SetTexture
#define D3DDevice_SetPixelShader wos_eot_SetPixelShader
#define D3DDevice_SetVertexShader wos_eot_SetVertexShader
#define D3DDevice_SetRenderTarget wos_eot_SetRenderTarget
#define D3DDevice_SetDepthStencilSurface wos_eot_SetDepthStencilSurface
#define D3DDevice_SetIndices wos_eot_SetIndices
#define D3DDevice_SetStreamSource wos_eot_SetStreamSource

// The originals: Web of Shadows' recompiled D3D
#define __imp__D3DDevice_DrawVertices __imp__sub_82ABECF0
#define __imp__D3DDevice_DrawIndexedVertices __imp__sub_82ABF0E0
#define __imp__D3DDevice_BeginVertices __imp__sub_82ABE800
#define __imp__D3DDevice_ClearF __imp__sub_82ABE618
#define __imp__D3DDevice_Resolve __imp__sub_82ABFCB0
#define __imp__D3DDevice_SetViewport __imp__sub_82AB8B98
#define __imp__D3DDevice_SetGammaRamp __imp__sub_82AB8658
#define __imp__D3DDevice_SetPWLGamma __imp__sub_82AB8720
#define __imp__D3DDevice_CreatePixelShader __imp__sub_82AB48A8
#define __imp__D3DDevice_CreateVertexShader __imp__sub_82AB49B8
#define __imp__XGRegisterVertexShader __imp__sub_82AB4368
#define __imp__D3D_UnlockResource __imp__sub_82AB5048
#define __imp__D3DDevice_SetTexture __imp__sub_82ABD328
#define __imp__D3DDevice_SetPixelShader __imp__sub_82AB41A8
#define __imp__D3DDevice_SetVertexShader __imp__sub_82AB4460
#define __imp__D3DDevice_SetRenderTarget __imp__sub_82AB8CC8
#define __imp__D3DDevice_SetDepthStencilSurface __imp__sub_82AB9028
#define __imp__D3DDevice_SetIndices __imp__sub_82AB8540
#define __imp__D3DDevice_SetStreamSource __imp__sub_82AB8420
