# WebOfShadowsRecomp for the PS5 - the title's program (included by ps5/CMakeLists.txt).
#
# The title's entry (wos/title_entry.cpp, with wos/title_main.cpp) runs the ReXGlue
# app the link holds through the runtime's PS5 entry, rex_ps5_app_main, in place
# of the template's Vulkan samples (PS5_TITLE_MAIN, ps5/src/main.cpp):
#
#   WOS_TITLE_APP=game   the game: WebOfShadowsRecomp's recompiled code and the
#                        title's PS5 app (wos/wos_game.cmake), drawing through the
#                        runtime's emulated Xenos GPU on Vulkan (RADV)
#   WOS_TITLE_APP=test   the test app (wos/rex_test_app.cpp): the window, the
#                        emulated GPU's Vulkan device and presenter on the display,
#                        the guest memory and the sound, with no game code at all
#
# The runtime is the ReXGlue fork with the PS5 target (../rexglue-sdk, or
# REXGLUE_SRC), cross-built by wos/tools/build-rexglue.sh into the fork's
# out/ps5-amd64/ (its build folder ../rexglue-ps5-build, or REXGLUE_PS5_BUILD):
# librexruntime.a and its libraries, and librexgpu-xenos.a, the emulated GPU,
# which a title links in (it loads no shared library).
#
# Unlike PS5_EdgeOfTimeTitle, nothing here draws with a renderer of its own:
# Edge of Time replaces the game's Direct3D with plume, Web of Shadows keeps the
# game's GPU commands and has the runtime emulate the Xenos GPU, as The Simpsons
# Game's PS5 port does.
#
# Copyright (C) 2026 KongaTime (reeot/reeot.cmake, which this follows)
# SPDX-License-Identifier: MIT

# Where the runtime and WebOfShadowsRecomp are: the environment's choice when the
# build folder is first configured (REXGLUE_SRC, REXGLUE_PS5_BUILD,
# WEBOFSHADOWSRECOMP_SRC), else the checkouts beside this repository; kept in the
# cache, so a later reconfigure without those variables finds the same ones
function(wos_path variable environment default description)
	if(NOT DEFINED CACHE{${variable}})
		set(value "$ENV{${environment}}")
		if(NOT value)
			set(value "${default}")
		endif()
		get_filename_component(value "${value}" ABSOLUTE)
		set(${variable} "${value}" CACHE PATH "${description}")
	endif()
endfunction()
wos_path(WOS_REXGLUE_SRC REXGLUE_SRC ${ROOT}/../rexglue-sdk "the ReXGlue fork with the PS5 target")
wos_path(WOS_REXGLUE_PS5_BUILD REXGLUE_PS5_BUILD ${ROOT}/../rexglue-ps5-build
	"its PS5 build folder (wos/tools/build-rexglue.sh)")
wos_path(WOS_WEBOFSHADOWS_SRC WEBOFSHADOWSRECOMP_SRC ${ROOT}/../WebOfShadowsRecomp
	"the WebOfShadowsRecomp checkout with the generated code")
set(wos_rexglue ${WOS_REXGLUE_SRC})
set(wos_runtime_build ${WOS_REXGLUE_PS5_BUILD})
set(wos_runtime_dir ${wos_rexglue}/out/ps5-amd64)
set(wos_src ${WOS_WEBOFSHADOWS_SRC})
file(WRITE ${CMAKE_BINARY_DIR}/release-dependency-paths.json
	"{\n  \"rexglue\": \"${wos_rexglue}\",\n  \"webofshadows\": \"${wos_src}\"\n}\n")

foreach(archive librexruntime.a librexgpu-xenos.a)
	if(NOT EXISTS ${wos_runtime_dir}/${archive})
		message(FATAL_ERROR "No ${archive} built for the PS5 at ${wos_runtime_dir}: run wos/tools/build-rexglue.sh "
			"(it builds the runtime and the emulated GPU), or set WOS_REXGLUE_SRC")
	endif()
endforeach()
if(NOT EXISTS ${wos_runtime_build}/include/rex/version.h)
	message(FATAL_ERROR "No ReXGlue PS5 build folder at ${wos_runtime_build} (its generated headers): run "
		"wos/tools/build-rexglue.sh, or set WOS_REXGLUE_PS5_BUILD")
endif()
message(STATUS "WebOfShadowsRecomp: the ReXGlue runtime and emulated GPU from ${wos_runtime_dir}")

set(WOS_TITLE_APP game CACHE STRING
	"What the title runs: game (WebOfShadowsRecomp) or test (wos/rex_test_app.cpp)")
set_property(CACHE WOS_TITLE_APP PROPERTY STRINGS game test)
message(STATUS "WebOfShadowsRecomp: the title runs the ${WOS_TITLE_APP} app")

# The title alone: the template's start-up (its platform layer and volk) and
# wos/title_entry.cpp, without the Vulkan samples, whose ImGui 1.65 would meet
# the runtime's ImGui 1.92 in the link
get_target_property(wos_samples_sources samples SOURCES)
list(FILTER wos_samples_sources INCLUDE REGEX "(^|/)(src/platform\\.c|third_party/volk/volk\\.c)$")
set_property(TARGET samples PROPERTY SOURCES ${wos_samples_sources} ${ROOT}/wos/title_entry.cpp
	${ROOT}/wos/title_main.cpp)
target_compile_definitions(samples PRIVATE PS5_TITLE_MAIN "WOS_TEST_RESULTS=\"/app0/wos-test-results.txt\""
	"WOS_TITLE_APP_NAME=\"${WOS_TITLE_APP}\"")

# The runtime's headers, defines and language settings, which every part of the
# program that talks to it builds with
set(wos_rex_includes ${wos_rexglue}/include ${wos_runtime_build}/include
	${wos_rexglue}/thirdparty/fmt/include ${wos_rexglue}/thirdparty/spdlog/include
	${wos_rexglue}/thirdparty/simde ${wos_rexglue}/thirdparty/simde/simde
	${wos_rexglue}/thirdparty/utfcpp/source ${wos_rexglue}/thirdparty/imgui
	${wos_rexglue}/thirdparty/vulkan-headers/include ${wos_rexglue}/thirdparty/tomlplusplus/include
	${wos_rexglue}/thirdparty/xxHash ${wos_rexglue}/thirdparty/renderdoc)
set(wos_rex_definitions SPDLOG_COMPILED_LIB SPDLOG_FMT_EXTERNAL REX_HAS_VULKAN=1
	REXGLUE_BUILD_CONFIG="Release" NDEBUG "WOS_TEST_RESULTS=\"/app0/wos-test-results.txt\"")
set(wos_rex_options -std=c++23 -fexperimental-library -fno-char8_t -fno-strict-aliasing -march=znver2
	-fno-omit-frame-pointer -ffunction-sections -fdata-sections)

set(wos_program_link_inputs)
set(wos_program_link_depends)
set(wos_program_link_flags)
if(WOS_TITLE_APP STREQUAL "test")
	add_library(wos_test OBJECT ${ROOT}/wos/rex_test_app.cpp)
	target_include_directories(wos_test PRIVATE ${wos_rex_includes})
	target_compile_definitions(wos_test PRIVATE ${wos_rex_definitions})
	target_compile_options(wos_test PRIVATE ${wos_rex_options} -O2)
	set(wos_program_link_inputs $<TARGET_OBJECTS:wos_test>)
	set(wos_program_link_depends wos_test $<TARGET_OBJECTS:wos_test>)
else()
	include(${ROOT}/wos/wos_game.cmake)
endif()

# The runtime and its libraries in one group (they refer to each other); the
# emulated GPU among them
file(GLOB wos_runtime_archives CONFIGURE_DEPENDS ${wos_runtime_dir}/*.a)
list(REMOVE_ITEM wos_runtime_archives ${wos_runtime_dir}/librexruntime.a)

# libc names the runtime imports that no module a title loads exports (null at
# run time: link-title.sh refuses them), or that the console refuses a title,
# bound to the platform layer's, as PS5_EdgeOfTimeTitle and PS5_RPCS3Title bind
# them: FFmpeg's isatty, libc++'s pathconf (std::filesystem), realpath (EPERM to
# a title) and fork (a title starts no process)
set(wos_libc_bindings)
foreach(name isatty pathconf realpath fork)
	list(APPEND wos_libc_bindings --defsym=${name}=ps5_${name})
endforeach()
# A bound name the SDK's stubs also define would be exported from the title to
# override theirs, and the native tool refuses a title's exports: every name
# bound here stays local, as PS5_Vulkan's recipe keeps its own
set(wos_local_map ${CMAKE_BINARY_DIR}/wos-local.map)
set(wos_local_names)
foreach(flag ${wos_libc_bindings})
	string(REGEX REPLACE "^--defsym=([^=]+)=.*$" "\\1" name "${flag}")
	string(APPEND wos_local_names "        ${name};\n")
endforeach()
# ...and the program's own wrappers of a name a module exports (wos_game.cmake)
foreach(name ${wos_program_local_names})
	string(APPEND wos_local_names "        ${name};\n")
endforeach()
file(WRITE ${wos_local_map} "{\n    local:\n${wos_local_names}};\n")

set(PS5_TITLE_LINK_INPUTS
	${wos_program_link_inputs}
	--start-group ${wos_rexglue}/out/ps5-amd64/librexruntime.a ${wos_runtime_archives}
	${PS5_PAYLOAD_SDK}/target/lib/libc++experimental.a --end-group
	${wos_libc_bindings} --version-script ${wos_local_map}
	# abort() through the runtime's fault records (its __wrap_abort)
	--wrap=abort
	${wos_program_link_flags})
set(PS5_TITLE_LINK_DEPENDS ${wos_runtime_dir}/librexruntime.a ${wos_runtime_archives} ${wos_program_link_depends})
