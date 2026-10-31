#include <stdlib.h>
#include <strings.h>
#include <wlr/types/wlr_scene.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/tree/container.h"
#include "sway/tree/root.h"
#include "sway/tree/view.h"
#include "util.h"

// blur_passes <0-6>: blur size for everything that's blurred, 0 disables it.
struct cmd_results *cmd_blur_passes(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "blur_passes", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	char *end;
	long passes = strtol(argv[0], &end, 10);
	if (*end != '\0' || passes < 0 || passes > 6) {
		return cmd_results_new(CMD_INVALID, "Expected a value from 0 to 6");
	}
	wlr_scene_set_blur_passes(root->root_scene, passes);
	return cmd_results_new(CMD_SUCCESS, NULL);
}

// blur_style <size> <contrast> <brightness> <vibrancy> <noise>: named like the
// Hyprland options, the defaults match its blur.
struct cmd_results *cmd_blur_style(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "blur_style", EXPECTED_EQUAL_TO, 5))) {
		return error;
	}
	float values[5];
	for (int i = 0; i < 5; i++) {
		char *end;
		values[i] = strtof(argv[i], &end);
		if (*end != '\0' || values[i] < 0) {
			return cmd_results_new(CMD_INVALID, "Expected numbers from 0 up");
		}
	}
	wlr_scene_set_blur_style(root->root_scene, values[0], values[1], values[2],
		values[3], values[4]);
	return cmd_results_new(CMD_SUCCESS, NULL);
}

// window_blur on|off: blur behind the translucent parts of a window.
struct cmd_results *cmd_window_blur(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "window_blur", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	struct sway_container *con = config->handler_context.container;
	if (con == NULL || con->view == NULL) {
		return cmd_results_new(CMD_INVALID, "Only views can have blur");
	}
	con->view->blur = parse_boolean(argv[0], con->view->blur);
	return cmd_results_new(CMD_SUCCESS, NULL);
}
