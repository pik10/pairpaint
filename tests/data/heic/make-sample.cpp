// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors
//
// Writes sample.heic, the HEIC test image: like an iPhone photo it is HEVC-compressed, has
// Display P3 colors, and is stored sideways with a "rotate 90° clockwise to display" flag.
// Stored as 64 x 48 pixels with four flat quadrants (Display P3 values):
//   top left (200, 60, 40)   top right (60, 180, 60)
//   bottom left (40, 60, 200) bottom right (128, 128, 128)
// Displayed (rotated) it is 48 x 64: bottom left becomes top left, top left becomes top right.
//
//   c++ make-sample.cpp $(pkg-config --cflags --libs libheif) -o make-sample && ./make-sample
// (needs a libheif with an HEVC encoder, such as x265)

#include <libheif/heif.h>

#include <cstdio>

int main()
{
    heif_init(nullptr);
    heif_context *ctx = heif_context_alloc();
    heif_encoder *encoder = nullptr;
    if (heif_context_get_encoder_for_format(ctx, heif_compression_HEVC, &encoder).code) {
        std::fprintf(stderr, "no HEVC encoder\n");
        return 1;
    }
    heif_encoder_set_lossy_quality(encoder, 92);

    const int w = 64, h = 48;
    heif_image *img = nullptr;
    heif_image_create(w, h, heif_colorspace_RGB, heif_chroma_interleaved_RGB, &img);
    heif_image_add_plane(img, heif_channel_interleaved, w, h, 8);
    size_t stride = 0;
    uint8_t *p = heif_image_get_plane2(img, heif_channel_interleaved, &stride);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            static const uint8_t colors[4][3] = {{200, 60, 40}, {60, 180, 60}, {40, 60, 200}, {128, 128, 128}};
            const uint8_t *c = colors[(y >= h / 2) * 2 + (x >= w / 2)];
            for (int k = 0; k < 3; ++k)
                p[y * stride + x * 3 + k] = c[k];
        }
    }
    heif_color_profile_nclx *nclx = heif_nclx_color_profile_alloc();
    nclx->color_primaries = heif_color_primaries_SMPTE_EG_432_1;  // Display P3
    nclx->transfer_characteristics = heif_transfer_characteristic_IEC_61966_2_1;  // sRGB curve
    nclx->matrix_coefficients = heif_matrix_coefficients_ITU_R_BT_601_6;
    nclx->full_range_flag = 1;
    heif_image_set_nclx_color_profile(img, nclx);

    heif_encoding_options *options = heif_encoding_options_alloc();
    options->image_orientation = heif_orientation_rotate_90_cw;
    options->output_nclx_profile = nclx;
    heif_error err = heif_context_encode_image(ctx, img, encoder, options, nullptr);
    if (!err.code)
        err = heif_context_write_to_file(ctx, "sample.heic");
    if (err.code)
        std::fprintf(stderr, "%s\n", err.message);
    return err.code ? 1 : 0;
}
