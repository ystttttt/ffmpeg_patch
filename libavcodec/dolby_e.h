/*
 * Copyright (C) 2017 foo86
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVCODEC_DOLBY_E_H
#define AVCODEC_DOLBY_E_H

#include <stdint.h>
#include "get_bits.h"

#define FRAME_SAMPLES   1792

#define MAX_PROG_CONF   23
#define MAX_PROGRAMS    8
#define MAX_CHANNELS    8

/**
 * @struct DBEContext
 * Dolby E reading context used by decoder and parser.
 */
typedef struct DBEContext {
    void        *avctx;
    GetBitContext   gb;

    const uint8_t *input;
    int         input_size;

    int         word_bits;
    int         word_bytes;
    int         key_present;

    uint8_t     buffer[1024 * 3 + AV_INPUT_BUFFER_PADDING_SIZE];
} DBEContext;

/**
 * @struct DolbyEHeaderInfo
 * Coded Dolby E header values up to end_gain element, plus derived values.
 */
typedef struct DolbyEHeaderInfo {
    /** @name Coded elements
     * @{
     */
    int         prog_conf;
    int         nb_channels;
    int         nb_programs;

    int         fr_code;
    int         fr_code_orig;

    int         ch_size[MAX_CHANNELS];
    int         mtd_ext_size;
    int         meter_size;

    int         rev_id[MAX_CHANNELS];
    int         begin_gain[MAX_CHANNELS];
    int         end_gain[MAX_CHANNELS];
    /** @} */

    /** @name Derived values
     * @{
     */
    int         multi_prog_warned;
    /** @} */
} DolbyEHeaderInfo;

static const uint16_t sample_rate_tab[16] = {
    0, 42965, 43008, 44800, 53706, 53760
};
/**
 * Initialize DBEContext.
 * Set word_bits/word_bytes, input, input_size, key_present.
 * @param[out] s DBEContext.
 * @param[in]  buf raw input buffer.
 * @param[in]  buf_size must be 3 bytes at least.
 * @return Returns 0 on success, AVERROR_INVALIDDATA on error
 */
int ff_dolby_e_parse_init(DBEContext *s, const uint8_t *buf, int buf_size);

/**
 * Parse Dolby E metadata.
 * Parse the header up to the end_gain element.
 * @param[in]  s DBEContext .
 * @param[out] hdr Pointer to struct where header info is written.
 * @return Returns 0 on success, AVERROR_INVALIDDATA on error
 */
int ff_dolby_e_parse_header(DBEContext *s, DolbyEHeaderInfo *hdr);

#endif
