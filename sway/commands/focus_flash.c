#include <stdlib.h>
#include <string.h>
#include "sway/commands.h"
#include "sway/config.h"

// focus_flash off|<alpha> <ms>: dip the opacity of a tiled window that gets
// focus from a focus command.
struct cmd_results *cmd_focus_flash(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "focus_flash", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	if (strcmp(argv[0], "off") == 0) {
		config->focus_flash_duration = 0;
		return cmd_results_new(CMD_SUCCESS, NULL);
	}
	if ((error = checkarg(argc, "focus_flash", EXPECTED_EQUAL_TO, 2))) {
		return error;
	}
	char *end;
	float alpha = strtof(argv[0], &end);
	if (*end != '\0' || alpha < 0 || alpha >= 1) {
		return cmd_results_new(CMD_INVALID, "Expected an opacity from 0 to below 1");
	}
	long duration = strtol(argv[1], &end, 10);
	if (*end != '\0' || duration < 1 || duration > 2000) {
		return cmd_results_new(CMD_INVALID, "Expected a duration from 1 to 2000 ms");
	}
	config->focus_flash_alpha = alpha;
	config->focus_flash_duration = duration;
	return cmd_results_new(CMD_SUCCESS, NULL);
}
