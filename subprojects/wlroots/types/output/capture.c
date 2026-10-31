#include <stdlib.h>
#include <wlr/render/allocator.h>
#include <wlr/render/color.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/addon.h>
#include <wlr/util/log.h>
#include "types/wlr_output.h"

bool output_get_sdr_gamut_primaries(const struct wlr_output *output,
		struct wlr_color_primaries *out) {
	const struct wlr_output_image_description *img_desc = output->image_description;
	float wideness = wlr_color_get_sdr_gamut_wideness();
	if (img_desc == NULL || output->default_primaries == NULL || wideness <= 0 ||
			img_desc->transfer_function != WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ) {
		return false;
	}

	struct wlr_color_primaries srgb;
	wlr_color_primaries_from_named(&srgb, WLR_COLOR_NAMED_PRIMARIES_SRGB);
	const struct wlr_color_primaries *native = output->default_primaries;
#define LERP_XY(field) \
	out->field.x = srgb.field.x + (native->field.x - srgb.field.x) * wideness; \
	out->field.y = srgb.field.y + (native->field.y - srgb.field.y) * wideness;
	LERP_XY(red)
	LERP_XY(green)
	LERP_XY(blue)
#undef LERP_XY
	out->white = srgb.white;
	return true;
}

struct capture_state {
	struct wlr_addon addon;
	struct wlr_swapchain *swapchain;
};

static void capture_state_destroy(struct wlr_addon *addon) {
	struct capture_state *state = wl_container_of(addon, state, addon);
	wlr_addon_finish(&state->addon);
	wlr_swapchain_destroy(state->swapchain);
	free(state);
}

static const struct wlr_addon_interface capture_state_impl = {
	.name = "wlr_output_capture_state",
	.destroy = capture_state_destroy,
};

static struct wlr_swapchain *get_capture_swapchain(struct wlr_output *output, int width, int height) {
	const struct wlr_drm_format *format = &output->swapchain->format;

	struct capture_state *state = NULL;
	struct wlr_addon *addon = wlr_addon_find(&output->addons, output, &capture_state_impl);
	if (addon != NULL) {
		state = wl_container_of(addon, state, addon);
		struct wlr_swapchain *swapchain = state->swapchain;
		if (swapchain != NULL && swapchain->width == width && swapchain->height == height &&
				swapchain->format.format == format->format &&
				swapchain->format.len == format->len) {
			return swapchain;
		}
	} else {
		state = calloc(1, sizeof(*state));
		if (state == NULL) {
			return NULL;
		}
		wlr_addon_init(&state->addon, &output->addons, output, &capture_state_impl);
	}

	wlr_swapchain_destroy(state->swapchain);
	state->swapchain = wlr_swapchain_create(output->allocator, width, height, format);
	return state->swapchain;
}

/*
 * Capture clients expect SDR pixels, but the buffer of an HDR output holds PQ
 * BT.2020. Convert it the opposite way wlr_scene does when it composites SDR
 * content, so that SDR white comes back as full white. PQ content brighter
 * than the SDR reference clips at white. Content wlr_scene gave the SDR gamut
 * boost gets it undone, except where the output says there is none.
 */
static struct wlr_buffer *render_sdr_buffer(struct wlr_output *output,
		struct wlr_buffer *src) {
	const struct wlr_output_image_description *img_desc = output->image_description;

	if (output->allocator == NULL || output->swapchain == NULL) {
		wlr_log(WLR_DEBUG, "No allocator or swapchain for HDR capture conversion");
		return NULL;
	}

	struct wlr_color_primaries primaries;
	wlr_color_primaries_from_named(&primaries, img_desc->primaries);

	struct wlr_color_luminances src_lum, dst_lum;
	wlr_color_transfer_function_get_default_luminance(img_desc->transfer_function, &src_lum);
	wlr_color_transfer_function_get_default_luminance(WLR_COLOR_TRANSFER_FUNCTION_GAMMA22, &dst_lum);
	float luminance_multiplier =
		(dst_lum.reference / src_lum.reference) * (src_lum.max / dst_lum.max);

	struct wlr_texture *texture = wlr_texture_from_buffer(output->renderer, src);
	if (texture == NULL) {
		wlr_log(WLR_DEBUG, "Failed to grab a texture from a buffer during HDR capture");
		return NULL;
	}

	// The output's own format, so that the buffer constraints advertised to
	// capture clients stay valid.
	struct wlr_swapchain *swapchain = get_capture_swapchain(output, src->width, src->height);
	struct wlr_buffer *dst = swapchain != NULL ? wlr_swapchain_acquire(swapchain) : NULL;
	if (dst == NULL) {
		wlr_log(WLR_DEBUG, "Failed to get a buffer for HDR capture");
		wlr_texture_destroy(texture);
		return NULL;
	}

	bool ok = false;
	struct wlr_color_primaries sdr_primaries;
	bool boosted = output_get_sdr_gamut_primaries(output, &sdr_primaries);
	// The default pass encoding is gamma 2.2, the inverse of how wlr_scene
	// decodes SDR content.
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(output->renderer, dst, NULL);
	if (pass != NULL) {
		struct wlr_render_texture_options options = {
			.texture = texture,
			.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
			.filter_mode = WLR_SCALE_FILTER_NEAREST,
			.transfer_function = img_desc->transfer_function,
			.primaries = &primaries,
			.dst_primaries = boosted ? &sdr_primaries : NULL,
			.luminance_multiplier = &luminance_multiplier,
		};
		wlr_render_pass_add_texture(pass, &options);
		if (boosted && !pixman_region32_empty(&output->unboosted_region)) {
			options.dst_primaries = NULL;
			options.clip = &output->unboosted_region;
			wlr_render_pass_add_texture(pass, &options);
		}
		ok = wlr_render_pass_submit(pass);
	}

	wlr_texture_destroy(texture);

	if (!ok) {
		wlr_log(WLR_DEBUG, "Failed to render the HDR capture conversion");
		wlr_buffer_unlock(dst);
		return NULL;
	}
	return dst;
}

struct wlr_buffer *output_capture_lock_sdr_buffer(struct wlr_output *output,
		struct wlr_buffer *src) {
	const struct wlr_output_image_description *img_desc = output->image_description;
	if (img_desc == NULL ||
			img_desc->transfer_function != WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ) {
		return wlr_buffer_lock(src);
	}
	return render_sdr_buffer(output, src);
}
