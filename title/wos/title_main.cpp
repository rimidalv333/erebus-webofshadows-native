/*
 * WebOfShadowsRecomp for the PS5 - the title's program.
 *
 * wos/title_entry.cpp calls this once the platform layer is up (klog, the pad)
 * and volk points at the RADV linked in. It runs the ReXGlue app the link holds
 * (REX_DEFINE_APP: the game, wos/app/wos_ps5_app.cpp, or the test app,
 * wos/rex_test_app.cpp) through the fork's PS5 entry, rex_ps5_app_main.
 *
 * The title gives the runtime its startup log (prospero_startup_log, which the
 * fork defines weakly): each marker is a line of WOS_TEST_RESULTS
 * (/app0/wos-test-results.txt, the title's folder, readable over FTP) and of
 * klog, written before the step it names, so a run that stops says where.
 *
 * Before the game starts, it checks that the player's game files are where the
 * runtime looks for them (/app0/gamedata/default.xex) and, when they are not,
 * says so on the console's screen, in the results file and in klog.
 *
 * Copyright (C) 2026 KongaTime (reeot/title_main.cpp, which this follows)
 * SPDX-License-Identifier: MIT
 */

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include "platform.h"

extern "C" int rex_ps5_app_main(int argc, char **argv);

// libkernel's on-screen notification (the toast in the top corner)
struct wos_notify_request {
	char reserved[45];
	char message[3075];
};
extern "C" int sceKernelSendNotificationRequest(int device, wos_notify_request *request, size_t size, int blocking);

extern "C" void wos_note(const char *format, ...) __attribute__((format(printf, 1, 2)));
extern "C" void wos_note(const char *format, ...)
{
	char line[512];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof line, format, args);
	va_end(args);
	say("%s", line);
	if (FILE *file = fopen(WOS_TEST_RESULTS, "a")) {
		fprintf(file, "%s\n", line);
		fclose(file);
	}
}

extern "C" void prospero_startup_log(const char *message)
{
	wos_note("startup: %s", message ? message : "(null)");
}

static void notify(const char *message)
{
	wos_notify_request request{};
	snprintf(request.message, sizeof request.message, "%s", message);
	sceKernelSendNotificationRequest(0, &request, sizeof request, 0);
}

// The game's files: default.xex at the top of /app0/gamedata, in any case
static bool game_files_present(void)
{
	DIR *dir = opendir("/app0/gamedata");
	if (!dir)
		return false;
	bool found = false;
	while (dirent *entry = readdir(dir)) {
		if (strcasecmp(entry->d_name, "default.xex") == 0) {
			found = true;
			break;
		}
	}
	closedir(dir);
	return found;
}

extern "C" int ps5_title_main(void)
{
	// A fresh results file each run, readable and writable over FTP
	if (FILE *file = fopen(WOS_TEST_RESULTS, "w")) {
		fclose(file);
		chmod(WOS_TEST_RESULTS, 0666);
	}
	wos_note("WebOfShadowsRecomp for the PS5: the %s app", WOS_TITLE_APP_NAME);

	if (strcmp(WOS_TITLE_APP_NAME, "game") == 0 && !game_files_present()) {
		wos_note("game files: MISSING - no default.xex in /app0/gamedata "
			 "(/data/homebrew/<title id>/gamedata on the console)");
		notify("Web of Shadows: game files missing.\n"
		       "Copy your extracted Xbox 360 game (with default.xex) into this title's gamedata folder.");
		std::this_thread::sleep_for(std::chrono::seconds(6));
		return 1;
	}

	// While the game is brought up: a line here every five seconds, so a run
	// that goes quiet says whether the title was still alive (a hang) or not
	std::thread([] {
		for (int seconds = 5;; seconds += 5) {
			std::this_thread::sleep_for(std::chrono::seconds(5));
			wos_note("alive: %d s", seconds);
		}
	}).detach();
	char program[] = "/app0/eboot.bin";
	// Info level: what a report needs without filling the console's storage
	char log_level[] = "--log_level=info";
	char *argv[] = {program, log_level, nullptr};
	const int status = rex_ps5_app_main(2, argv);
	wos_note("rex_ps5_app_main returned %d", status);
	return status;
}
