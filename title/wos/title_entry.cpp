/*
 * WebOfShadowsRecomp for the PS5 - the title's main when it runs the game.
 *
 * ps5/src/main.cpp starts the console's side as the template does, then runs
 * ps5_title_main (PS5_TITLE_MAIN); but it also keeps the template's sample menu
 * and test runs, which link the Vulkan samples and their ImGui 1.65 beside the
 * runtime's ImGui 1.92. This main does the same start-up and runs the game alone,
 * so wos/wos.cmake builds the title without the samples.
 *
 * Copyright (C) 2026 KongaTime
 * SPDX-License-Identifier: MIT
 */

#include <iostream>

#include "platform.h"

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);
extern "C" int ps5_title_main(void);

int main()
{
	platform_init(PS5_TITLE_NAME);
	std::cout.rdbuf(std::cerr.rdbuf());
	pad_open();
	// Vulkan's commands come from the RADV linked into the title, through volk
	volkInitializeCustom(vk_icdGetInstanceProcAddr);
	const int status = ps5_title_main();
	say("ends: status %d", status);
	return status;
}
