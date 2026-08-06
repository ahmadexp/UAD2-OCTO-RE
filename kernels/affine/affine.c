/* SPDX-License-Identifier: MIT */
/* Minimal ADSP-21469 affine kernel used for the first custom-code proof. */

#define UAD_AFFINE_MAX_FRAMES 64u
#define UAD_AFFINE_OK 0
#define UAD_AFFINE_BAD_POINTER (-1)
#define UAD_AFFINE_BAD_COUNT (-2)

/*
 * This is a processor-level entry point, not yet a claim about the UAD
 * framework call ABI.  The eventual UAD adapter must translate its recovered
 * Process/module object into these five arguments and preserve the bounds.
 */
#pragma retain_name
int uad_affine_entry(const float *input, float *output, unsigned int frames,
                     float scale, float bias)
{
    unsigned int index;

    if (input == 0 || output == 0)
        return UAD_AFFINE_BAD_POINTER;
    if (frames > UAD_AFFINE_MAX_FRAMES)
        return UAD_AFFINE_BAD_COUNT;

    for (index = 0; index < frames; index++)
        output[index] = input[index] * scale + bias;

    return UAD_AFFINE_OK;
}
