# WebOfShadowsRecomp for the PS5 - Edge of Time's renderer, for the native renderer
# (wos_native_renderer; included by wos/wos_game.cmake).
#
# Builds, from wos/eot/src (EdgeOfTimeRecomp's src/gpu and the few headers it needs, at
# af29aca, adapted for Web of Shadows: see "WoS port" in the sources) and from an
# EdgeOfTimeRecomp checkout beside this repository (../EdgeOfTimeRecomp, or
# EDGEOFTIMERECOMP_SRC) for its third-party code:
#
#   - wos_plume: plume, the renderer's Vulkan layer (thirdparty/plume with its
#     ps5/patches/plume-ps5.patch applied), on the title's volk;
#   - wos_eot: the renderer, its hooks renamed for Web of Shadows (wos_eot_names.h);
#   - wos_eot_zstd: zstd, which unpacks the shader cache (XenosRecomp's);
#   - the renderer's own shaders (src/gpu/shaders/hlsl), compiled to SPIR-V by dxc;
#   - wos_eot_shader_cache: the game's shaders, recompiled by Edge of Time's XenosRecomp
#     from the harvest (wos/app/wos_ps5_shader_harvest.cpp). Game-derived: it lives
#     outside the repository, at ../wos_shader_cache/wos_shader_cache.cpp (or
#     WOS_SHADER_CACHE); without it the renderer draws nothing of the game's own.
#
# Copyright (C) 2026 KongaTime
# SPDX-License-Identifier: MIT

set(wos_eotr "$ENV{EDGEOFTIMERECOMP_SRC}")
if(NOT wos_eotr)
	set(wos_eotr ${ROOT}/../EdgeOfTimeRecomp)
endif()
get_filename_component(wos_eotr "${wos_eotr}" ABSOLUTE)
set(wos_plume ${wos_eotr}/thirdparty/plume)
set(wos_xenosrecomp ${wos_eotr}/thirdparty/XenosRecomp)
if(NOT EXISTS ${wos_plume}/contrib/volk/volk.h)
	message(FATAL_ERROR "No plume with its submodules at ${wos_plume}: git -C ${wos_eotr} submodule update "
		"--init thirdparty/plume, then git -C ${wos_plume} submodule update --init contrib/volk "
		"contrib/Vulkan-Headers contrib/VulkanMemoryAllocator")
endif()
file(READ ${wos_plume}/plume_vulkan.cpp wos_plume_source)
if(NOT wos_plume_source MATCHES "createProsperoDisplaySurface")
	message(FATAL_ERROR "plume at ${wos_plume} has no PS5 support: git -C ${wos_plume} apply "
		"${wos_eotr}/ps5/patches/plume-ps5.patch")
endif()
if(NOT EXISTS ${wos_xenosrecomp}/thirdparty/zstd/lib/zstd.h OR NOT EXISTS ${wos_xenosrecomp}/thirdparty/smol-v/source/smolv.cpp)
	message(FATAL_ERROR "No XenosRecomp with its submodules at ${wos_xenosrecomp}: git -C ${wos_eotr} submodule "
		"update --init --recursive thirdparty/XenosRecomp")
endif()
find_program(WOS_DXC NAMES dxc dxc-linux HINTS ${wos_xenosrecomp}/thirdparty/dxc-bin/bin/x64 REQUIRED)
message(STATUS "WebOfShadowsRecomp: native renderer from ${CMAKE_CURRENT_LIST_DIR}/src, plume from ${wos_plume}, dxc ${WOS_DXC}")

set(wos_eot_gen ${CMAKE_BINARY_DIR}/wos_eot_gen)
file(MAKE_DIRECTORY ${wos_eot_gen}/shaders)
set(wos_eot_src ${CMAKE_CURRENT_LIST_DIR}/src)

# --- plume --------------------------------------------------------------------------
# WoS port (fix34): plume is built from a copy with three guards (the fix29-fix32 crashes
# were in VulkanFramebuffer's constructor and VulkanCommandList::barriers - fault.log RIPs
# 0x36de752 / 0x36df7c2 against the fix33 llvm-pie.elf): a texture or buffer barrier on a
# null object is skipped, and a framebuffer on one mip of a texture takes that mip's size
# (it took mip 0's, larger than its attachment - invalid in Vulkan).
set(wos_plume_patched "${wos_plume_source}")
set(wos_plume_patches 0)
macro(wos_plume_patch old new)
	string(FIND "${wos_plume_patched}" "${old}" wos_at)
	if(wos_at GREATER_EQUAL 0)
		string(REPLACE "${old}" "${new}" wos_plume_patched "${wos_plume_patched}")
		math(EXPR wos_plume_patches "${wos_plume_patches} + 1")
	else()
		message(WARNING "WebOfShadowsRecomp: plume guard not applied (text not found): ${old}")
	endif()
endmacro()
wos_plume_patch("VulkanTexture *interfaceTexture = static_cast<VulkanTexture *>(textureBarrier.texture);"
	"VulkanTexture *interfaceTexture = static_cast<VulkanTexture *>(textureBarrier.texture);\n            if (interfaceTexture == nullptr || interfaceTexture->vk == VK_NULL_HANDLE) continue; // WoS port")
wos_plume_patch("VulkanBuffer *interfaceBuffer = static_cast<VulkanBuffer *>(bufferBarrier.buffer);\n            VkBufferMemoryBarrier"
	"VulkanBuffer *interfaceBuffer = static_cast<VulkanBuffer *>(bufferBarrier.buffer);\n            if (interfaceBuffer == nullptr || interfaceBuffer->vk == VK_NULL_HANDLE) continue; // WoS port\n            VkBufferMemoryBarrier")
wos_plume_patch("            RenderFormat colorAttachmentFormat;\n"
	"            RenderFormat colorAttachmentFormat;\n            uint32_t colorMip = 0; // WoS port\n")
wos_plume_patch("colorAttachmentFormat = colorAttachmentView->desc.format;"
	"colorAttachmentFormat = colorAttachmentView->desc.format;\n                colorMip = colorAttachmentView->desc.mipSlice; // WoS port")
wos_plume_patch("                width = uint32_t(colorAttachment->desc.width);\n                height = colorAttachment->desc.height;"
	"                width = std::max<uint32_t>(1u, uint32_t(colorAttachment->desc.width) >> colorMip); // WoS port\n                height = std::max<uint32_t>(1u, uint32_t(colorAttachment->desc.height) >> colorMip);")
wos_plume_patch("                width = uint32_t(depthAttachment->desc.width);\n                height = depthAttachment->desc.height;"
	"                width = std::max<uint32_t>(1u, uint32_t(depthAttachment->desc.width) >> depthAttachmentViewDesc.mipSlice); // WoS port\n                height = std::max<uint32_t>(1u, uint32_t(depthAttachment->desc.height) >> depthAttachmentViewDesc.mipSlice);")
# WoS port (fix58): an integer vertex fetch feeding a float shader input is converted by the
# GPU (7 -> 7.0): EoT flags such elements with semanticIndex bit 8 -> *_USCALED / *_SSCALED
wos_plume_patch("attribute.format = toVk(inputElement.format);"
	"attribute.format = toVk(inputElement.format);\n            if (inputElement.semanticIndex & 0x100u) { switch (attribute.format) { case VK_FORMAT_R8G8B8A8_UINT: attribute.format = VK_FORMAT_R8G8B8A8_USCALED; break; case VK_FORMAT_R8G8B8A8_SINT: attribute.format = VK_FORMAT_R8G8B8A8_SSCALED; break; case VK_FORMAT_R8G8_UINT: attribute.format = VK_FORMAT_R8G8_USCALED; break; case VK_FORMAT_R8G8_SINT: attribute.format = VK_FORMAT_R8G8_SSCALED; break; case VK_FORMAT_R8_UINT: attribute.format = VK_FORMAT_R8_USCALED; break; case VK_FORMAT_R8_SINT: attribute.format = VK_FORMAT_R8_SSCALED; break; case VK_FORMAT_R16G16B16A16_UINT: attribute.format = VK_FORMAT_R16G16B16A16_USCALED; break; case VK_FORMAT_R16G16B16A16_SINT: attribute.format = VK_FORMAT_R16G16B16A16_SSCALED; break; case VK_FORMAT_R16G16_UINT: attribute.format = VK_FORMAT_R16G16_USCALED; break; case VK_FORMAT_R16G16_SINT: attribute.format = VK_FORMAT_R16G16_SSCALED; break; case VK_FORMAT_R16_UINT: attribute.format = VK_FORMAT_R16_USCALED; break; case VK_FORMAT_R16_SINT: attribute.format = VK_FORMAT_R16_SSCALED; break; default: break; } } // WoS port")
message(STATUS "WebOfShadowsRecomp: plume guards applied: ${wos_plume_patches} of 7")
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/wos_eot_gen)
set(wos_plume_copy ${CMAKE_BINARY_DIR}/wos_eot_gen/plume_vulkan.cpp)
set(wos_plume_old "")
if(EXISTS ${wos_plume_copy})
	file(READ ${wos_plume_copy} wos_plume_old)
endif()
if(NOT wos_plume_old STREQUAL wos_plume_patched)
	file(WRITE ${wos_plume_copy} "${wos_plume_patched}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${wos_plume}/plume_vulkan.cpp)
add_library(wos_plume OBJECT ${wos_plume_copy})
target_include_directories(wos_plume PRIVATE ${wos_plume} ${wos_plume}/contrib/volk
	${wos_plume}/contrib/Vulkan-Headers/include ${wos_plume}/contrib/VulkanMemoryAllocator/include)
target_compile_definitions(wos_plume PRIVATE PLUME_NO_VOLK_IMPLEMENTATION NDEBUG)
target_compile_options(wos_plume PRIVATE -std=c++20 -O2 -fno-omit-frame-pointer)

# --- the renderer's own shaders, as SPIR-V (EdgeOfTimeRecomp's cmake/host_shaders.cmake) ---
set(wos_eot_hlsl ${wos_eot_src}/gpu/shaders/hlsl)
file(GLOB wos_eot_hlsli ${wos_eot_hlsl}/*.hlsli)
set(wos_eot_shader_headers)
get_filename_component(wos_dxc_dir ${WOS_DXC} DIRECTORY)
foreach(shader copy_vs:vs_6_0 imgui_vs:vs_6_0 blit_ps:ps_6_0 copy_depth_ps:ps_6_0 copy_depth_f24_ps:ps_6_0 imgui_ps:ps_6_0 taa_ps:ps_6_0
		velocity_resolve_2x_ps:ps_6_0 velocity_resolve_4x_ps:ps_6_0 velocity_resolve_8x_ps:ps_6_0
		resolve_msaa_color_2x_ps:ps_6_0 resolve_msaa_color_4x_ps:ps_6_0 resolve_msaa_color_8x_ps:ps_6_0
		resolve_msaa_depth_2x_ps:ps_6_0 resolve_msaa_depth_4x_ps:ps_6_0 resolve_msaa_depth_8x_ps:ps_6_0
		derive_depth_stencil_2x_ps:ps_6_0 derive_depth_stencil_4x_ps:ps_6_0 derive_depth_stencil_8x_ps:ps_6_0)
	string(REPLACE ":" ";" parts ${shader})
	list(GET parts 0 stem)
	list(GET parts 1 profile)
	set(arguments -spirv -fvk-use-dx-layout)
	if(profile MATCHES "^vs")
		list(APPEND arguments -fvk-invert-y)
	endif()
	set(header ${wos_eot_gen}/shaders/${stem}.hlsl.spirv.h)
	add_custom_command(OUTPUT ${header}
		COMMAND ${CMAKE_COMMAND} -E env LD_LIBRARY_PATH=${wos_dxc_dir}/../../lib/x64:$ENV{LD_LIBRARY_PATH}
			${WOS_DXC} -T ${profile} -HV 2021 -all-resources-bound -Wno-ignored-attributes ${arguments}
			-I ${wos_eot_hlsl} -Fh ${header} ${wos_eot_hlsl}/${stem}.hlsl -Vn g_${stem}_spirv
		DEPENDS ${wos_eot_hlsl}/${stem}.hlsl ${wos_eot_hlsli}
		COMMENT "WebOfShadowsRecomp: renderer shader ${stem}.hlsl (${profile}, SPIR-V)" VERBATIM)
	list(APPEND wos_eot_shader_headers ${header})
endforeach()

# --- the game's shaders -------------------------------------------------------------
set(WOS_SHADER_CACHE "$ENV{WOS_SHADER_CACHE}")
if(NOT WOS_SHADER_CACHE)
	set(WOS_SHADER_CACHE ${ROOT}/../wos_shader_cache/wos_shader_cache.cpp)
endif()
if(EXISTS ${WOS_SHADER_CACHE})
	message(STATUS "WebOfShadowsRecomp: the game's shaders from ${WOS_SHADER_CACHE}")
	set(wos_eot_shader_cache ${WOS_SHADER_CACHE})
else()
	message(WARNING "WebOfShadowsRecomp: no shader cache at ${WOS_SHADER_CACHE}, so the native renderer "
		"draws none of the game's own shaders")
	set(wos_eot_shader_cache ${wos_eot_src}/gpu/shaders/shader_cache_empty.cpp)
endif()
add_library(wos_eot_shader_cache OBJECT ${wos_eot_shader_cache})
target_include_directories(wos_eot_shader_cache PRIVATE ${wos_eot_src}/gpu/shaders)
target_compile_options(wos_eot_shader_cache PRIVATE -O0 -w)

# --- zstd -----------------------------------------------------------------------------
set(wos_zstd ${wos_xenosrecomp}/thirdparty/zstd/lib)
file(GLOB wos_zstd_sources ${wos_zstd}/common/*.c ${wos_zstd}/compress/*.c ${wos_zstd}/decompress/*.c)
add_library(wos_eot_zstd OBJECT ${wos_zstd_sources})
target_include_directories(wos_eot_zstd PRIVATE ${wos_zstd} ${wos_zstd}/common)
# ZSTD_TRACE=0: its trace hooks are weak references, which a title may not import
target_compile_definitions(wos_eot_zstd PRIVATE ZSTD_DISABLE_ASM ZSTD_TRACE=0 XXH_NAMESPACE=ZSTD_)
target_compile_options(wos_eot_zstd PRIVATE -O2 -fno-omit-frame-pointer -ffunction-sections -fdata-sections)

# --- the renderer -----------------------------------------------------------------------
set(wos_eot_sources
	gpu/settings.cpp gpu/device.cpp gpu/frame_ring.cpp gpu/present.cpp gpu/imgui_overlay.cpp
	gpu/format.cpp gpu/textures.cpp gpu/surfaces.cpp gpu/vertex_layout.cpp gpu/draw.cpp
	gpu/render_thread.cpp gpu/resolve.cpp gpu/trace.cpp gpu/pipeline/pipeline_cache.cpp
	gpu/gpu_timing.cpp gpu/memory_report.cpp gpu/pipeline/pso_records.cpp gpu/pipeline/pso_precache.cpp
	gpu/pipeline/pso_predictor.cpp gpu/taa.cpp gpu/shaders/guest_shaders.cpp
	gpu/hooks/state.cpp gpu/hooks/resource.cpp platform/display.cpp)
list(TRANSFORM wos_eot_sources PREPEND ${wos_eot_src}/)
add_library(wos_eot OBJECT ${wos_eot_sources} ${wos_eot_shader_headers}
	${wos_xenosrecomp}/thirdparty/smol-v/source/smolv.cpp)
# The hooks take Web of Shadows' names (wos_eot_names.h)
set_source_files_properties(${wos_eot_src}/gpu/hooks/state.cpp ${wos_eot_src}/gpu/hooks/resource.cpp
	PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/wos_eot_names.h")
set(wos_eot_includes ${wos_eot_src} ${wos_eot_gen} ${wos_xenosrecomp}/XenosRecomp
	${wos_xenosrecomp}/thirdparty/smol-v/source ${wos_zstd} ${wos_eotr}/thirdparty/renderdoc
	${wos_plume} ${wos_plume}/contrib/volk ${wos_plume}/contrib/Vulkan-Headers/include
	${wos_plume}/contrib/VulkanMemoryAllocator/include)
target_include_directories(wos_eot PRIVATE ${wos_eot_includes} ${wos_rex_includes})
target_compile_definitions(wos_eot PRIVATE ${wos_rex_definitions} _ITERATOR_DEBUG_LEVEL=0 NOMINMAX
	WIN32_LEAN_AND_MEAN PLUME_NO_VOLK_IMPLEMENTATION)
target_compile_options(wos_eot PRIVATE ${wos_rex_options} -O2)

# The title's app reaches the renderer through its headers (wos_ps5_native_renderer.cpp)
set_source_files_properties(${ROOT}/wos/app/wos_ps5_native_renderer.cpp
	${ROOT}/wos/app/wos_ps5_capture.cpp PROPERTIES
	INCLUDE_DIRECTORIES "${wos_eot_includes}"
	COMPILE_DEFINITIONS "_ITERATOR_DEBUG_LEVEL=0;NOMINMAX;PLUME_NO_VOLK_IMPLEMENTATION")

# plume's VMA under names of its own: the runtime's Vulkan GPU links its own VMA (prefix_vma.py)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(wos_plume_object ${CMAKE_BINARY_DIR}/wos_plume_vma.o)
add_custom_command(OUTPUT ${wos_plume_object}
	COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/prefix_vma.py --bin ${PS5_PAYLOAD_SDK}/bin
		--prefix wosplume_ $<TARGET_OBJECTS:wos_plume> ${wos_plume_object}
	DEPENDS wos_plume $<TARGET_OBJECTS:wos_plume> ${CMAKE_CURRENT_LIST_DIR}/prefix_vma.py
	COMMAND_EXPAND_LISTS
	COMMENT "WebOfShadowsRecomp: plume's VMA under its own names" VERBATIM)
add_custom_target(wos_plume_vma DEPENDS ${wos_plume_object})

list(APPEND wos_eot_link_inputs ${wos_plume_object} $<TARGET_OBJECTS:wos_eot>
	$<TARGET_OBJECTS:wos_eot_zstd> $<TARGET_OBJECTS:wos_eot_shader_cache>)
set(wos_eot_link_depends wos_plume_vma wos_eot wos_eot_zstd wos_eot_shader_cache ${wos_eot_link_inputs})
