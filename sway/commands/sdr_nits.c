#include <stdlib.h>
#include <wlr/render/color.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include "sway/commands.h"
#include "sway/output.h"
#include "sway/tree/root.h"
#include "list.h"

// sdr_nits <80-1000>: SDR white level on HDR (PQ) outputs, applied live.
struct cmd_results *cmd_sdr_nits(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "sdr_nits", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	char *end;
	float nits = strtof(argv[0], &end);
	if (*end != '\0' || nits < 80 || nits > 1000) {
		return cmd_results_new(CMD_INVALID, "Expected a value from 80 to 1000 nits");
	}
	wlr_color_set_pq_reference_luminance(nits);
	for (int i = 0; i < root->outputs->length; i++) {
		struct sway_output *output = root->outputs->items[i];
		wlr_scene_output_damage_whole(output->scene_output);
		wlr_output_update_needs_frame(output->wlr_output);
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}
