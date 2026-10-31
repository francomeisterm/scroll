#include <wlr/render/allocator.h>
#include <wlr/render/color.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
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

/*
 * Undo the SDR gamut boost: the texture pass ends up in sRGB primaries, this
 * maps them back to the SDR values wlr_scene started from. NULL means the
 * default gamma 2.2 encoding is enough.
 */
static struct wlr_color_transform *create_sdr_gamut_transform(
		const struct wlr_output *output) {
	struct wlr_color_primaries sdr_primaries, srgb;
	if (!output_get_sdr_gamut_primaries(output, &sdr_primaries)) {
		return NULL;
	}
	wlr_color_primaries_from_named(&srgb, WLR_COLOR_NAMED_PRIMARIES_SRGB);

	float matrix[9];
	wlr_color_primaries_transform_absolute_colorimetric(&srgb, &sdr_primaries, matrix);

	struct wlr_color_transform *transforms[2] = {
		wlr_color_transform_init_matrix(matrix),
		wlr_color_transform_init_linear_to_inverse_eotf(WLR_COLOR_TRANSFER_FUNCTION_GAMMA22),
	};
	struct wlr_color_transform *pipeline = NULL;
	if (transforms[0] != NULL && transforms[1] != NULL) {
		pipeline = wlr_color_transform_init_pipeline(transforms, 2);
	}
	wlr_color_transform_unref(transforms[0]);
	wlr_color_transform_unref(transforms[1]);
	return pipeline;
}

/*
 * Capture clients expect SDR pixels, but the buffer of an HDR output holds PQ
 * BT.2020. Convert it the opposite way wlr_scene does when it composites SDR
 * content, so that SDR white comes back as full white. PQ content brighter
 * than the SDR reference clips at white. The gamut boost is a per-texture SDR
 * step that can't be told apart here, so PQ surfaces get undone by it too.
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
	struct wlr_buffer *dst = wlr_allocator_create_buffer(output->allocator,
		src->width, src->height, &output->swapchain->format);
	if (dst == NULL) {
		wlr_log(WLR_DEBUG, "Failed to allocate a buffer for HDR capture");
		wlr_texture_destroy(texture);
		return NULL;
	}

	bool ok = false;
	struct wlr_color_transform *gamut_transform = create_sdr_gamut_transform(output);
	// The pass encoding is gamma 2.2, the inverse of how wlr_scene decodes SDR
	// content.
	struct wlr_render_pass *pass =
		wlr_renderer_begin_buffer_pass(output->renderer, dst,
			&(struct wlr_buffer_pass_options) {
				.color_transform = gamut_transform,
			});
	if (pass != NULL) {
		wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options) {
			.texture = texture,
			.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
			.filter_mode = WLR_SCALE_FILTER_NEAREST,
			.transfer_function = img_desc->transfer_function,
			.primaries = &primaries,
			.luminance_multiplier = &luminance_multiplier,
		});
		ok = wlr_render_pass_submit(pass);
	}

	wlr_color_transform_unref(gamut_transform);
	wlr_texture_destroy(texture);

	if (!ok) {
		wlr_log(WLR_DEBUG, "Failed to render the HDR capture conversion");
		wlr_buffer_drop(dst);
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

	struct wlr_buffer *dst = render_sdr_buffer(output, src);
	if (dst == NULL) {
		return NULL;
	}

	// The lock is now the only reference, unlocking destroys the buffer
	wlr_buffer_lock(dst);
	wlr_buffer_drop(dst);
	return dst;
}
