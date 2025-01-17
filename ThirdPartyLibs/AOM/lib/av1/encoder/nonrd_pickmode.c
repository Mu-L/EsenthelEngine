/*
 * Copyright (c) 2016, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.

 */

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>

#include "config/aom_dsp_rtcd.h"
#include "config/av1_rtcd.h"

#include "aom_dsp/aom_dsp_common.h"
#include "aom_dsp/txfm_common.h"
#include "aom_ports/mem.h"

#include "av1/common/blockd.h"
#include "av1/common/mvref_common.h"
#include "av1/common/pred_common.h"
#include "av1/common/reconinter.h"
#include "av1/common/reconintra.h"

#include "av1/encoder/encodemv.h"
#include "av1/encoder/encoder.h"
#include "av1/encoder/intra_mode_search.h"
#include "av1/encoder/model_rd.h"
#include "av1/encoder/motion_search_facade.h"
#include "av1/encoder/nonrd_opt.h"
#include "av1/encoder/rdopt.h"
#include "av1/encoder/reconinter_enc.h"
#include "av1/encoder/var_based_part.h"

#define CALC_BIASED_RDCOST(rdcost) (7 * (rdcost) >> 3)
extern int g_pick_inter_mode_cnt;
/*!\cond */
typedef struct {
  uint8_t *data;
  int stride;
  int in_use;
} PRED_BUFFER;

typedef struct {
  PRED_BUFFER *best_pred;
  PREDICTION_MODE best_mode;
  TX_SIZE best_tx_size;
  TX_TYPE tx_type;
  MV_REFERENCE_FRAME best_ref_frame;
  MV_REFERENCE_FRAME best_second_ref_frame;
  uint8_t best_mode_skip_txfm;
  uint8_t best_mode_initial_skip_flag;
  int_interpfilters best_pred_filter;
  MOTION_MODE best_motion_mode;
  WarpedMotionParams wm_params;
  int num_proj_ref;
  uint8_t blk_skip[MAX_MIB_SIZE * MAX_MIB_SIZE / 4];
  PALETTE_MODE_INFO pmi;
  int64_t best_sse;
} BEST_PICKMODE;

typedef struct {
  MV_REFERENCE_FRAME ref_frame;
  PREDICTION_MODE pred_mode;
} REF_MODE;

typedef struct {
  MV_REFERENCE_FRAME ref_frame[2];
  PREDICTION_MODE pred_mode;
} COMP_REF_MODE;
/*!\endcond */

/*!\brief Structure to store parameters and statistics used in non-rd inter mode
 * evaluation.
 */
typedef struct {
  //! Structure to hold best inter mode data
  BEST_PICKMODE best_pickmode;
  //! Structure to RD cost of current mode
  RD_STATS this_rdc;
  //! Pointer to the RD Cost for the best mode found so far
  RD_STATS best_rdc;
  //! Distortion of chroma planes for all modes and reference frames
  int64_t uv_dist[RTC_INTER_MODES][REF_FRAMES];
  //! Buffer to hold predicted block for all reference frames and planes
  struct buf_2d yv12_mb[REF_FRAMES][MAX_MB_PLANE];
  //! Array to hold variance of all modes and reference frames
  unsigned int vars[RTC_INTER_MODES][REF_FRAMES];
  //! Array to hold ref cost of single reference mode for all ref frames
  unsigned int ref_costs_single[REF_FRAMES];
  //! Array to hold motion vector for all modes and reference frames
  int_mv frame_mv[MB_MODE_COUNT][REF_FRAMES];
  //! Array to hold best mv for all modes and reference frames
  int_mv frame_mv_best[MB_MODE_COUNT][REF_FRAMES];
  //! Array to hold inter mode cost of single ref mode for all ref frames
  int single_inter_mode_costs[RTC_INTER_MODES][REF_FRAMES];
  //! Array to hold use reference frame mask for each reference frame
  int use_ref_frame_mask[REF_FRAMES];
  //! Array to hold flags of evaluated modes for each reference frame
  uint8_t mode_checked[MB_MODE_COUNT][REF_FRAMES];
} InterModeSearchStateNonrd;

#define NUM_COMP_INTER_MODES_RT (6)
#define NUM_INTER_MODES 12

// GLOBALMV in the set below is in fact ZEROMV as we don't do global ME in RT
// mode
static const REF_MODE ref_mode_set[NUM_INTER_MODES] = {
  { LAST_FRAME, NEARESTMV },   { LAST_FRAME, NEARMV },
  { LAST_FRAME, GLOBALMV },    { LAST_FRAME, NEWMV },
  { GOLDEN_FRAME, NEARESTMV }, { GOLDEN_FRAME, NEARMV },
  { GOLDEN_FRAME, GLOBALMV },  { GOLDEN_FRAME, NEWMV },
  { ALTREF_FRAME, NEARESTMV }, { ALTREF_FRAME, NEARMV },
  { ALTREF_FRAME, GLOBALMV },  { ALTREF_FRAME, NEWMV },
};

static const COMP_REF_MODE comp_ref_mode_set[NUM_COMP_INTER_MODES_RT] = {
  { { LAST_FRAME, GOLDEN_FRAME }, GLOBAL_GLOBALMV },
  { { LAST_FRAME, GOLDEN_FRAME }, NEAREST_NEARESTMV },
  { { LAST_FRAME, LAST2_FRAME }, GLOBAL_GLOBALMV },
  { { LAST_FRAME, LAST2_FRAME }, NEAREST_NEARESTMV },
  { { LAST_FRAME, ALTREF_FRAME }, GLOBAL_GLOBALMV },
  { { LAST_FRAME, ALTREF_FRAME }, NEAREST_NEARESTMV },
};

static const int_interpfilters filters_ref_set[9] = {
  [0].as_filters = { EIGHTTAP_REGULAR, EIGHTTAP_REGULAR },
  [1].as_filters = { EIGHTTAP_SMOOTH, EIGHTTAP_SMOOTH },
  [2].as_filters = { EIGHTTAP_REGULAR, EIGHTTAP_SMOOTH },
  [3].as_filters = { EIGHTTAP_SMOOTH, EIGHTTAP_REGULAR },
  [4].as_filters = { MULTITAP_SHARP, MULTITAP_SHARP },
  [5].as_filters = { EIGHTTAP_REGULAR, MULTITAP_SHARP },
  [6].as_filters = { MULTITAP_SHARP, EIGHTTAP_REGULAR },
  [7].as_filters = { EIGHTTAP_SMOOTH, MULTITAP_SHARP },
  [8].as_filters = { MULTITAP_SHARP, EIGHTTAP_SMOOTH }
};

enum {
  //  INTER_ALL = (1 << NEARESTMV) | (1 << NEARMV) | (1 << NEWMV),
  INTER_NEAREST = (1 << NEARESTMV),
  INTER_NEAREST_NEW = (1 << NEARESTMV) | (1 << NEWMV),
  INTER_NEAREST_NEAR = (1 << NEARESTMV) | (1 << NEARMV),
  INTER_NEAR_NEW = (1 << NEARMV) | (1 << NEWMV),
};

// The original scan order (default_scan_8x8) is modified according to the extra
// transpose in hadamard c implementation, i.e., aom_hadamard_lp_8x8_c and
// aom_hadamard_8x8_c.
static const int16_t default_scan_8x8_transpose[64] = {
  0,  8,  1,  2,  9,  16, 24, 17, 10, 3,  4,  11, 18, 25, 32, 40,
  33, 26, 19, 12, 5,  6,  13, 20, 27, 34, 41, 48, 56, 49, 42, 35,
  28, 21, 14, 7,  15, 22, 29, 36, 43, 50, 57, 58, 51, 44, 37, 30,
  23, 31, 38, 45, 52, 59, 60, 53, 46, 39, 47, 54, 61, 62, 55, 63
};

// The original scan order (av1_default_iscan_8x8) is modified to match
// hadamard AVX2 implementation, i.e., aom_hadamard_lp_8x8_avx2 and
// aom_hadamard_8x8_avx2. Since hadamard AVX2 implementation will modify the
// order of coefficients, such that the normal scan order is no longer
// guaranteed to scan low coefficients first, therefore we modify the scan order
// accordingly.
// Note that this one has to be used together with default_scan_8x8_transpose.
static const int16_t av1_default_iscan_8x8_transpose[64] = {
  0,  2,  3,  9,  10, 20, 21, 35, 1,  4,  8,  11, 19, 22, 34, 36,
  5,  7,  12, 18, 23, 33, 37, 48, 6,  13, 17, 24, 32, 38, 47, 49,
  14, 16, 25, 31, 39, 46, 50, 57, 15, 26, 30, 40, 45, 51, 56, 58,
  27, 29, 41, 44, 52, 55, 59, 62, 28, 42, 43, 53, 54, 60, 61, 63
};

// The original scan order (default_scan_16x16) is modified according to the
// extra transpose in hadamard c implementation in lp case, i.e.,
// aom_hadamard_lp_16x16_c.
static const int16_t default_scan_lp_16x16_transpose[256] = {
  0,   8,   2,   4,   10,  16,  24,  18,  12,  6,   64,  14,  20,  26,  32,
  40,  34,  28,  22,  72,  66,  68,  74,  80,  30,  36,  42,  48,  56,  50,
  44,  38,  88,  82,  76,  70,  128, 78,  84,  90,  96,  46,  52,  58,  1,
  9,   3,   60,  54,  104, 98,  92,  86,  136, 130, 132, 138, 144, 94,  100,
  106, 112, 62,  5,   11,  17,  25,  19,  13,  7,   120, 114, 108, 102, 152,
  146, 140, 134, 192, 142, 148, 154, 160, 110, 116, 122, 65,  15,  21,  27,
  33,  41,  35,  29,  23,  73,  67,  124, 118, 168, 162, 156, 150, 200, 194,
  196, 202, 208, 158, 164, 170, 176, 126, 69,  75,  81,  31,  37,  43,  49,
  57,  51,  45,  39,  89,  83,  77,  71,  184, 178, 172, 166, 216, 210, 204,
  198, 206, 212, 218, 224, 174, 180, 186, 129, 79,  85,  91,  97,  47,  53,
  59,  61,  55,  105, 99,  93,  87,  137, 131, 188, 182, 232, 226, 220, 214,
  222, 228, 234, 240, 190, 133, 139, 145, 95,  101, 107, 113, 63,  121, 115,
  109, 103, 153, 147, 141, 135, 248, 242, 236, 230, 238, 244, 250, 193, 143,
  149, 155, 161, 111, 117, 123, 125, 119, 169, 163, 157, 151, 201, 195, 252,
  246, 254, 197, 203, 209, 159, 165, 171, 177, 127, 185, 179, 173, 167, 217,
  211, 205, 199, 207, 213, 219, 225, 175, 181, 187, 189, 183, 233, 227, 221,
  215, 223, 229, 235, 241, 191, 249, 243, 237, 231, 239, 245, 251, 253, 247,
  255
};

#if CONFIG_AV1_HIGHBITDEPTH
// The original scan order (default_scan_16x16) is modified according to the
// extra shift in hadamard c implementation in fp case, i.e.,
// aom_hadamard_16x16_c. Note that 16x16 lp and fp hadamard generate different
// outputs, so we handle them separately.
static const int16_t default_scan_fp_16x16_transpose[256] = {
  0,   4,   2,   8,   6,   16,  20,  18,  12,  10,  64,  14,  24,  22,  32,
  36,  34,  28,  26,  68,  66,  72,  70,  80,  30,  40,  38,  48,  52,  50,
  44,  42,  84,  82,  76,  74,  128, 78,  88,  86,  96,  46,  56,  54,  1,
  5,   3,   60,  58,  100, 98,  92,  90,  132, 130, 136, 134, 144, 94,  104,
  102, 112, 62,  9,   7,   17,  21,  19,  13,  11,  116, 114, 108, 106, 148,
  146, 140, 138, 192, 142, 152, 150, 160, 110, 120, 118, 65,  15,  25,  23,
  33,  37,  35,  29,  27,  69,  67,  124, 122, 164, 162, 156, 154, 196, 194,
  200, 198, 208, 158, 168, 166, 176, 126, 73,  71,  81,  31,  41,  39,  49,
  53,  51,  45,  43,  85,  83,  77,  75,  180, 178, 172, 170, 212, 210, 204,
  202, 206, 216, 214, 224, 174, 184, 182, 129, 79,  89,  87,  97,  47,  57,
  55,  61,  59,  101, 99,  93,  91,  133, 131, 188, 186, 228, 226, 220, 218,
  222, 232, 230, 240, 190, 137, 135, 145, 95,  105, 103, 113, 63,  117, 115,
  109, 107, 149, 147, 141, 139, 244, 242, 236, 234, 238, 248, 246, 193, 143,
  153, 151, 161, 111, 121, 119, 125, 123, 165, 163, 157, 155, 197, 195, 252,
  250, 254, 201, 199, 209, 159, 169, 167, 177, 127, 181, 179, 173, 171, 213,
  211, 205, 203, 207, 217, 215, 225, 175, 185, 183, 189, 187, 229, 227, 221,
  219, 223, 233, 231, 241, 191, 245, 243, 237, 235, 239, 249, 247, 253, 251,
  255
};
#endif

// The original scan order (av1_default_iscan_16x16) is modified to match
// hadamard AVX2 implementation, i.e., aom_hadamard_lp_16x16_avx2.
// Since hadamard AVX2 implementation will modify the order of coefficients,
// such that the normal scan order is no longer guaranteed to scan low
// coefficients first, therefore we modify the scan order accordingly. Note that
// this one has to be used together with default_scan_lp_16x16_transpose.
static const int16_t av1_default_iscan_lp_16x16_transpose[256] = {
  0,   44,  2,   46,  3,   63,  9,   69,  1,   45,  4,   64,  8,   68,  11,
  87,  5,   65,  7,   67,  12,  88,  18,  94,  6,   66,  13,  89,  17,  93,
  24,  116, 14,  90,  16,  92,  25,  117, 31,  123, 15,  91,  26,  118, 30,
  122, 41,  148, 27,  119, 29,  121, 42,  149, 48,  152, 28,  120, 43,  150,
  47,  151, 62,  177, 10,  86,  20,  96,  21,  113, 35,  127, 19,  95,  22,
  114, 34,  126, 37,  144, 23,  115, 33,  125, 38,  145, 52,  156, 32,  124,
  39,  146, 51,  155, 58,  173, 40,  147, 50,  154, 59,  174, 73,  181, 49,
  153, 60,  175, 72,  180, 83,  198, 61,  176, 71,  179, 84,  199, 98,  202,
  70,  178, 85,  200, 97,  201, 112, 219, 36,  143, 54,  158, 55,  170, 77,
  185, 53,  157, 56,  171, 76,  184, 79,  194, 57,  172, 75,  183, 80,  195,
  102, 206, 74,  182, 81,  196, 101, 205, 108, 215, 82,  197, 100, 204, 109,
  216, 131, 223, 99,  203, 110, 217, 130, 222, 140, 232, 111, 218, 129, 221,
  141, 233, 160, 236, 128, 220, 142, 234, 159, 235, 169, 245, 78,  193, 104,
  208, 105, 212, 135, 227, 103, 207, 106, 213, 134, 226, 136, 228, 107, 214,
  133, 225, 137, 229, 164, 240, 132, 224, 138, 230, 163, 239, 165, 241, 139,
  231, 162, 238, 166, 242, 189, 249, 161, 237, 167, 243, 188, 248, 190, 250,
  168, 244, 187, 247, 191, 251, 210, 254, 186, 246, 192, 252, 209, 253, 211,
  255
};

#if CONFIG_AV1_HIGHBITDEPTH
// The original scan order (av1_default_iscan_16x16) is modified to match
// hadamard AVX2 implementation, i.e., aom_hadamard_16x16_avx2.
// Since hadamard AVX2 implementation will modify the order of coefficients,
// such that the normal scan order is no longer guaranteed to scan low
// coefficients first, therefore we modify the scan order accordingly. Note that
// this one has to be used together with default_scan_fp_16x16_transpose.
static const int16_t av1_default_iscan_fp_16x16_transpose[256] = {
  0,   44,  2,   46,  1,   45,  4,   64,  3,   63,  9,   69,  8,   68,  11,
  87,  5,   65,  7,   67,  6,   66,  13,  89,  12,  88,  18,  94,  17,  93,
  24,  116, 14,  90,  16,  92,  15,  91,  26,  118, 25,  117, 31,  123, 30,
  122, 41,  148, 27,  119, 29,  121, 28,  120, 43,  150, 42,  149, 48,  152,
  47,  151, 62,  177, 10,  86,  20,  96,  19,  95,  22,  114, 21,  113, 35,
  127, 34,  126, 37,  144, 23,  115, 33,  125, 32,  124, 39,  146, 38,  145,
  52,  156, 51,  155, 58,  173, 40,  147, 50,  154, 49,  153, 60,  175, 59,
  174, 73,  181, 72,  180, 83,  198, 61,  176, 71,  179, 70,  178, 85,  200,
  84,  199, 98,  202, 97,  201, 112, 219, 36,  143, 54,  158, 53,  157, 56,
  171, 55,  170, 77,  185, 76,  184, 79,  194, 57,  172, 75,  183, 74,  182,
  81,  196, 80,  195, 102, 206, 101, 205, 108, 215, 82,  197, 100, 204, 99,
  203, 110, 217, 109, 216, 131, 223, 130, 222, 140, 232, 111, 218, 129, 221,
  128, 220, 142, 234, 141, 233, 160, 236, 159, 235, 169, 245, 78,  193, 104,
  208, 103, 207, 106, 213, 105, 212, 135, 227, 134, 226, 136, 228, 107, 214,
  133, 225, 132, 224, 138, 230, 137, 229, 164, 240, 163, 239, 165, 241, 139,
  231, 162, 238, 161, 237, 167, 243, 166, 242, 189, 249, 188, 248, 190, 250,
  168, 244, 187, 247, 186, 246, 192, 252, 191, 251, 210, 254, 209, 253, 211,
  255
};
#endif

// For entropy coding, IDTX shares the scan orders of the other 2D-transforms,
// but the fastest way to calculate the IDTX transform (i.e. no transposes)
// results in coefficients that are a transposition of the entropy coding
// versions. These tables are used as substitute for the scan order for the
// faster version of IDTX.

// Must be used together with av1_fast_idtx_iscan_4x4
static const int16_t av1_fast_idtx_scan_4x4[16] = {
  0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15
};

// Must be used together with av1_fast_idtx_scan_4x4
static const int16_t av1_fast_idtx_iscan_4x4[16] = { 0, 1,  5,  6, 2,  4,
                                                     7, 12, 3,  8, 11, 13,
                                                     9, 10, 14, 15 };

static const SCAN_ORDER av1_fast_idtx_scan_order_4x4 = {
  av1_fast_idtx_scan_4x4, av1_fast_idtx_iscan_4x4
};

// Must be used together with av1_fast_idtx_iscan_8x8
static const int16_t av1_fast_idtx_scan_8x8[64] = {
  0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
  12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
  35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
  58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

// Must be used together with av1_fast_idtx_scan_8x8
static const int16_t av1_fast_idtx_iscan_8x8[64] = {
  0,  1,  5,  6,  14, 15, 27, 28, 2,  4,  7,  13, 16, 26, 29, 42,
  3,  8,  12, 17, 25, 30, 41, 43, 9,  11, 18, 24, 31, 40, 44, 53,
  10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60,
  21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63
};

static const SCAN_ORDER av1_fast_idtx_scan_order_8x8 = {
  av1_fast_idtx_scan_8x8, av1_fast_idtx_iscan_8x8
};

// Must be used together with av1_fast_idtx_iscan_16x16
static const int16_t av1_fast_idtx_scan_16x16[256] = {
  0,   1,   16,  32,  17,  2,   3,   18,  33,  48,  64,  49,  34,  19,  4,
  5,   20,  35,  50,  65,  80,  96,  81,  66,  51,  36,  21,  6,   7,   22,
  37,  52,  67,  82,  97,  112, 128, 113, 98,  83,  68,  53,  38,  23,  8,
  9,   24,  39,  54,  69,  84,  99,  114, 129, 144, 160, 145, 130, 115, 100,
  85,  70,  55,  40,  25,  10,  11,  26,  41,  56,  71,  86,  101, 116, 131,
  146, 161, 176, 192, 177, 162, 147, 132, 117, 102, 87,  72,  57,  42,  27,
  12,  13,  28,  43,  58,  73,  88,  103, 118, 133, 148, 163, 178, 193, 208,
  224, 209, 194, 179, 164, 149, 134, 119, 104, 89,  74,  59,  44,  29,  14,
  15,  30,  45,  60,  75,  90,  105, 120, 135, 150, 165, 180, 195, 210, 225,
  240, 241, 226, 211, 196, 181, 166, 151, 136, 121, 106, 91,  76,  61,  46,
  31,  47,  62,  77,  92,  107, 122, 137, 152, 167, 182, 197, 212, 227, 242,
  243, 228, 213, 198, 183, 168, 153, 138, 123, 108, 93,  78,  63,  79,  94,
  109, 124, 139, 154, 169, 184, 199, 214, 229, 244, 245, 230, 215, 200, 185,
  170, 155, 140, 125, 110, 95,  111, 126, 141, 156, 171, 186, 201, 216, 231,
  246, 247, 232, 217, 202, 187, 172, 157, 142, 127, 143, 158, 173, 188, 203,
  218, 233, 248, 249, 234, 219, 204, 189, 174, 159, 175, 190, 205, 220, 235,
  250, 251, 236, 221, 206, 191, 207, 222, 237, 252, 253, 238, 223, 239, 254,
  255
};

// Must be used together with av1_fast_idtx_scan_16x16
static const int16_t av1_fast_idtx_iscan_16x16[256] = {
  0,   1,   5,   6,   14,  15,  27,  28,  44,  45,  65,  66,  90,  91,  119,
  120, 2,   4,   7,   13,  16,  26,  29,  43,  46,  64,  67,  89,  92,  118,
  121, 150, 3,   8,   12,  17,  25,  30,  42,  47,  63,  68,  88,  93,  117,
  122, 149, 151, 9,   11,  18,  24,  31,  41,  48,  62,  69,  87,  94,  116,
  123, 148, 152, 177, 10,  19,  23,  32,  40,  49,  61,  70,  86,  95,  115,
  124, 147, 153, 176, 178, 20,  22,  33,  39,  50,  60,  71,  85,  96,  114,
  125, 146, 154, 175, 179, 200, 21,  34,  38,  51,  59,  72,  84,  97,  113,
  126, 145, 155, 174, 180, 199, 201, 35,  37,  52,  58,  73,  83,  98,  112,
  127, 144, 156, 173, 181, 198, 202, 219, 36,  53,  57,  74,  82,  99,  111,
  128, 143, 157, 172, 182, 197, 203, 218, 220, 54,  56,  75,  81,  100, 110,
  129, 142, 158, 171, 183, 196, 204, 217, 221, 234, 55,  76,  80,  101, 109,
  130, 141, 159, 170, 184, 195, 205, 216, 222, 233, 235, 77,  79,  102, 108,
  131, 140, 160, 169, 185, 194, 206, 215, 223, 232, 236, 245, 78,  103, 107,
  132, 139, 161, 168, 186, 193, 207, 214, 224, 231, 237, 244, 246, 104, 106,
  133, 138, 162, 167, 187, 192, 208, 213, 225, 230, 238, 243, 247, 252, 105,
  134, 137, 163, 166, 188, 191, 209, 212, 226, 229, 239, 242, 248, 251, 253,
  135, 136, 164, 165, 189, 190, 210, 211, 227, 228, 240, 241, 249, 250, 254,
  255
};

static const SCAN_ORDER av1_fast_idtx_scan_order_16x16 = {
  av1_fast_idtx_scan_16x16, av1_fast_idtx_iscan_16x16
};

static INLINE int early_term_inter_search_with_sse(int early_term_idx,
                                                   BLOCK_SIZE bsize,
                                                   int64_t this_sse,
                                                   int64_t best_sse,
                                                   PREDICTION_MODE this_mode) {
  // Aggressiveness to terminate inter mode search early is adjusted based on
  // speed and block size.
  static const double early_term_thresh[4][4] = { { 0.65, 0.65, 0.65, 0.7 },
                                                  { 0.6, 0.65, 0.85, 0.9 },
                                                  { 0.5, 0.5, 0.55, 0.6 },
                                                  { 0.6, 0.75, 0.85, 0.85 } };
  static const double early_term_thresh_newmv_nearestmv[4] = { 0.3, 0.3, 0.3,
                                                               0.3 };

  const int size_group = size_group_lookup[bsize];
  assert(size_group < 4);
  assert((early_term_idx > 0) && (early_term_idx < EARLY_TERM_INDICES));
  const double threshold =
      ((early_term_idx == EARLY_TERM_IDX_4) &&
       (this_mode == NEWMV || this_mode == NEARESTMV))
          ? early_term_thresh_newmv_nearestmv[size_group]
          : early_term_thresh[early_term_idx - 1][size_group];

  // Terminate inter mode search early based on best sse so far.
  if ((early_term_idx > 0) && (threshold * this_sse > best_sse)) {
    return 1;
  }
  return 0;
}

static INLINE void init_best_pickmode(BEST_PICKMODE *bp) {
  bp->best_sse = INT64_MAX;
  bp->best_mode = NEARESTMV;
  bp->best_ref_frame = LAST_FRAME;
  bp->best_second_ref_frame = NONE_FRAME;
  bp->best_tx_size = TX_8X8;
  bp->tx_type = DCT_DCT;
  bp->best_pred_filter = av1_broadcast_interp_filter(EIGHTTAP_REGULAR);
  bp->best_mode_skip_txfm = 0;
  bp->best_mode_initial_skip_flag = 0;
  bp->best_pred = NULL;
  bp->best_motion_mode = SIMPLE_TRANSLATION;
  bp->num_proj_ref = 0;
  av1_zero(bp->wm_params);
  av1_zero(bp->blk_skip);
  av1_zero(bp->pmi);
}

// Copy best inter mode parameters to best_pickmode
static INLINE void update_search_state_nonrd(
    InterModeSearchStateNonrd *search_state, MB_MODE_INFO *const mi,
    TxfmSearchInfo *txfm_info, RD_STATS *nonskip_rdc, PICK_MODE_CONTEXT *ctx,
    PREDICTION_MODE this_best_mode, const int64_t sse_y) {
  BEST_PICKMODE *const best_pickmode = &search_state->best_pickmode;
  const int num_8x8_blocks = ctx->num_4x4_blk / 4;

  best_pickmode->best_sse = sse_y;
  best_pickmode->best_mode = this_best_mode;
  best_pickmode->best_motion_mode = mi->motion_mode;
  best_pickmode->wm_params = mi->wm_params;
  best_pickmode->num_proj_ref = mi->num_proj_ref;
  best_pickmode->best_pred_filter = mi->interp_filters;
  best_pickmode->best_tx_size = mi->tx_size;
  best_pickmode->best_ref_frame = mi->ref_frame[0];
  best_pickmode->best_second_ref_frame = mi->ref_frame[1];
  best_pickmode->best_mode_skip_txfm = search_state->this_rdc.skip_txfm;
  best_pickmode->best_mode_initial_skip_flag =
      (nonskip_rdc->rate == INT_MAX && search_state->this_rdc.skip_txfm);
  if (!best_pickmode->best_mode_skip_txfm) {
    memcpy(best_pickmode->blk_skip, txfm_info->blk_skip,
           sizeof(txfm_info->blk_skip[0]) * num_8x8_blocks);
  }
}

static INLINE int subpel_select(AV1_COMP *cpi, MACROBLOCK *x, BLOCK_SIZE bsize,
                                int_mv *mv, MV ref_mv, FULLPEL_MV start_mv,
                                bool fullpel_performed_well) {
  const int frame_lowmotion = cpi->rc.avg_frame_low_motion;
  const int reduce_mv_pel_precision_highmotion =
      cpi->sf.rt_sf.reduce_mv_pel_precision_highmotion;

  // Reduce MV precision for higher int MV value & frame-level motion
  if (reduce_mv_pel_precision_highmotion >= 3) {
    int mv_thresh = 4;
    const int is_low_resoln =
        (cpi->common.width * cpi->common.height <= 320 * 240);
    mv_thresh = (bsize > BLOCK_32X32) ? 2 : (bsize > BLOCK_16X16) ? 4 : 6;
    if (frame_lowmotion > 0 && frame_lowmotion < 40) mv_thresh = 12;
    mv_thresh = (is_low_resoln) ? mv_thresh >> 1 : mv_thresh;
    if (abs(mv->as_fullmv.row) >= mv_thresh ||
        abs(mv->as_fullmv.col) >= mv_thresh)
      return HALF_PEL;
  } else if (reduce_mv_pel_precision_highmotion >= 1) {
    int mv_thresh;
    const int th_vals[2][3] = { { 4, 8, 10 }, { 4, 6, 8 } };
    const int th_idx = reduce_mv_pel_precision_highmotion - 1;
    assert(th_idx >= 0 && th_idx < 2);
    if (frame_lowmotion > 0 && frame_lowmotion < 40)
      mv_thresh = 12;
    else
      mv_thresh = (bsize >= BLOCK_32X32)   ? th_vals[th_idx][0]
                  : (bsize >= BLOCK_16X16) ? th_vals[th_idx][1]
                                           : th_vals[th_idx][2];
    if (abs(mv->as_fullmv.row) >= (mv_thresh << 1) ||
        abs(mv->as_fullmv.col) >= (mv_thresh << 1))
      return FULL_PEL;
    else if (abs(mv->as_fullmv.row) >= mv_thresh ||
             abs(mv->as_fullmv.col) >= mv_thresh)
      return HALF_PEL;
  }
  // Reduce MV precision for relatively static (e.g. background), low-complex
  // large areas
  if (cpi->sf.rt_sf.reduce_mv_pel_precision_lowcomplex >= 2) {
    const int qband = x->qindex >> (QINDEX_BITS - 2);
    assert(qband < 4);
    if (x->content_state_sb.source_sad_nonrd <= kVeryLowSad &&
        bsize > BLOCK_16X16 && qband != 0) {
      if (x->source_variance < 500)
        return FULL_PEL;
      else if (x->source_variance < 5000)
        return HALF_PEL;
    }
  } else if (cpi->sf.rt_sf.reduce_mv_pel_precision_lowcomplex >= 1) {
    if (fullpel_performed_well && ref_mv.row == 0 && ref_mv.col == 0 &&
        start_mv.row == 0 && start_mv.col == 0)
      return HALF_PEL;
  }
  return cpi->sf.mv_sf.subpel_force_stop;
}

static bool use_aggressive_subpel_search_method(MACROBLOCK *x,
                                                bool use_adaptive_subpel_search,
                                                bool fullpel_performed_well) {
  if (!use_adaptive_subpel_search) return false;
  const int qband = x->qindex >> (QINDEX_BITS - 2);
  assert(qband < 4);
  if ((qband > 0) && (fullpel_performed_well ||
                      (x->content_state_sb.source_sad_nonrd <= kLowSad) ||
                      (x->source_variance < 100)))
    return true;
  return false;
}

/*!\brief Runs Motion Estimation for a specific block and specific ref frame.
 *
 * \ingroup nonrd_mode_search
 * \callgraph
 * \callergraph
 * Finds the best Motion Vector by running Motion Estimation for a specific
 * block and a specific reference frame. Exits early if RDCost of Full Pel part
 * exceeds best RD Cost fund so far
 * \param[in]    cpi                      Top-level encoder structure
 * \param[in]    x                        Pointer to structure holding all the
 *                                        data for the current macroblock
 * \param[in]    bsize                    Current block size
 * \param[in]    mi_row                   Row index in 4x4 units
 * \param[in]    mi_col                   Column index in 4x4 units
 * \param[in]    tmp_mv                   Pointer to best found New MV
 * \param[in]    rate_mv                  Pointer to Rate of the best new MV
 * \param[in]    best_rd_sofar            RD Cost of the best mode found so far
 * \param[in]    use_base_mv              Flag, indicating that tmp_mv holds
 *                                        specific MV to start the search with
 *
 * \return Returns 0 if ME was terminated after Full Pel Search because too
 * high RD Cost. Otherwise returns 1. Best New MV is placed into \c tmp_mv.
 * Rate estimation for this vector is placed to \c rate_mv
 */
static int combined_motion_search(AV1_COMP *cpi, MACROBLOCK *x,
                                  BLOCK_SIZE bsize, int mi_row, int mi_col,
                                  int_mv *tmp_mv, int *rate_mv,
                                  int64_t best_rd_sofar, int use_base_mv) {
  MACROBLOCKD *xd = &x->e_mbd;
  const AV1_COMMON *cm = &cpi->common;
  const int num_planes = av1_num_planes(cm);
  const SPEED_FEATURES *sf = &cpi->sf;
  MB_MODE_INFO *mi = xd->mi[0];
  struct buf_2d backup_yv12[MAX_MB_PLANE] = { { 0, 0, 0, 0, 0 } };
  int step_param = (sf->rt_sf.fullpel_search_step_param)
                       ? sf->rt_sf.fullpel_search_step_param
                       : cpi->mv_search_params.mv_step_param;
  FULLPEL_MV start_mv;
  const int ref = mi->ref_frame[0];
  const MV ref_mv = av1_get_ref_mv(x, mi->ref_mv_idx).as_mv;
  MV center_mv;
  int dis;
  int rv = 0;
  int cost_list[5];
  int search_subpel = 1;
  const YV12_BUFFER_CONFIG *scaled_ref_frame =
      av1_get_scaled_ref_frame(cpi, ref);

  if (scaled_ref_frame) {
    int plane;
    // Swap out the reference frame for a version that's been scaled to
    // match the resolution of the current frame, allowing the existing
    // motion search code to be used without additional modifications.
    for (plane = 0; plane < MAX_MB_PLANE; plane++)
      backup_yv12[plane] = xd->plane[plane].pre[0];
    av1_setup_pre_planes(xd, 0, scaled_ref_frame, mi_row, mi_col, NULL,
                         num_planes);
  }

  start_mv = get_fullmv_from_mv(&ref_mv);

  if (!use_base_mv)
    center_mv = ref_mv;
  else
    center_mv = tmp_mv->as_mv;

  const SEARCH_METHODS search_method = sf->mv_sf.search_method;
  const MotionVectorSearchParams *mv_search_params = &cpi->mv_search_params;
  const int ref_stride = xd->plane[AOM_PLANE_Y].pre[0].stride;
  const search_site_config *src_search_sites = av1_get_search_site_config(
      x->search_site_cfg_buf, mv_search_params, search_method, ref_stride);
  FULLPEL_MOTION_SEARCH_PARAMS full_ms_params;
  av1_make_default_fullpel_ms_params(&full_ms_params, cpi, x, bsize, &center_mv,
                                     src_search_sites,
                                     /*fine_search_interval=*/0);

  const unsigned int full_var_rd = av1_full_pixel_search(
      start_mv, &full_ms_params, step_param, cond_cost_list(cpi, cost_list),
      &tmp_mv->as_fullmv, NULL);

  // calculate the bit cost on motion vector
  MV mvp_full = get_mv_from_fullmv(&tmp_mv->as_fullmv);

  *rate_mv = av1_mv_bit_cost(&mvp_full, &ref_mv, x->mv_costs->nmv_joint_cost,
                             x->mv_costs->mv_cost_stack, MV_COST_WEIGHT);

  // TODO(kyslov) Account for Rate Mode!
  rv = !(RDCOST(x->rdmult, (*rate_mv), 0) > best_rd_sofar);

  if (rv && search_subpel) {
    SUBPEL_MOTION_SEARCH_PARAMS ms_params;
    av1_make_default_subpel_ms_params(&ms_params, cpi, x, bsize, &ref_mv,
                                      cost_list);
    const bool fullpel_performed_well =
        (bsize == BLOCK_64X64 && full_var_rd * 40 < 62267 * 7) ||
        (bsize == BLOCK_32X32 && full_var_rd * 8 < 42380) ||
        (bsize == BLOCK_16X16 && full_var_rd * 8 < 10127);
    if (sf->rt_sf.reduce_mv_pel_precision_highmotion ||
        sf->rt_sf.reduce_mv_pel_precision_lowcomplex)
      ms_params.forced_stop = subpel_select(cpi, x, bsize, tmp_mv, ref_mv,
                                            start_mv, fullpel_performed_well);

    MV subpel_start_mv = get_mv_from_fullmv(&tmp_mv->as_fullmv);
    // adaptively downgrade subpel search method based on block properties
    if (use_aggressive_subpel_search_method(
            x, sf->rt_sf.use_adaptive_subpel_search, fullpel_performed_well))
      av1_find_best_sub_pixel_tree_pruned_more(xd, cm, &ms_params,
                                               subpel_start_mv, &tmp_mv->as_mv,
                                               &dis, &x->pred_sse[ref], NULL);
    else
      cpi->mv_search_params.find_fractional_mv_step(
          xd, cm, &ms_params, subpel_start_mv, &tmp_mv->as_mv, &dis,
          &x->pred_sse[ref], NULL);
    *rate_mv =
        av1_mv_bit_cost(&tmp_mv->as_mv, &ref_mv, x->mv_costs->nmv_joint_cost,
                        x->mv_costs->mv_cost_stack, MV_COST_WEIGHT);
  }

  if (scaled_ref_frame) {
    for (int plane = 0; plane < MAX_MB_PLANE; plane++)
      xd->plane[plane].pre[0] = backup_yv12[plane];
  }
  // The final MV can not be equal to the reference MV as this will trigger an
  // assert later. This can happen if both NEAREST and NEAR modes were skipped.
  rv = (tmp_mv->as_mv.col != ref_mv.col || tmp_mv->as_mv.row != ref_mv.row);
  return rv;
}

/*!\brief Searches for the best New Motion Vector.
 *
 * \ingroup nonrd_mode_search
 * \callgraph
 * \callergraph
 * Finds the best Motion Vector by doing Motion Estimation. Uses reduced
 * complexity ME for non-LAST frames or calls \c combined_motion_search
 * for LAST reference frame
 * \param[in]    cpi                      Top-level encoder structure
 * \param[in]    x                        Pointer to structure holding all the
 *                                        data for the current macroblock
 * \param[in]    frame_mv                 Array that holds MVs for all modes
 *                                        and ref frames
 * \param[in]    ref_frame                Reference frame for which to find
 *                                        the best New MVs
 * \param[in]    gf_temporal_ref          Flag, indicating temporal reference
 *                                        for GOLDEN frame
 * \param[in]    bsize                    Current block size
 * \param[in]    mi_row                   Row index in 4x4 units
 * \param[in]    mi_col                   Column index in 4x4 units
 * \param[in]    rate_mv                  Pointer to Rate of the best new MV
 * \param[in]    best_rdc                 Pointer to the RD Cost for the best
 *                                        mode found so far
 *
 * \return Returns -1 if the search was not done, otherwise returns 0.
 * Best New MV is placed into \c frame_mv array, Rate estimation for this
 * vector is placed to \c rate_mv
 */
static int search_new_mv(AV1_COMP *cpi, MACROBLOCK *x,
                         int_mv frame_mv[][REF_FRAMES],
                         MV_REFERENCE_FRAME ref_frame, int gf_temporal_ref,
                         BLOCK_SIZE bsize, int mi_row, int mi_col, int *rate_mv,
                         RD_STATS *best_rdc) {
  MACROBLOCKD *const xd = &x->e_mbd;
  MB_MODE_INFO *const mi = xd->mi[0];
  AV1_COMMON *cm = &cpi->common;
  int_mv *this_ref_frm_newmv = &frame_mv[NEWMV][ref_frame];
  if (ref_frame > LAST_FRAME && cpi->oxcf.rc_cfg.mode == AOM_CBR &&
      gf_temporal_ref) {
    int tmp_sad;
    int dis;

    if (bsize < BLOCK_16X16) return -1;

    tmp_sad = av1_int_pro_motion_estimation(
        cpi, x, bsize, mi_row, mi_col,
        &x->mbmi_ext.ref_mv_stack[ref_frame][0].this_mv.as_mv);

    if (tmp_sad > x->pred_mv_sad[LAST_FRAME]) return -1;

    this_ref_frm_newmv->as_int = mi->mv[0].as_int;
    int_mv best_mv = mi->mv[0];
    best_mv.as_mv.row >>= 3;
    best_mv.as_mv.col >>= 3;
    MV ref_mv = av1_get_ref_mv(x, 0).as_mv;
    this_ref_frm_newmv->as_mv.row >>= 3;
    this_ref_frm_newmv->as_mv.col >>= 3;

    SUBPEL_MOTION_SEARCH_PARAMS ms_params;
    av1_make_default_subpel_ms_params(&ms_params, cpi, x, bsize, &ref_mv, NULL);
    if (cpi->sf.rt_sf.reduce_mv_pel_precision_highmotion ||
        cpi->sf.rt_sf.reduce_mv_pel_precision_lowcomplex) {
      FULLPEL_MV start_mv = { .row = 0, .col = 0 };
      ms_params.forced_stop =
          subpel_select(cpi, x, bsize, &best_mv, ref_mv, start_mv, false);
    }
    MV start_mv = get_mv_from_fullmv(&best_mv.as_fullmv);
    cpi->mv_search_params.find_fractional_mv_step(
        xd, cm, &ms_params, start_mv, &best_mv.as_mv, &dis,
        &x->pred_sse[ref_frame], NULL);
    this_ref_frm_newmv->as_int = best_mv.as_int;

    // When NEWMV is same as ref_mv from the drl, it is preferred to code the
    // MV as NEARESTMV or NEARMV. In this case, NEWMV needs to be skipped to
    // avoid an assert failure at a later stage. The scenario can occur if
    // NEARESTMV was not evaluated for ALTREF.
    if (this_ref_frm_newmv->as_mv.col == ref_mv.col &&
        this_ref_frm_newmv->as_mv.row == ref_mv.row)
      return -1;

    *rate_mv = av1_mv_bit_cost(&this_ref_frm_newmv->as_mv, &ref_mv,
                               x->mv_costs->nmv_joint_cost,
                               x->mv_costs->mv_cost_stack, MV_COST_WEIGHT);
  } else if (!combined_motion_search(cpi, x, bsize, mi_row, mi_col,
                                     &frame_mv[NEWMV][ref_frame], rate_mv,
                                     best_rdc->rdcost, 0)) {
    return -1;
  }

  return 0;
}

static void estimate_single_ref_frame_costs(const AV1_COMMON *cm,
                                            const MACROBLOCKD *xd,
                                            const ModeCosts *mode_costs,
                                            int segment_id, BLOCK_SIZE bsize,
                                            unsigned int *ref_costs_single) {
  int seg_ref_active =
      segfeature_active(&cm->seg, segment_id, SEG_LVL_REF_FRAME);
  if (seg_ref_active) {
    memset(ref_costs_single, 0, REF_FRAMES * sizeof(*ref_costs_single));
  } else {
    int intra_inter_ctx = av1_get_intra_inter_context(xd);
    ref_costs_single[INTRA_FRAME] =
        mode_costs->intra_inter_cost[intra_inter_ctx][0];
    unsigned int base_cost = mode_costs->intra_inter_cost[intra_inter_ctx][1];
    if (cm->current_frame.reference_mode == REFERENCE_MODE_SELECT &&
        is_comp_ref_allowed(bsize)) {
      const int comp_ref_type_ctx = av1_get_comp_reference_type_context(xd);
      base_cost += mode_costs->comp_ref_type_cost[comp_ref_type_ctx][1];
    }
    ref_costs_single[LAST_FRAME] = base_cost;
    ref_costs_single[GOLDEN_FRAME] = base_cost;
    ref_costs_single[ALTREF_FRAME] = base_cost;
    // add cost for last, golden, altref
    ref_costs_single[LAST_FRAME] += mode_costs->single_ref_cost[0][0][0];
    ref_costs_single[GOLDEN_FRAME] += mode_costs->single_ref_cost[0][0][1];
    ref_costs_single[GOLDEN_FRAME] += mode_costs->single_ref_cost[0][1][0];
    ref_costs_single[ALTREF_FRAME] += mode_costs->single_ref_cost[0][0][1];
    ref_costs_single[ALTREF_FRAME] += mode_costs->single_ref_cost[0][2][0];
  }
}

static INLINE void set_force_skip_flag(const AV1_COMP *const cpi,
                                       MACROBLOCK *const x, unsigned int sse,
                                       int *force_skip) {
  if (x->txfm_search_params.tx_mode_search_type == TX_MODE_SELECT &&
      cpi->sf.rt_sf.tx_size_level_based_on_qstep &&
      cpi->sf.rt_sf.tx_size_level_based_on_qstep >= 2) {
    const int qstep = x->plane[AOM_PLANE_Y].dequant_QTX[1] >> (x->e_mbd.bd - 5);
    const unsigned int qstep_sq = qstep * qstep;
    // If the sse is low for low source variance blocks, mark those as
    // transform skip.
    // Note: Though qstep_sq is based on ac qstep, the threshold is kept
    // low so that reliable early estimate of tx skip can be obtained
    // through its comparison with sse.
    if (sse < qstep_sq && x->source_variance < qstep_sq &&
        x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_U)] == 0 &&
        x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_V)] == 0)
      *force_skip = 1;
  }
}

#define CAP_TX_SIZE_FOR_BSIZE_GT32(tx_mode_search_type, bsize) \
  (((tx_mode_search_type) != ONLY_4X4 && (bsize) > BLOCK_32X32) ? true : false)
#define TX_SIZE_FOR_BSIZE_GT32 (TX_16X16)

static TX_SIZE calculate_tx_size(const AV1_COMP *const cpi, BLOCK_SIZE bsize,
                                 MACROBLOCK *const x, unsigned int var,
                                 unsigned int sse, int *force_skip) {
  MACROBLOCKD *const xd = &x->e_mbd;
  TX_SIZE tx_size;
  const TxfmSearchParams *txfm_params = &x->txfm_search_params;
  if (txfm_params->tx_mode_search_type == TX_MODE_SELECT) {
    int multiplier = 8;
    unsigned int var_thresh = 0;
    unsigned int is_high_var = 1;
    // Use quantizer based thresholds to determine transform size.
    if (cpi->sf.rt_sf.tx_size_level_based_on_qstep) {
      const int qband = x->qindex >> (QINDEX_BITS - 2);
      const int mult[4] = { 8, 7, 6, 5 };
      assert(qband < 4);
      multiplier = mult[qband];
      const int qstep = x->plane[AOM_PLANE_Y].dequant_QTX[1] >> (xd->bd - 5);
      const unsigned int qstep_sq = qstep * qstep;
      var_thresh = qstep_sq * 2;
      if (cpi->sf.rt_sf.tx_size_level_based_on_qstep >= 2) {
        // If the sse is low for low source variance blocks, mark those as
        // transform skip.
        // Note: Though qstep_sq is based on ac qstep, the threshold is kept
        // low so that reliable early estimate of tx skip can be obtained
        // through its comparison with sse.
        if (sse < qstep_sq && x->source_variance < qstep_sq &&
            x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_U)] == 0 &&
            x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_V)] == 0)
          *force_skip = 1;
        // Further lower transform size based on aq mode only if residual
        // variance is high.
        is_high_var = (var >= var_thresh);
      }
    }
    // Choose larger transform size for blocks where dc component is dominant or
    // the ac component is low.
    if (sse > ((var * multiplier) >> 2) || (var < var_thresh))
      tx_size =
          AOMMIN(max_txsize_lookup[bsize],
                 tx_mode_to_biggest_tx_size[txfm_params->tx_mode_search_type]);
    else
      tx_size = TX_8X8;

    if (cpi->oxcf.q_cfg.aq_mode == CYCLIC_REFRESH_AQ &&
        cyclic_refresh_segment_id_boosted(xd->mi[0]->segment_id) && is_high_var)
      tx_size = TX_8X8;
    else if (tx_size > TX_16X16)
      tx_size = TX_16X16;
  } else {
    tx_size =
        AOMMIN(max_txsize_lookup[bsize],
               tx_mode_to_biggest_tx_size[txfm_params->tx_mode_search_type]);
  }

  if (CAP_TX_SIZE_FOR_BSIZE_GT32(txfm_params->tx_mode_search_type, bsize))
    tx_size = TX_SIZE_FOR_BSIZE_GT32;

  return AOMMIN(tx_size, TX_16X16);
}

static const uint8_t b_width_log2_lookup[BLOCK_SIZES] = { 0, 0, 1, 1, 1, 2,
                                                          2, 2, 3, 3, 3, 4,
                                                          4, 4, 5, 5 };
static const uint8_t b_height_log2_lookup[BLOCK_SIZES] = { 0, 1, 0, 1, 2, 1,
                                                           2, 3, 2, 3, 4, 3,
                                                           4, 5, 4, 5 };

static void block_variance(const uint8_t *src, int src_stride,
                           const uint8_t *ref, int ref_stride, int w, int h,
                           unsigned int *sse, int *sum, int block_size,
                           uint32_t *sse8x8, int *sum8x8, uint32_t *var8x8) {
  int k = 0;
  *sse = 0;
  *sum = 0;

  // This function is called for block sizes >= BLOCK_32x32. As per the design
  // the aom_get_var_sse_sum_8x8_quad() processes four 8x8 blocks (in a 8x32)
  // per call. Hence the width and height of the block need to be at least 8 and
  // 32 samples respectively.
  assert(w >= 32);
  assert(h >= 8);
  for (int row = 0; row < h; row += block_size) {
    for (int col = 0; col < w; col += 32) {
      aom_get_var_sse_sum_8x8_quad(src + src_stride * row + col, src_stride,
                                   ref + ref_stride * row + col, ref_stride,
                                   &sse8x8[k], &sum8x8[k], sse, sum,
                                   &var8x8[k]);
      k += 4;
    }
  }
}

static void block_variance_16x16_dual(const uint8_t *src, int src_stride,
                                      const uint8_t *ref, int ref_stride, int w,
                                      int h, unsigned int *sse, int *sum,
                                      int block_size, uint32_t *sse16x16,
                                      uint32_t *var16x16) {
  int k = 0;
  *sse = 0;
  *sum = 0;
  // This function is called for block sizes >= BLOCK_32x32. As per the design
  // the aom_get_var_sse_sum_16x16_dual() processes four 16x16 blocks (in a
  // 16x32) per call. Hence the width and height of the block need to be at
  // least 16 and 32 samples respectively.
  assert(w >= 32);
  assert(h >= 16);
  for (int row = 0; row < h; row += block_size) {
    for (int col = 0; col < w; col += 32) {
      aom_get_var_sse_sum_16x16_dual(src + src_stride * row + col, src_stride,
                                     ref + ref_stride * row + col, ref_stride,
                                     &sse16x16[k], sse, sum, &var16x16[k]);
      k += 2;
    }
  }
}

static void calculate_variance(int bw, int bh, TX_SIZE tx_size,
                               unsigned int *sse_i, int *sum_i,
                               unsigned int *var_o, unsigned int *sse_o,
                               int *sum_o) {
  const BLOCK_SIZE unit_size = txsize_to_bsize[tx_size];
  const int nw = 1 << (bw - b_width_log2_lookup[unit_size]);
  const int nh = 1 << (bh - b_height_log2_lookup[unit_size]);
  int row, col, k = 0;

  for (row = 0; row < nh; row += 2) {
    for (col = 0; col < nw; col += 2) {
      sse_o[k] = sse_i[row * nw + col] + sse_i[row * nw + col + 1] +
                 sse_i[(row + 1) * nw + col] + sse_i[(row + 1) * nw + col + 1];
      sum_o[k] = sum_i[row * nw + col] + sum_i[row * nw + col + 1] +
                 sum_i[(row + 1) * nw + col] + sum_i[(row + 1) * nw + col + 1];
      var_o[k] = sse_o[k] - (uint32_t)(((int64_t)sum_o[k] * sum_o[k]) >>
                                       (b_width_log2_lookup[unit_size] +
                                        b_height_log2_lookup[unit_size] + 6));
      k++;
    }
  }
}

// Adjust the ac_thr according to speed, width, height and normalized sum
static int ac_thr_factor(int speed, int width, int height, int norm_sum) {
  if (speed >= 8 && norm_sum < 5) {
    if (width <= 640 && height <= 480)
      return 4;
    else
      return 2;
  }
  return 1;
}

// Sets early_term flag based on chroma planes prediction
static INLINE void set_early_term_based_on_uv_plane(
    AV1_COMP *cpi, MACROBLOCK *x, BLOCK_SIZE bsize, MACROBLOCKD *xd, int mi_row,
    int mi_col, int *early_term, int num_blk, const unsigned int *sse_tx,
    const unsigned int *var_tx, int sum, unsigned int var, unsigned int sse) {
  AV1_COMMON *const cm = &cpi->common;
  struct macroblock_plane *const p = &x->plane[AOM_PLANE_Y];
  const uint32_t dc_quant = p->dequant_QTX[0];
  const uint32_t ac_quant = p->dequant_QTX[1];
  const int64_t dc_thr = dc_quant * dc_quant >> 6;
  int64_t ac_thr = ac_quant * ac_quant >> 6;
  const int bw = b_width_log2_lookup[bsize];
  const int bh = b_height_log2_lookup[bsize];
  int ac_test = 1;
  int dc_test = 1;
  const int norm_sum = abs(sum) >> (bw + bh);

#if CONFIG_AV1_TEMPORAL_DENOISING
  if (cpi->oxcf.noise_sensitivity > 0 && denoise_svc(cpi) &&
      cpi->oxcf.speed > 5)
    ac_thr = av1_scale_acskip_thresh(ac_thr, cpi->denoiser.denoising_level,
                                     norm_sum, cpi->svc.temporal_layer_id);
  else
    ac_thr *= ac_thr_factor(cpi->oxcf.speed, cm->width, cm->height, norm_sum);
#else
  ac_thr *= ac_thr_factor(cpi->oxcf.speed, cm->width, cm->height, norm_sum);

#endif

  for (int k = 0; k < num_blk; k++) {
    // Check if all ac coefficients can be quantized to zero.
    if (!(var_tx[k] < ac_thr || var == 0)) {
      ac_test = 0;
      break;
    }
    // Check if dc coefficient can be quantized to zero.
    if (!(sse_tx[k] - var_tx[k] < dc_thr || sse == var)) {
      dc_test = 0;
      break;
    }
  }

  // Check if chroma can be skipped based on ac and dc test flags.
  if (ac_test && dc_test) {
    int skip_uv[2] = { 0 };
    unsigned int var_uv[2];
    unsigned int sse_uv[2];
    // Transform skipping test in UV planes.
    for (int plane = AOM_PLANE_U; plane <= AOM_PLANE_V; plane++) {
      int j = plane - 1;
      skip_uv[j] = 1;
      if (x->color_sensitivity[COLOR_SENS_IDX(plane)]) {
        skip_uv[j] = 0;
        struct macroblock_plane *const puv = &x->plane[plane];
        struct macroblockd_plane *const puvd = &xd->plane[plane];
        const BLOCK_SIZE uv_bsize = get_plane_block_size(
            bsize, puvd->subsampling_x, puvd->subsampling_y);
        // Adjust these thresholds for UV.
        const int64_t uv_dc_thr =
            (puv->dequant_QTX[0] * puv->dequant_QTX[0]) >> 3;
        const int64_t uv_ac_thr =
            (puv->dequant_QTX[1] * puv->dequant_QTX[1]) >> 3;
        av1_enc_build_inter_predictor(cm, xd, mi_row, mi_col, NULL, bsize,
                                      plane, plane);
        var_uv[j] = cpi->ppi->fn_ptr[uv_bsize].vf(puv->src.buf, puv->src.stride,
                                                  puvd->dst.buf,
                                                  puvd->dst.stride, &sse_uv[j]);
        if ((var_uv[j] < uv_ac_thr || var_uv[j] == 0) &&
            (sse_uv[j] - var_uv[j] < uv_dc_thr || sse_uv[j] == var_uv[j]))
          skip_uv[j] = 1;
        else
          break;
      }
    }
    if (skip_uv[0] & skip_uv[1]) {
      *early_term = 1;
    }
  }
}

static INLINE void calc_rate_dist_block_param(AV1_COMP *cpi, MACROBLOCK *x,
                                              RD_STATS *rd_stats,
                                              int calculate_rd, int *early_term,
                                              BLOCK_SIZE bsize,
                                              unsigned int sse) {
  if (calculate_rd) {
    if (!*early_term) {
      const int bw = block_size_wide[bsize];
      const int bh = block_size_high[bsize];

      model_rd_with_curvfit(cpi, x, bsize, AOM_PLANE_Y, rd_stats->sse, bw * bh,
                            &rd_stats->rate, &rd_stats->dist);
    }

    if (*early_term) {
      rd_stats->rate = 0;
      rd_stats->dist = sse << 4;
    }
  }
}

static void model_skip_for_sb_y_large_64(AV1_COMP *cpi, BLOCK_SIZE bsize,
                                         int mi_row, int mi_col, MACROBLOCK *x,
                                         MACROBLOCKD *xd, RD_STATS *rd_stats,
                                         int *early_term, int calculate_rd,
                                         int64_t best_sse,
                                         unsigned int *var_output,
                                         unsigned int var_prune_threshold) {
  // Note our transform coeffs are 8 times an orthogonal transform.
  // Hence quantizer step is also 8 times. To get effective quantizer
  // we need to divide by 8 before sending to modeling function.
  unsigned int sse;
  struct macroblock_plane *const p = &x->plane[AOM_PLANE_Y];
  struct macroblockd_plane *const pd = &xd->plane[AOM_PLANE_Y];
  int test_skip = 1;
  unsigned int var;
  int sum;
  const int bw = b_width_log2_lookup[bsize];
  const int bh = b_height_log2_lookup[bsize];
  unsigned int sse16x16[64] = { 0 };
  unsigned int var16x16[64] = { 0 };
  assert(xd->mi[0]->tx_size == TX_16X16);
  assert(bsize > BLOCK_32X32);

  // Calculate variance for whole partition, and also save 16x16 blocks'
  // variance to be used in following transform skipping test.
  block_variance_16x16_dual(p->src.buf, p->src.stride, pd->dst.buf,
                            pd->dst.stride, 4 << bw, 4 << bh, &sse, &sum, 16,
                            sse16x16, var16x16);

  var = sse - (unsigned int)(((int64_t)sum * sum) >> (bw + bh + 4));
  if (var_output) {
    *var_output = var;
    if (*var_output > var_prune_threshold) {
      return;
    }
  }

  rd_stats->sse = sse;
  // Skipping test
  *early_term = 0;
  set_force_skip_flag(cpi, x, sse, early_term);
  // The code below for setting skip flag assumes transform size of at least
  // 8x8, so force this lower limit on transform.
  MB_MODE_INFO *const mi = xd->mi[0];
  if (!calculate_rd && cpi->sf.rt_sf.sse_early_term_inter_search &&
      early_term_inter_search_with_sse(
          cpi->sf.rt_sf.sse_early_term_inter_search, bsize, sse, best_sse,
          mi->mode))
    test_skip = 0;

  if (*early_term) test_skip = 0;

  // Evaluate if the partition block is a skippable block in Y plane.
  if (test_skip) {
    const unsigned int *sse_tx = sse16x16;
    const unsigned int *var_tx = var16x16;
    const unsigned int num_block = (1 << (bw + bh - 2)) >> 2;
    set_early_term_based_on_uv_plane(cpi, x, bsize, xd, mi_row, mi_col,
                                     early_term, num_block, sse_tx, var_tx, sum,
                                     var, sse);
  }
  calc_rate_dist_block_param(cpi, x, rd_stats, calculate_rd, early_term, bsize,
                             sse);
}

static void model_skip_for_sb_y_large(AV1_COMP *cpi, BLOCK_SIZE bsize,
                                      int mi_row, int mi_col, MACROBLOCK *x,
                                      MACROBLOCKD *xd, RD_STATS *rd_stats,
                                      int *early_term, int calculate_rd,
                                      int64_t best_sse,
                                      unsigned int *var_output,
                                      unsigned int var_prune_threshold) {
  if (x->force_zeromv_skip_for_blk) {
    *early_term = 1;
    rd_stats->rate = 0;
    rd_stats->dist = 0;
    rd_stats->sse = 0;
    return;
  }

  // For block sizes greater than 32x32, the transform size is always 16x16.
  // This function avoids calling calculate_variance() for tx_size 16x16 cases
  // by directly populating variance at tx_size level from
  // block_variance_16x16_dual() function.
  const TxfmSearchParams *txfm_params = &x->txfm_search_params;
  if (CAP_TX_SIZE_FOR_BSIZE_GT32(txfm_params->tx_mode_search_type, bsize)) {
    xd->mi[0]->tx_size = TX_SIZE_FOR_BSIZE_GT32;
    model_skip_for_sb_y_large_64(cpi, bsize, mi_row, mi_col, x, xd, rd_stats,
                                 early_term, calculate_rd, best_sse, var_output,
                                 var_prune_threshold);
    return;
  }

  // Note our transform coeffs are 8 times an orthogonal transform.
  // Hence quantizer step is also 8 times. To get effective quantizer
  // we need to divide by 8 before sending to modeling function.
  unsigned int sse;
  struct macroblock_plane *const p = &x->plane[AOM_PLANE_Y];
  struct macroblockd_plane *const pd = &xd->plane[AOM_PLANE_Y];
  int test_skip = 1;
  unsigned int var;
  int sum;

  const int bw = b_width_log2_lookup[bsize];
  const int bh = b_height_log2_lookup[bsize];
  unsigned int sse8x8[256] = { 0 };
  int sum8x8[256] = { 0 };
  unsigned int var8x8[256] = { 0 };
  TX_SIZE tx_size;

  // Calculate variance for whole partition, and also save 8x8 blocks' variance
  // to be used in following transform skipping test.
  block_variance(p->src.buf, p->src.stride, pd->dst.buf, pd->dst.stride,
                 4 << bw, 4 << bh, &sse, &sum, 8, sse8x8, sum8x8, var8x8);
  var = sse - (unsigned int)(((int64_t)sum * sum) >> (bw + bh + 4));
  if (var_output) {
    *var_output = var;
    if (*var_output > var_prune_threshold) {
      return;
    }
  }

  rd_stats->sse = sse;
  // Skipping test
  *early_term = 0;
  tx_size = calculate_tx_size(cpi, bsize, x, var, sse, early_term);
  assert(tx_size <= TX_16X16);
  // The code below for setting skip flag assumes transform size of at least
  // 8x8, so force this lower limit on transform.
  if (tx_size < TX_8X8) tx_size = TX_8X8;
  xd->mi[0]->tx_size = tx_size;

  MB_MODE_INFO *const mi = xd->mi[0];
  if (!calculate_rd && cpi->sf.rt_sf.sse_early_term_inter_search &&
      early_term_inter_search_with_sse(
          cpi->sf.rt_sf.sse_early_term_inter_search, bsize, sse, best_sse,
          mi->mode))
    test_skip = 0;

  if (*early_term) test_skip = 0;

  // Evaluate if the partition block is a skippable block in Y plane.
  if (test_skip) {
    unsigned int sse16x16[64] = { 0 };
    int sum16x16[64] = { 0 };
    unsigned int var16x16[64] = { 0 };
    const unsigned int *sse_tx = sse8x8;
    const unsigned int *var_tx = var8x8;
    unsigned int num_blks = 1 << (bw + bh - 2);

    if (tx_size >= TX_16X16) {
      calculate_variance(bw, bh, TX_8X8, sse8x8, sum8x8, var16x16, sse16x16,
                         sum16x16);
      sse_tx = sse16x16;
      var_tx = var16x16;
      num_blks = num_blks >> 2;
    }
    set_early_term_based_on_uv_plane(cpi, x, bsize, xd, mi_row, mi_col,
                                     early_term, num_blks, sse_tx, var_tx, sum,
                                     var, sse);
  }
  calc_rate_dist_block_param(cpi, x, rd_stats, calculate_rd, early_term, bsize,
                             sse);
}

static void model_rd_for_sb_y(const AV1_COMP *const cpi, BLOCK_SIZE bsize,
                              MACROBLOCK *x, MACROBLOCKD *xd,
                              RD_STATS *rd_stats, unsigned int *var_out,
                              int calculate_rd, int *early_term) {
  if (x->force_zeromv_skip_for_blk && early_term != NULL) {
    *early_term = 1;
    rd_stats->rate = 0;
    rd_stats->dist = 0;
    rd_stats->sse = 0;
  }

  // Note our transform coeffs are 8 times an orthogonal transform.
  // Hence quantizer step is also 8 times. To get effective quantizer
  // we need to divide by 8 before sending to modeling function.
  const int ref = xd->mi[0]->ref_frame[0];

  assert(bsize < BLOCK_SIZES_ALL);

  struct macroblock_plane *const p = &x->plane[AOM_PLANE_Y];
  struct macroblockd_plane *const pd = &xd->plane[AOM_PLANE_Y];
  unsigned int sse;
  int rate;
  int64_t dist;

  unsigned int var = cpi->ppi->fn_ptr[bsize].vf(
      p->src.buf, p->src.stride, pd->dst.buf, pd->dst.stride, &sse);
  int force_skip = 0;
  xd->mi[0]->tx_size = calculate_tx_size(cpi, bsize, x, var, sse, &force_skip);
  if (var_out) {
    *var_out = var;
  }

  if (calculate_rd && (!force_skip || ref == INTRA_FRAME)) {
    const int bwide = block_size_wide[bsize];
    const int bhigh = block_size_high[bsize];
    model_rd_with_curvfit(cpi, x, bsize, AOM_PLANE_Y, sse, bwide * bhigh, &rate,
                          &dist);
  } else {
    rate = INT_MAX;  // this will be overwritten later with block_yrd
    dist = INT_MAX;
  }
  rd_stats->sse = sse;
  x->pred_sse[ref] = (unsigned int)AOMMIN(sse, UINT_MAX);

  if (force_skip && ref > INTRA_FRAME) {
    rate = 0;
    dist = (int64_t)sse << 4;
  }

  assert(rate >= 0);

  rd_stats->skip_txfm = (rate == 0);
  rate = AOMMIN(rate, INT_MAX);
  rd_stats->rate = rate;
  rd_stats->dist = dist;
}

static INLINE void aom_process_hadamard_lp_8x16(MACROBLOCK *x,
                                                int max_blocks_high,
                                                int max_blocks_wide,
                                                int num_4x4_w, int step,
                                                int block_step) {
  struct macroblock_plane *const p = &x->plane[AOM_PLANE_Y];
  const int bw = 4 * num_4x4_w;
  const int num_4x4 = AOMMIN(num_4x4_w, max_blocks_wide);
  int block = 0;

  for (int r = 0; r < max_blocks_high; r += block_step) {
    for (int c = 0; c < num_4x4; c += 2 * block_step) {
      const int16_t *src_diff = &p->src_diff[(r * bw + c) << 2];
      int16_t *low_coeff = (int16_t *)p->coeff + BLOCK_OFFSET(block);
      aom_hadamard_lp_8x8_dual(src_diff, (ptrdiff_t)bw, low_coeff);
      block += 2 * step;
    }
  }
}

#define DECLARE_BLOCK_YRD_BUFFERS()                      \
  DECLARE_ALIGNED(64, tran_low_t, dqcoeff_buf[16 * 16]); \
  DECLARE_ALIGNED(64, tran_low_t, qcoeff_buf[16 * 16]);  \
  DECLARE_ALIGNED(64, tran_low_t, coeff_buf[16 * 16]);   \
  uint16_t eob[1];

#define DECLARE_BLOCK_YRD_VARS()                                          \
  /* When is_tx_8x8_dual_applicable is true, we compute the txfm for the  \
   * entire bsize and write macroblock_plane::coeff. So low_coeff is kept \
   * as a non-const so we can reassign it to macroblock_plane::coeff. */  \
  int16_t *low_coeff = (int16_t *)coeff_buf;                              \
  int16_t *const low_qcoeff = (int16_t *)qcoeff_buf;                      \
  int16_t *const low_dqcoeff = (int16_t *)dqcoeff_buf;                    \
  const int diff_stride = bw;

#define DECLARE_LOOP_VARS_BLOCK_YRD() \
  const int16_t *src_diff = &p->src_diff[(r * diff_stride + c) << 2];

#if CONFIG_AV1_HIGHBITDEPTH
#define DECLARE_BLOCK_YRD_HBD_VARS()     \
  tran_low_t *const coeff = coeff_buf;   \
  tran_low_t *const qcoeff = qcoeff_buf; \
  tran_low_t *const dqcoeff = dqcoeff_buf;

static AOM_FORCE_INLINE void update_yrd_loop_vars_hbd(
    MACROBLOCK *x, int *skippable, int step, int ncoeffs,
    tran_low_t *const coeff, tran_low_t *const qcoeff,
    tran_low_t *const dqcoeff, RD_STATS *this_rdc, int *eob_cost,
    int tx_blk_id) {
  const int is_txfm_skip = (ncoeffs == 0);
  *skippable &= is_txfm_skip;
  x->txfm_search_info.blk_skip[tx_blk_id] = is_txfm_skip;
  *eob_cost += get_msb(ncoeffs + 1);

  int64_t dummy;
  if (ncoeffs == 1)
    this_rdc->rate += (int)abs(qcoeff[0]);
  else if (ncoeffs > 1)
    this_rdc->rate += aom_satd(qcoeff, step << 4);

  this_rdc->dist += av1_block_error(coeff, dqcoeff, step << 4, &dummy) >> 2;
}
#endif
static AOM_FORCE_INLINE void update_yrd_loop_vars(
    MACROBLOCK *x, int *skippable, int step, int ncoeffs,
    int16_t *const low_coeff, int16_t *const low_qcoeff,
    int16_t *const low_dqcoeff, RD_STATS *this_rdc, int *eob_cost,
    int tx_blk_id) {
  const int is_txfm_skip = (ncoeffs == 0);
  *skippable &= is_txfm_skip;
  x->txfm_search_info.blk_skip[tx_blk_id] = is_txfm_skip;
  *eob_cost += get_msb(ncoeffs + 1);
  if (ncoeffs == 1)
    this_rdc->rate += (int)abs(low_qcoeff[0]);
  else if (ncoeffs > 1)
    this_rdc->rate += aom_satd_lp(low_qcoeff, step << 4);

  this_rdc->dist += av1_block_error_lp(low_coeff, low_dqcoeff, step << 4) >> 2;
}

/*!\brief Calculates RD Cost using Hadamard transform.
 *
 * \ingroup nonrd_mode_search
 * \callgraph
 * \callergraph
 * Calculates RD Cost using Hadamard transform. For low bit depth this function
 * uses low-precision set of functions (16-bit) and 32 bit for high bit depth
 * \param[in]    x              Pointer to structure holding all the data for
                                the current macroblock
 * \param[in]    this_rdc       Pointer to calculated RD Cost
 * \param[in]    skippable      Pointer to a flag indicating possible tx skip
 * \param[in]    bsize          Current block size
 * \param[in]    tx_size        Transform size
 * \param[in]    is_inter_mode  Flag to indicate inter mode
 *
 * \remark Nothing is returned. Instead, calculated RD cost is placed to
 * \c this_rdc. \c skippable flag is set if there is no non-zero quantized
 * coefficients for Hadamard transform
 */
static void block_yrd(MACROBLOCK *x, RD_STATS *this_rdc, int *skippable,
                      BLOCK_SIZE bsize, TX_SIZE tx_size, int is_inter_mode) {
  MACROBLOCKD *xd = &x->e_mbd;
  const struct macroblockd_plane *pd = &xd->plane[AOM_PLANE_Y];
  struct macroblock_plane *const p = &x->plane[AOM_PLANE_Y];
  assert(bsize < BLOCK_SIZES_ALL);
  const int num_4x4_w = mi_size_wide[bsize];
  const int num_4x4_h = mi_size_high[bsize];
  const int step = 1 << (tx_size << 1);
  const int block_step = (1 << tx_size);
  const int row_step = step * num_4x4_w >> tx_size;
  int block = 0;
  const int max_blocks_wide =
      num_4x4_w + (xd->mb_to_right_edge >= 0 ? 0 : xd->mb_to_right_edge >> 5);
  const int max_blocks_high =
      num_4x4_h + (xd->mb_to_bottom_edge >= 0 ? 0 : xd->mb_to_bottom_edge >> 5);
  int eob_cost = 0;
  const int bw = 4 * num_4x4_w;
  const int bh = 4 * num_4x4_h;
  const int use_hbd = is_cur_buf_hbd(xd);
  int num_blk_skip_w = num_4x4_w;
  int sh_blk_skip = 0;
  if (is_inter_mode) {
    num_blk_skip_w = num_4x4_w >> 1;
    sh_blk_skip = 1;
  }

#if CONFIG_AV1_HIGHBITDEPTH
  if (use_hbd) {
    aom_highbd_subtract_block(bh, bw, p->src_diff, bw, p->src.buf,
                              p->src.stride, pd->dst.buf, pd->dst.stride);
  } else {
    aom_subtract_block(bh, bw, p->src_diff, bw, p->src.buf, p->src.stride,
                       pd->dst.buf, pd->dst.stride);
  }
#else
  aom_subtract_block(bh, bw, p->src_diff, bw, p->src.buf, p->src.stride,
                     pd->dst.buf, pd->dst.stride);
#endif

  // Keep the intermediate value on the stack here. Writing directly to
  // skippable causes speed regression due to load-and-store issues in
  // update_yrd_loop_vars.
  int temp_skippable = 1;
  this_rdc->dist = 0;
  this_rdc->rate = 0;
  // For block sizes 8x16 or above, Hadamard txfm of two adjacent 8x8 blocks
  // can be done per function call. Hence the call of Hadamard txfm is
  // abstracted here for the specified cases.
  int is_tx_8x8_dual_applicable =
      (tx_size == TX_8X8 && block_size_wide[bsize] >= 16 &&
       block_size_high[bsize] >= 8);

#if CONFIG_AV1_HIGHBITDEPTH
  // As of now, dual implementation of hadamard txfm is available for low
  // bitdepth.
  if (use_hbd) is_tx_8x8_dual_applicable = 0;
#endif

  if (is_tx_8x8_dual_applicable) {
    aom_process_hadamard_lp_8x16(x, max_blocks_high, max_blocks_wide, num_4x4_w,
                                 step, block_step);
  }

  const SCAN_ORDER *const scan_order = &av1_scan_orders[tx_size][DCT_DCT];
  DECLARE_BLOCK_YRD_BUFFERS()
  DECLARE_BLOCK_YRD_VARS()
#if CONFIG_AV1_HIGHBITDEPTH
  DECLARE_BLOCK_YRD_HBD_VARS()
#else
  (void)use_hbd;
#endif

  // Keep track of the row and column of the blocks we use so that we know
  // if we are in the unrestricted motion border.
  for (int r = 0; r < max_blocks_high; r += block_step) {
    for (int c = 0, s = 0; c < max_blocks_wide; c += block_step, s += step) {
      DECLARE_LOOP_VARS_BLOCK_YRD()

      switch (tx_size) {
#if CONFIG_AV1_HIGHBITDEPTH
        case TX_16X16:
          if (use_hbd) {
            aom_hadamard_16x16(src_diff, diff_stride, coeff);
            av1_quantize_fp(coeff, 16 * 16, p->zbin_QTX, p->round_fp_QTX,
                            p->quant_fp_QTX, p->quant_shift_QTX, qcoeff,
                            dqcoeff, p->dequant_QTX, eob,
                            // default_scan_fp_16x16_transpose and
                            // av1_default_iscan_fp_16x16_transpose have to be
                            // used together.
                            default_scan_fp_16x16_transpose,
                            av1_default_iscan_fp_16x16_transpose);
          } else {
            aom_hadamard_lp_16x16(src_diff, diff_stride, low_coeff);
            av1_quantize_lp(low_coeff, 16 * 16, p->round_fp_QTX,
                            p->quant_fp_QTX, low_qcoeff, low_dqcoeff,
                            p->dequant_QTX, eob,
                            // default_scan_lp_16x16_transpose and
                            // av1_default_iscan_lp_16x16_transpose have to be
                            // used together.
                            default_scan_lp_16x16_transpose,
                            av1_default_iscan_lp_16x16_transpose);
          }
          break;
        case TX_8X8:
          if (use_hbd) {
            aom_hadamard_8x8(src_diff, diff_stride, coeff);
            av1_quantize_fp(
                coeff, 8 * 8, p->zbin_QTX, p->round_fp_QTX, p->quant_fp_QTX,
                p->quant_shift_QTX, qcoeff, dqcoeff, p->dequant_QTX, eob,
                default_scan_8x8_transpose, av1_default_iscan_8x8_transpose);
          } else {
            if (is_tx_8x8_dual_applicable) {
              // The coeffs are pre-computed for the whole block, so re-assign
              // low_coeff to the appropriate location.
              const int block_offset = BLOCK_OFFSET(block + s);
              low_coeff = (int16_t *)p->coeff + block_offset;
            } else {
              aom_hadamard_lp_8x8(src_diff, diff_stride, low_coeff);
            }
            av1_quantize_lp(
                low_coeff, 8 * 8, p->round_fp_QTX, p->quant_fp_QTX, low_qcoeff,
                low_dqcoeff, p->dequant_QTX, eob,
                // default_scan_8x8_transpose and
                // av1_default_iscan_8x8_transpose have to be used together.
                default_scan_8x8_transpose, av1_default_iscan_8x8_transpose);
          }
          break;
        default:
          assert(tx_size == TX_4X4);
          // In tx_size=4x4 case, aom_fdct4x4 and aom_fdct4x4_lp generate
          // normal coefficients order, so we don't need to change the scan
          // order here.
          if (use_hbd) {
            aom_fdct4x4(src_diff, coeff, diff_stride);
            av1_quantize_fp(coeff, 4 * 4, p->zbin_QTX, p->round_fp_QTX,
                            p->quant_fp_QTX, p->quant_shift_QTX, qcoeff,
                            dqcoeff, p->dequant_QTX, eob, scan_order->scan,
                            scan_order->iscan);
          } else {
            aom_fdct4x4_lp(src_diff, low_coeff, diff_stride);
            av1_quantize_lp(low_coeff, 4 * 4, p->round_fp_QTX, p->quant_fp_QTX,
                            low_qcoeff, low_dqcoeff, p->dequant_QTX, eob,
                            scan_order->scan, scan_order->iscan);
          }
          break;
#else
        case TX_16X16:
          aom_hadamard_lp_16x16(src_diff, diff_stride, low_coeff);
          av1_quantize_lp(low_coeff, 16 * 16, p->round_fp_QTX, p->quant_fp_QTX,
                          low_qcoeff, low_dqcoeff, p->dequant_QTX, eob,
                          default_scan_lp_16x16_transpose,
                          av1_default_iscan_lp_16x16_transpose);
          break;
        case TX_8X8:
          if (is_tx_8x8_dual_applicable) {
            // The coeffs are pre-computed for the whole block, so re-assign
            // low_coeff to the appropriate location.
            const int block_offset = BLOCK_OFFSET(block + s);
            low_coeff = (int16_t *)p->coeff + block_offset;
          } else {
            aom_hadamard_lp_8x8(src_diff, diff_stride, low_coeff);
          }
          av1_quantize_lp(low_coeff, 8 * 8, p->round_fp_QTX, p->quant_fp_QTX,
                          low_qcoeff, low_dqcoeff, p->dequant_QTX, eob,
                          default_scan_8x8_transpose,
                          av1_default_iscan_8x8_transpose);
          break;
        default:
          aom_fdct4x4_lp(src_diff, low_coeff, diff_stride);
          av1_quantize_lp(low_coeff, 4 * 4, p->round_fp_QTX, p->quant_fp_QTX,
                          low_qcoeff, low_dqcoeff, p->dequant_QTX, eob,
                          scan_order->scan, scan_order->iscan);
          break;
#endif
      }
      assert(*eob <= 1024);
#if CONFIG_AV1_HIGHBITDEPTH
      if (use_hbd)
        update_yrd_loop_vars_hbd(x, &temp_skippable, step, *eob, coeff, qcoeff,
                                 dqcoeff, this_rdc, &eob_cost,
                                 (r * num_blk_skip_w + c) >> sh_blk_skip);
      else
#endif
        update_yrd_loop_vars(x, &temp_skippable, step, *eob, low_coeff,
                             low_qcoeff, low_dqcoeff, this_rdc, &eob_cost,
                             (r * num_blk_skip_w + c) >> sh_blk_skip);
    }
    block += row_step;
  }

  this_rdc->skip_txfm = *skippable = temp_skippable;
  if (this_rdc->sse < INT64_MAX) {
    this_rdc->sse = (this_rdc->sse << 6) >> 2;
    if (temp_skippable) {
      this_rdc->dist = 0;
      this_rdc->dist = this_rdc->sse;
      return;
    }
  }

  // If skippable is set, rate gets clobbered later.
  this_rdc->rate <<= (2 + AV1_PROB_COST_SHIFT);
  this_rdc->rate += (eob_cost << AV1_PROB_COST_SHIFT);
}

// Explicitly enumerate the cases so the compiler can generate SIMD for the
// function. According to the disassembler, gcc generates SSE codes for each of
// the possible block sizes. The hottest case is tx_width 16, which takes up
// about 8% of the self cycle of av1_nonrd_pick_inter_mode_sb. Since
// av1_nonrd_pick_inter_mode_sb takes up about 3% of total encoding time, the
// potential room of improvement for writing AVX2 optimization is only 3% * 8% =
// 0.24% of total encoding time.
static AOM_INLINE void scale_square_buf_vals(int16_t *dst, int tx_width,
                                             const int16_t *src,
                                             int src_stride) {
#define DO_SCALING                                                   \
  do {                                                               \
    for (int idy = 0; idy < tx_width; ++idy) {                       \
      for (int idx = 0; idx < tx_width; ++idx) {                     \
        dst[idy * tx_width + idx] = src[idy * src_stride + idx] * 8; \
      }                                                              \
    }                                                                \
  } while (0)

  if (tx_width == 4) {
    DO_SCALING;
  } else if (tx_width == 8) {
    DO_SCALING;
  } else if (tx_width == 16) {
    DO_SCALING;
  } else {
    assert(0);
  }

#undef DO_SCALING
}

/*!\brief Calculates RD Cost when the block uses Identity transform.
 * Note that thie function is only for low bit depth encoding, since it
 * is called in real-time mode for now, which sets high bit depth to 0:
 * -DCONFIG_AV1_HIGHBITDEPTH=0
 *
 * \ingroup nonrd_mode_search
 * \callgraph
 * \callergraph
 * Calculates RD Cost. For low bit depth this function
 * uses low-precision set of functions (16-bit) and 32 bit for high bit depth
 * \param[in]    x              Pointer to structure holding all the data for
                                the current macroblock
 * \param[in]    this_rdc       Pointer to calculated RD Cost
 * \param[in]    skippable      Pointer to a flag indicating possible tx skip
 * \param[in]    bsize          Current block size
 * \param[in]    tx_size        Transform size
 *
 * \remark Nothing is returned. Instead, calculated RD cost is placed to
 * \c this_rdc. \c skippable flag is set if all coefficients are zero.
 */
static void block_yrd_idtx(MACROBLOCK *x, RD_STATS *this_rdc, int *skippable,
                           BLOCK_SIZE bsize, TX_SIZE tx_size) {
  MACROBLOCKD *xd = &x->e_mbd;
  const struct macroblockd_plane *pd = &xd->plane[AOM_PLANE_Y];
  struct macroblock_plane *const p = &x->plane[AOM_PLANE_Y];
  assert(bsize < BLOCK_SIZES_ALL);
  const int num_4x4_w = mi_size_wide[bsize];
  const int num_4x4_h = mi_size_high[bsize];
  const int step = 1 << (tx_size << 1);
  const int block_step = (1 << tx_size);
  const int max_blocks_wide =
      num_4x4_w + (xd->mb_to_right_edge >= 0 ? 0 : xd->mb_to_right_edge >> 5);
  const int max_blocks_high =
      num_4x4_h + (xd->mb_to_bottom_edge >= 0 ? 0 : xd->mb_to_bottom_edge >> 5);
  int eob_cost = 0;
  const int bw = 4 * num_4x4_w;
  const int bh = 4 * num_4x4_h;
  const int num_blk_skip_w = num_4x4_w >> 1;
  const int sh_blk_skip = 1;
  // Keep the intermediate value on the stack here. Writing directly to
  // skippable causes speed regression due to load-and-store issues in
  // update_yrd_loop_vars.
  int temp_skippable = 1;
  int tx_wd = 0;
  const SCAN_ORDER *scan_order = NULL;
  switch (tx_size) {
    case TX_64X64:
      assert(0);  // Not implemented
      break;
    case TX_32X32:
      assert(0);  // Not used
      break;
    case TX_16X16:
      scan_order = &av1_fast_idtx_scan_order_16x16;
      tx_wd = 16;
      break;
    case TX_8X8:
      scan_order = &av1_fast_idtx_scan_order_8x8;
      tx_wd = 8;
      break;
    default:
      assert(tx_size == TX_4X4);
      scan_order = &av1_fast_idtx_scan_order_4x4;
      tx_wd = 4;
      break;
  }
  assert(scan_order != NULL);

  this_rdc->dist = 0;
  this_rdc->rate = 0;
  aom_subtract_block(bh, bw, p->src_diff, bw, p->src.buf, p->src.stride,
                     pd->dst.buf, pd->dst.stride);
  // Keep track of the row and column of the blocks we use so that we know
  // if we are in the unrestricted motion border.
  DECLARE_BLOCK_YRD_BUFFERS()
  DECLARE_BLOCK_YRD_VARS()
  for (int r = 0; r < max_blocks_high; r += block_step) {
    for (int c = 0, s = 0; c < max_blocks_wide; c += block_step, s += step) {
      DECLARE_LOOP_VARS_BLOCK_YRD()
      scale_square_buf_vals(low_coeff, tx_wd, src_diff, diff_stride);
      av1_quantize_lp(low_coeff, tx_wd * tx_wd, p->round_fp_QTX,
                      p->quant_fp_QTX, low_qcoeff, low_dqcoeff, p->dequant_QTX,
                      eob, scan_order->scan, scan_order->iscan);
      assert(*eob <= 1024);
      update_yrd_loop_vars(x, &temp_skippable, step, *eob, low_coeff,
                           low_qcoeff, low_dqcoeff, this_rdc, &eob_cost,
                           (r * num_blk_skip_w + c) >> sh_blk_skip);
    }
  }
  this_rdc->skip_txfm = *skippable = temp_skippable;
  if (this_rdc->sse < INT64_MAX) {
    this_rdc->sse = (this_rdc->sse << 6) >> 2;
    if (temp_skippable) {
      this_rdc->dist = 0;
      this_rdc->dist = this_rdc->sse;
      return;
    }
  }
  // If skippable is set, rate gets clobbered later.
  this_rdc->rate <<= (2 + AV1_PROB_COST_SHIFT);
  this_rdc->rate += (eob_cost << AV1_PROB_COST_SHIFT);
}

static INLINE void init_mbmi(MB_MODE_INFO *mbmi, PREDICTION_MODE pred_mode,
                             MV_REFERENCE_FRAME ref_frame0,
                             MV_REFERENCE_FRAME ref_frame1,
                             const AV1_COMMON *cm) {
  PALETTE_MODE_INFO *const pmi = &mbmi->palette_mode_info;
  mbmi->ref_mv_idx = 0;
  mbmi->mode = pred_mode;
  mbmi->uv_mode = UV_DC_PRED;
  mbmi->ref_frame[0] = ref_frame0;
  mbmi->ref_frame[1] = ref_frame1;
  pmi->palette_size[PLANE_TYPE_Y] = 0;
  pmi->palette_size[PLANE_TYPE_UV] = 0;
  mbmi->filter_intra_mode_info.use_filter_intra = 0;
  mbmi->mv[0].as_int = mbmi->mv[1].as_int = 0;
  mbmi->motion_mode = SIMPLE_TRANSLATION;
  mbmi->num_proj_ref = 1;
  mbmi->interintra_mode = 0;
  set_default_interp_filters(mbmi, cm->features.interp_filter);
}

#if CONFIG_INTERNAL_STATS
static void store_coding_context(MACROBLOCK *x, PICK_MODE_CONTEXT *ctx,
                                 int mode_index) {
#else
static void store_coding_context(MACROBLOCK *x, PICK_MODE_CONTEXT *ctx) {
#endif  // CONFIG_INTERNAL_STATS
  MACROBLOCKD *const xd = &x->e_mbd;
  TxfmSearchInfo *txfm_info = &x->txfm_search_info;

  // Take a snapshot of the coding context so it can be
  // restored if we decide to encode this way
  ctx->rd_stats.skip_txfm = txfm_info->skip_txfm;

  ctx->skippable = txfm_info->skip_txfm;
#if CONFIG_INTERNAL_STATS
  ctx->best_mode_index = mode_index;
#endif  // CONFIG_INTERNAL_STATS
  ctx->mic = *xd->mi[0];
  ctx->skippable = txfm_info->skip_txfm;
  av1_copy_mbmi_ext_to_mbmi_ext_frame(&ctx->mbmi_ext_best, &x->mbmi_ext,
                                      av1_ref_frame_type(xd->mi[0]->ref_frame));
}

static int get_pred_buffer(PRED_BUFFER *p, int len) {
  for (int buf_idx = 0; buf_idx < len; buf_idx++) {
    if (!p[buf_idx].in_use) {
      p[buf_idx].in_use = 1;
      return buf_idx;
    }
  }
  return -1;
}

static void free_pred_buffer(PRED_BUFFER *p) {
  if (p != NULL) p->in_use = 0;
}

static INLINE int get_drl_cost(PREDICTION_MODE this_mode, int ref_mv_idx,
                               const MB_MODE_INFO_EXT *mbmi_ext,
                               const int (*const drl_mode_cost0)[2],
                               int8_t ref_frame_type) {
  int cost = 0;
  if (this_mode == NEWMV || this_mode == NEW_NEWMV) {
    for (int idx = 0; idx < 2; ++idx) {
      if (mbmi_ext->ref_mv_count[ref_frame_type] > idx + 1) {
        uint8_t drl_ctx = av1_drl_ctx(mbmi_ext->weight[ref_frame_type], idx);
        cost += drl_mode_cost0[drl_ctx][ref_mv_idx != idx];
        if (ref_mv_idx == idx) return cost;
      }
    }
    return cost;
  }

  if (have_nearmv_in_inter_mode(this_mode)) {
    for (int idx = 1; idx < 3; ++idx) {
      if (mbmi_ext->ref_mv_count[ref_frame_type] > idx + 1) {
        uint8_t drl_ctx = av1_drl_ctx(mbmi_ext->weight[ref_frame_type], idx);
        cost += drl_mode_cost0[drl_ctx][ref_mv_idx != (idx - 1)];
        if (ref_mv_idx == (idx - 1)) return cost;
      }
    }
    return cost;
  }
  return cost;
}

static int cost_mv_ref(const ModeCosts *const mode_costs, PREDICTION_MODE mode,
                       int16_t mode_context) {
  if (is_inter_compound_mode(mode)) {
    return mode_costs
        ->inter_compound_mode_cost[mode_context][INTER_COMPOUND_OFFSET(mode)];
  }

  int mode_cost = 0;
  int16_t mode_ctx = mode_context & NEWMV_CTX_MASK;

  assert(is_inter_mode(mode));

  if (mode == NEWMV) {
    mode_cost = mode_costs->newmv_mode_cost[mode_ctx][0];
    return mode_cost;
  } else {
    mode_cost = mode_costs->newmv_mode_cost[mode_ctx][1];
    mode_ctx = (mode_context >> GLOBALMV_OFFSET) & GLOBALMV_CTX_MASK;

    if (mode == GLOBALMV) {
      mode_cost += mode_costs->zeromv_mode_cost[mode_ctx][0];
      return mode_cost;
    } else {
      mode_cost += mode_costs->zeromv_mode_cost[mode_ctx][1];
      mode_ctx = (mode_context >> REFMV_OFFSET) & REFMV_CTX_MASK;
      mode_cost += mode_costs->refmv_mode_cost[mode_ctx][mode != NEARESTMV];
      return mode_cost;
    }
  }
}

static void newmv_diff_bias(MACROBLOCKD *xd, PREDICTION_MODE this_mode,
                            RD_STATS *this_rdc, BLOCK_SIZE bsize, int mv_row,
                            int mv_col, int speed, uint32_t spatial_variance,
                            CONTENT_STATE_SB content_state_sb) {
  // Bias against MVs associated with NEWMV mode that are very different from
  // top/left neighbors.
  if (this_mode == NEWMV) {
    int al_mv_average_row;
    int al_mv_average_col;
    int row_diff, col_diff;
    int above_mv_valid = 0;
    int left_mv_valid = 0;
    int above_row = INVALID_MV_ROW_COL, above_col = INVALID_MV_ROW_COL;
    int left_row = INVALID_MV_ROW_COL, left_col = INVALID_MV_ROW_COL;
    if (bsize >= BLOCK_64X64 && content_state_sb.source_sad_nonrd != kHighSad &&
        spatial_variance < 300 &&
        (mv_row > 16 || mv_row < -16 || mv_col > 16 || mv_col < -16)) {
      this_rdc->rdcost = this_rdc->rdcost << 2;
      return;
    }
    if (xd->above_mbmi) {
      above_mv_valid = xd->above_mbmi->mv[0].as_int != INVALID_MV;
      above_row = xd->above_mbmi->mv[0].as_mv.row;
      above_col = xd->above_mbmi->mv[0].as_mv.col;
    }
    if (xd->left_mbmi) {
      left_mv_valid = xd->left_mbmi->mv[0].as_int != INVALID_MV;
      left_row = xd->left_mbmi->mv[0].as_mv.row;
      left_col = xd->left_mbmi->mv[0].as_mv.col;
    }
    if (above_mv_valid && left_mv_valid) {
      al_mv_average_row = (above_row + left_row + 1) >> 1;
      al_mv_average_col = (above_col + left_col + 1) >> 1;
    } else if (above_mv_valid) {
      al_mv_average_row = above_row;
      al_mv_average_col = above_col;
    } else if (left_mv_valid) {
      al_mv_average_row = left_row;
      al_mv_average_col = left_col;
    } else {
      al_mv_average_row = al_mv_average_col = 0;
    }
    row_diff = al_mv_average_row - mv_row;
    col_diff = al_mv_average_col - mv_col;
    if (row_diff > 80 || row_diff < -80 || col_diff > 80 || col_diff < -80) {
      if (bsize >= BLOCK_32X32)
        this_rdc->rdcost = this_rdc->rdcost << 1;
      else
        this_rdc->rdcost = 5 * this_rdc->rdcost >> 2;
    }
  } else {
    // Bias for speed >= 8 for low spatial variance.
    if (speed >= 8 && spatial_variance < 150 &&
        (mv_row > 64 || mv_row < -64 || mv_col > 64 || mv_col < -64))
      this_rdc->rdcost = 5 * this_rdc->rdcost >> 2;
  }
}

static int64_t model_rd_for_sb_uv(AV1_COMP *cpi, BLOCK_SIZE plane_bsize,
                                  MACROBLOCK *x, MACROBLOCKD *xd,
                                  RD_STATS *this_rdc, int start_plane,
                                  int stop_plane) {
  // Note our transform coeffs are 8 times an orthogonal transform.
  // Hence quantizer step is also 8 times. To get effective quantizer
  // we need to divide by 8 before sending to modeling function.
  unsigned int sse;
  int rate;
  int64_t dist;
  int plane;
  int64_t tot_sse = 0;

  this_rdc->rate = 0;
  this_rdc->dist = 0;
  this_rdc->skip_txfm = 0;

  for (plane = start_plane; plane <= stop_plane; ++plane) {
    struct macroblock_plane *const p = &x->plane[plane];
    struct macroblockd_plane *const pd = &xd->plane[plane];
    const uint32_t dc_quant = p->dequant_QTX[0];
    const uint32_t ac_quant = p->dequant_QTX[1];
    const BLOCK_SIZE bs = plane_bsize;
    unsigned int var;
    if (!x->color_sensitivity[COLOR_SENS_IDX(plane)]) continue;

    var = cpi->ppi->fn_ptr[bs].vf(p->src.buf, p->src.stride, pd->dst.buf,
                                  pd->dst.stride, &sse);
    assert(sse >= var);
    tot_sse += sse;

    av1_model_rd_from_var_lapndz(sse - var, num_pels_log2_lookup[bs],
                                 dc_quant >> 3, &rate, &dist);

    this_rdc->rate += rate >> 1;
    this_rdc->dist += dist << 3;

    av1_model_rd_from_var_lapndz(var, num_pels_log2_lookup[bs], ac_quant >> 3,
                                 &rate, &dist);

    this_rdc->rate += rate;
    this_rdc->dist += dist << 4;
  }

  if (this_rdc->rate == 0) {
    this_rdc->skip_txfm = 1;
  }

  if (RDCOST(x->rdmult, this_rdc->rate, this_rdc->dist) >=
      RDCOST(x->rdmult, 0, tot_sse << 4)) {
    this_rdc->rate = 0;
    this_rdc->dist = tot_sse << 4;
    this_rdc->skip_txfm = 1;
  }

  return tot_sse;
}

/*!\cond */
struct estimate_block_intra_args {
  AV1_COMP *cpi;
  MACROBLOCK *x;
  PREDICTION_MODE mode;
  int skippable;
  RD_STATS *rdc;
};
/*!\endcond */

/*!\brief Estimation of RD cost of an intra mode for Non-RD optimized case.
 *
 * \ingroup nonrd_mode_search
 * \callgraph
 * \callergraph
 * Calculates RD Cost for an intra mode for a single TX block using Hadamard
 * transform.
 * \param[in]    plane          Color plane
 * \param[in]    block          Index of a TX block in a prediction block
 * \param[in]    row            Row of a current TX block
 * \param[in]    col            Column of a current TX block
 * \param[in]    plane_bsize    Block size of a current prediction block
 * \param[in]    tx_size        Transform size
 * \param[in]    arg            Pointer to a structure that holds parameters
 *                              for intra mode search
 *
 * \remark Nothing is returned. Instead, best mode and RD Cost of the best mode
 * are set in \c args->rdc and \c args->mode
 */
static void estimate_block_intra(int plane, int block, int row, int col,
                                 BLOCK_SIZE plane_bsize, TX_SIZE tx_size,
                                 void *arg) {
  struct estimate_block_intra_args *const args = arg;
  AV1_COMP *const cpi = args->cpi;
  AV1_COMMON *const cm = &cpi->common;
  MACROBLOCK *const x = args->x;
  MACROBLOCKD *const xd = &x->e_mbd;
  struct macroblock_plane *const p = &x->plane[plane];
  struct macroblockd_plane *const pd = &xd->plane[plane];
  const BLOCK_SIZE bsize_tx = txsize_to_bsize[tx_size];
  uint8_t *const src_buf_base = p->src.buf;
  uint8_t *const dst_buf_base = pd->dst.buf;
  const int64_t src_stride = p->src.stride;
  const int64_t dst_stride = pd->dst.stride;
  RD_STATS this_rdc;

  (void)block;
  (void)plane_bsize;

  av1_predict_intra_block_facade(cm, xd, plane, col, row, tx_size);
  av1_invalid_rd_stats(&this_rdc);

  p->src.buf = &src_buf_base[4 * (row * src_stride + col)];
  pd->dst.buf = &dst_buf_base[4 * (row * dst_stride + col)];

  if (plane == 0) {
    block_yrd(x, &this_rdc, &args->skippable, bsize_tx,
              AOMMIN(tx_size, TX_16X16), 0);
  } else {
    model_rd_for_sb_uv(cpi, bsize_tx, x, xd, &this_rdc, plane, plane);
  }

  p->src.buf = src_buf_base;
  pd->dst.buf = dst_buf_base;
  args->rdc->rate += this_rdc.rate;
  args->rdc->dist += this_rdc.dist;
}

static INLINE void update_thresh_freq_fact(AV1_COMP *cpi, MACROBLOCK *x,
                                           BLOCK_SIZE bsize,
                                           MV_REFERENCE_FRAME ref_frame,
                                           THR_MODES best_mode_idx,
                                           PREDICTION_MODE mode) {
  const THR_MODES thr_mode_idx = mode_idx[ref_frame][mode_offset(mode)];
  const BLOCK_SIZE min_size = AOMMAX(bsize - 3, BLOCK_4X4);
  const BLOCK_SIZE max_size = AOMMIN(bsize + 6, BLOCK_128X128);
  for (BLOCK_SIZE bs = min_size; bs <= max_size; bs += 3) {
    int *freq_fact = &x->thresh_freq_fact[bs][thr_mode_idx];
    if (thr_mode_idx == best_mode_idx) {
      *freq_fact -= (*freq_fact >> 4);
    } else {
      *freq_fact =
          AOMMIN(*freq_fact + RD_THRESH_INC,
                 cpi->sf.inter_sf.adaptive_rd_thresh * RD_THRESH_MAX_FACT);
    }
  }
}

#if CONFIG_AV1_TEMPORAL_DENOISING
static void av1_pickmode_ctx_den_update(
    AV1_PICKMODE_CTX_DEN *ctx_den, int64_t zero_last_cost_orig,
    unsigned int ref_frame_cost[REF_FRAMES],
    int_mv frame_mv[MB_MODE_COUNT][REF_FRAMES], int reuse_inter_pred,
    BEST_PICKMODE *bp) {
  ctx_den->zero_last_cost_orig = zero_last_cost_orig;
  ctx_den->ref_frame_cost = ref_frame_cost;
  ctx_den->frame_mv = frame_mv;
  ctx_den->reuse_inter_pred = reuse_inter_pred;
  ctx_den->best_tx_size = bp->best_tx_size;
  ctx_den->best_mode = bp->best_mode;
  ctx_den->best_ref_frame = bp->best_ref_frame;
  ctx_den->best_pred_filter = bp->best_pred_filter;
  ctx_den->best_mode_skip_txfm = bp->best_mode_skip_txfm;
}

static void recheck_zeromv_after_denoising(
    AV1_COMP *cpi, MB_MODE_INFO *const mi, MACROBLOCK *x, MACROBLOCKD *const xd,
    AV1_DENOISER_DECISION decision, AV1_PICKMODE_CTX_DEN *ctx_den,
    struct buf_2d yv12_mb[4][MAX_MB_PLANE], RD_STATS *best_rdc,
    BEST_PICKMODE *best_pickmode, BLOCK_SIZE bsize, int mi_row, int mi_col) {
  // If INTRA or GOLDEN reference was selected, re-evaluate ZEROMV on
  // denoised result. Only do this under noise conditions, and if rdcost of
  // ZEROMV on original source is not significantly higher than rdcost of best
  // mode.
  if (cpi->noise_estimate.enabled && cpi->noise_estimate.level > kLow &&
      ctx_den->zero_last_cost_orig < (best_rdc->rdcost << 3) &&
      ((ctx_den->best_ref_frame == INTRA_FRAME && decision >= FILTER_BLOCK) ||
       (ctx_den->best_ref_frame == GOLDEN_FRAME &&
        cpi->svc.number_spatial_layers == 1 &&
        decision == FILTER_ZEROMV_BLOCK))) {
    // Check if we should pick ZEROMV on denoised signal.
    AV1_COMMON *const cm = &cpi->common;
    RD_STATS this_rdc;
    const ModeCosts *mode_costs = &x->mode_costs;
    TxfmSearchInfo *txfm_info = &x->txfm_search_info;
    MB_MODE_INFO_EXT *const mbmi_ext = &x->mbmi_ext;

    mi->mode = GLOBALMV;
    mi->ref_frame[0] = LAST_FRAME;
    mi->ref_frame[1] = NONE_FRAME;
    set_ref_ptrs(cm, xd, mi->ref_frame[0], NONE_FRAME);
    mi->mv[0].as_int = 0;
    mi->interp_filters = av1_broadcast_interp_filter(EIGHTTAP_REGULAR);
    xd->plane[AOM_PLANE_Y].pre[0] = yv12_mb[LAST_FRAME][AOM_PLANE_Y];
    av1_enc_build_inter_predictor_y(xd, mi_row, mi_col);
    unsigned int var;
    model_rd_for_sb_y(cpi, bsize, x, xd, &this_rdc, &var, 1, NULL);

    const int16_t mode_ctx =
        av1_mode_context_analyzer(mbmi_ext->mode_context, mi->ref_frame);
    this_rdc.rate += cost_mv_ref(mode_costs, GLOBALMV, mode_ctx);

    this_rdc.rate += ctx_den->ref_frame_cost[LAST_FRAME];
    this_rdc.rdcost = RDCOST(x->rdmult, this_rdc.rate, this_rdc.dist);
    txfm_info->skip_txfm = this_rdc.skip_txfm;
    // Don't switch to ZEROMV if the rdcost for ZEROMV on denoised source
    // is higher than best_ref mode (on original source).
    if (this_rdc.rdcost > best_rdc->rdcost) {
      this_rdc = *best_rdc;
      mi->mode = best_pickmode->best_mode;
      mi->ref_frame[0] = best_pickmode->best_ref_frame;
      set_ref_ptrs(cm, xd, mi->ref_frame[0], NONE_FRAME);
      mi->interp_filters = best_pickmode->best_pred_filter;
      if (best_pickmode->best_ref_frame == INTRA_FRAME) {
        mi->mv[0].as_int = INVALID_MV;
      } else {
        mi->mv[0].as_int = ctx_den
                               ->frame_mv[best_pickmode->best_mode]
                                         [best_pickmode->best_ref_frame]
                               .as_int;
        if (ctx_den->reuse_inter_pred) {
          xd->plane[AOM_PLANE_Y].pre[0] = yv12_mb[GOLDEN_FRAME][AOM_PLANE_Y];
          av1_enc_build_inter_predictor_y(xd, mi_row, mi_col);
        }
      }
      mi->tx_size = best_pickmode->best_tx_size;
      txfm_info->skip_txfm = best_pickmode->best_mode_skip_txfm;
    } else {
      ctx_den->best_ref_frame = LAST_FRAME;
      *best_rdc = this_rdc;
    }
  }
}
#endif  // CONFIG_AV1_TEMPORAL_DENOISING

#define FILTER_SEARCH_SIZE 2

/*!\brief Searches for the best interpolation filter
 *
 * \ingroup nonrd_mode_search
 * \callgraph
 * \callergraph
 * Iterates through subset of possible interpolation filters (EIGHTTAP_REGULAR,
 * EIGTHTAP_SMOOTH, MULTITAP_SHARP, depending on FILTER_SEARCH_SIZE) and selects
 * the one that gives lowest RD cost. RD cost is calculated using curvfit model.
 * Support for dual filters (different filters in the x & y directions) is
 * allowed if sf.interp_sf.disable_dual_filter = 0.
 *
 * \param[in]    cpi                  Top-level encoder structure
 * \param[in]    x                    Pointer to structure holding all the
 *                                    data for the current macroblock
 * \param[in]    this_rdc             Pointer to calculated RD Cost
 * \param[in]    inter_pred_params_sr Pointer to structure holding parameters of
                                      inter prediction for single reference
 * \param[in]    mi_row               Row index in 4x4 units
 * \param[in]    mi_col               Column index in 4x4 units
 * \param[in]    tmp_buffer           Pointer to a temporary buffer for
 *                                    prediction re-use
 * \param[in]    bsize                Current block size
 * \param[in]    reuse_inter_pred     Flag, indicating prediction re-use
 * \param[out]   this_mode_pred       Pointer to store prediction buffer
 *                                    for prediction re-use
 * \param[out]   this_early_term      Flag, indicating that transform can be
 *                                    skipped
 * \param[out]   var                  The residue variance of the current
 *                                    predictor.
 * \param[in]    use_model_yrd_large  Flag, indicating special logic to handle
 *                                    large blocks
 * \param[in]    best_sse             Best sse so far.
 * \param[in]    is_single_pred       Flag, indicating single mode.
 *
 * \remark Nothing is returned. Instead, calculated RD cost is placed to
 * \c this_rdc and best filter is placed to \c mi->interp_filters. In case
 * \c reuse_inter_pred flag is set, this function also outputs
 * \c this_mode_pred. Also \c this_early_temp is set if transform can be
 * skipped
 */
static void search_filter_ref(AV1_COMP *cpi, MACROBLOCK *x, RD_STATS *this_rdc,
                              InterPredParams *inter_pred_params_sr, int mi_row,
                              int mi_col, PRED_BUFFER *tmp_buffer,
                              BLOCK_SIZE bsize, int reuse_inter_pred,
                              PRED_BUFFER **this_mode_pred,
                              int *this_early_term, unsigned int *var,
                              int use_model_yrd_large, int64_t best_sse,
                              int is_single_pred) {
  AV1_COMMON *const cm = &cpi->common;
  MACROBLOCKD *const xd = &x->e_mbd;
  struct macroblockd_plane *const pd = &xd->plane[AOM_PLANE_Y];
  MB_MODE_INFO *const mi = xd->mi[0];
  const int bw = block_size_wide[bsize];
  int dim_factor =
      (cpi->sf.interp_sf.disable_dual_filter == 0) ? FILTER_SEARCH_SIZE : 1;
  RD_STATS pf_rd_stats[FILTER_SEARCH_SIZE * FILTER_SEARCH_SIZE] = { 0 };
  TX_SIZE pf_tx_size[FILTER_SEARCH_SIZE * FILTER_SEARCH_SIZE] = { 0 };
  PRED_BUFFER *current_pred = *this_mode_pred;
  int best_skip = 0;
  int best_early_term = 0;
  int64_t best_cost = INT64_MAX;
  int best_filter_index = -1;

  SubpelParams subpel_params;
  // Initialize inter prediction params at mode level for single reference
  // mode.
  if (is_single_pred)
    init_inter_mode_params(&mi->mv[0].as_mv, inter_pred_params_sr,
                           &subpel_params, xd->block_ref_scale_factors[0],
                           pd->pre->width, pd->pre->height);
  for (int filter_idx = 0; filter_idx < FILTER_SEARCH_SIZE * FILTER_SEARCH_SIZE;
       ++filter_idx) {
    int64_t cost;
    if (cpi->sf.interp_sf.disable_dual_filter &&
        filters_ref_set[filter_idx].as_filters.x_filter !=
            filters_ref_set[filter_idx].as_filters.y_filter)
      continue;

    mi->interp_filters.as_int = filters_ref_set[filter_idx].as_int;
    if (is_single_pred)
      av1_enc_build_inter_predictor_y_nonrd(xd, inter_pred_params_sr,
                                            &subpel_params);
    else
      av1_enc_build_inter_predictor(cm, xd, mi_row, mi_col, NULL, bsize,
                                    AOM_PLANE_Y, AOM_PLANE_Y);
    unsigned int curr_var = UINT_MAX;
    if (use_model_yrd_large)
      model_skip_for_sb_y_large(cpi, bsize, mi_row, mi_col, x, xd,
                                &pf_rd_stats[filter_idx], this_early_term, 1,
                                best_sse, &curr_var, UINT_MAX);
    else
      model_rd_for_sb_y(cpi, bsize, x, xd, &pf_rd_stats[filter_idx], &curr_var,
                        1, NULL);
    pf_rd_stats[filter_idx].rate += av1_get_switchable_rate(
        x, xd, cm->features.interp_filter, cm->seq_params->enable_dual_filter);
    cost = RDCOST(x->rdmult, pf_rd_stats[filter_idx].rate,
                  pf_rd_stats[filter_idx].dist);
    pf_tx_size[filter_idx] = mi->tx_size;
    if (cost < best_cost) {
      *var = curr_var;
      best_filter_index = filter_idx;
      best_cost = cost;
      best_skip = pf_rd_stats[filter_idx].skip_txfm;
      best_early_term = *this_early_term;
      if (reuse_inter_pred) {
        if (*this_mode_pred != current_pred) {
          free_pred_buffer(*this_mode_pred);
          *this_mode_pred = current_pred;
        }
        current_pred = &tmp_buffer[get_pred_buffer(tmp_buffer, 3)];
        pd->dst.buf = current_pred->data;
        pd->dst.stride = bw;
      }
    }
  }
  assert(best_filter_index >= 0 &&
         best_filter_index < dim_factor * FILTER_SEARCH_SIZE);
  if (reuse_inter_pred && *this_mode_pred != current_pred)
    free_pred_buffer(current_pred);

  mi->interp_filters.as_int = filters_ref_set[best_filter_index].as_int;
  mi->tx_size = pf_tx_size[best_filter_index];
  this_rdc->rate = pf_rd_stats[best_filter_index].rate;
  this_rdc->dist = pf_rd_stats[best_filter_index].dist;
  this_rdc->sse = pf_rd_stats[best_filter_index].sse;
  this_rdc->skip_txfm = (best_skip || best_early_term);
  *this_early_term = best_early_term;
  if (reuse_inter_pred) {
    pd->dst.buf = (*this_mode_pred)->data;
    pd->dst.stride = (*this_mode_pred)->stride;
  } else if (best_filter_index < dim_factor * FILTER_SEARCH_SIZE - 1) {
    if (is_single_pred)
      av1_enc_build_inter_predictor_y_nonrd(xd, inter_pred_params_sr,
                                            &subpel_params);
    else
      av1_enc_build_inter_predictor(cm, xd, mi_row, mi_col, NULL, bsize,
                                    AOM_PLANE_Y, AOM_PLANE_Y);
  }
}
#if !CONFIG_REALTIME_ONLY
#define MOTION_MODE_SEARCH_SIZE 2

static AOM_INLINE int is_warped_mode_allowed(const AV1_COMP *cpi,
                                             MACROBLOCK *const x,
                                             const MB_MODE_INFO *mbmi) {
  const FeatureFlags *const features = &cpi->common.features;
  const MACROBLOCKD *xd = &x->e_mbd;

  if (cpi->sf.inter_sf.extra_prune_warped) return 0;
  if (has_second_ref(mbmi)) return 0;
  MOTION_MODE last_motion_mode_allowed = SIMPLE_TRANSLATION;

  if (features->switchable_motion_mode) {
    // Determine which motion modes to search if more than SIMPLE_TRANSLATION
    // is allowed.
    last_motion_mode_allowed = motion_mode_allowed(
        xd->global_motion, xd, mbmi, features->allow_warped_motion);
  }

  if (last_motion_mode_allowed == WARPED_CAUSAL) {
    return 1;
  }

  return 0;
}

static void calc_num_proj_ref(AV1_COMP *cpi, MACROBLOCK *x, MB_MODE_INFO *mi) {
  AV1_COMMON *const cm = &cpi->common;
  MACROBLOCKD *const xd = &x->e_mbd;
  const FeatureFlags *const features = &cm->features;

  mi->num_proj_ref = 1;
  WARP_SAMPLE_INFO *const warp_sample_info =
      &x->warp_sample_info[mi->ref_frame[0]];
  int *pts0 = warp_sample_info->pts;
  int *pts_inref0 = warp_sample_info->pts_inref;
  MOTION_MODE last_motion_mode_allowed = SIMPLE_TRANSLATION;

  if (features->switchable_motion_mode) {
    // Determine which motion modes to search if more than SIMPLE_TRANSLATION
    // is allowed.
    last_motion_mode_allowed = motion_mode_allowed(
        xd->global_motion, xd, mi, features->allow_warped_motion);
  }

  if (last_motion_mode_allowed == WARPED_CAUSAL) {
    if (warp_sample_info->num < 0) {
      warp_sample_info->num = av1_findSamples(cm, xd, pts0, pts_inref0);
    }
    mi->num_proj_ref = warp_sample_info->num;
  }
}

static void search_motion_mode(AV1_COMP *cpi, MACROBLOCK *x, RD_STATS *this_rdc,
                               int mi_row, int mi_col, BLOCK_SIZE bsize,
                               int *this_early_term, int use_model_yrd_large,
                               int *rate_mv, int64_t best_sse) {
  AV1_COMMON *const cm = &cpi->common;
  MACROBLOCKD *const xd = &x->e_mbd;
  const FeatureFlags *const features = &cm->features;
  MB_MODE_INFO *const mi = xd->mi[0];
  RD_STATS pf_rd_stats[MOTION_MODE_SEARCH_SIZE] = { 0 };
  int best_skip = 0;
  int best_early_term = 0;
  int64_t best_cost = INT64_MAX;
  int best_mode_index = -1;
  const int interp_filter = features->interp_filter;

  const MOTION_MODE motion_modes[MOTION_MODE_SEARCH_SIZE] = {
    SIMPLE_TRANSLATION, WARPED_CAUSAL
  };
  int mode_search_size = is_warped_mode_allowed(cpi, x, mi) ? 2 : 1;

  WARP_SAMPLE_INFO *const warp_sample_info =
      &x->warp_sample_info[mi->ref_frame[0]];
  int *pts0 = warp_sample_info->pts;
  int *pts_inref0 = warp_sample_info->pts_inref;

  const int total_samples = mi->num_proj_ref;
  if (total_samples == 0) {
    // Do not search WARPED_CAUSAL if there are no samples to use to determine
    // warped parameters.
    mode_search_size = 1;
  }

  const MB_MODE_INFO base_mbmi = *mi;
  MB_MODE_INFO best_mbmi;

  for (int mode_index = 0; mode_index < mode_search_size; ++mode_index) {
    int64_t cost = INT64_MAX;
    MOTION_MODE motion_mode = motion_modes[mode_index];
    *mi = base_mbmi;
    mi->motion_mode = motion_mode;
    if (motion_mode == SIMPLE_TRANSLATION) {
      mi->interp_filters = av1_broadcast_interp_filter(EIGHTTAP_REGULAR);

      av1_enc_build_inter_predictor(cm, xd, mi_row, mi_col, NULL, bsize,
                                    AOM_PLANE_Y, AOM_PLANE_Y);
      if (use_model_yrd_large)
        model_skip_for_sb_y_large(cpi, bsize, mi_row, mi_col, x, xd,
                                  &pf_rd_stats[mode_index], this_early_term, 1,
                                  best_sse, NULL, UINT_MAX);
      else
        model_rd_for_sb_y(cpi, bsize, x, xd, &pf_rd_stats[mode_index], NULL, 1,
                          NULL);
      pf_rd_stats[mode_index].rate +=
          av1_get_switchable_rate(x, xd, cm->features.interp_filter,
                                  cm->seq_params->enable_dual_filter);
      cost = RDCOST(x->rdmult, pf_rd_stats[mode_index].rate,
                    pf_rd_stats[mode_index].dist);
    } else if (motion_mode == WARPED_CAUSAL) {
      int pts[SAMPLES_ARRAY_SIZE], pts_inref[SAMPLES_ARRAY_SIZE];
      const ModeCosts *mode_costs = &x->mode_costs;
      mi->wm_params.wmtype = DEFAULT_WMTYPE;
      mi->interp_filters =
          av1_broadcast_interp_filter(av1_unswitchable_filter(interp_filter));

      memcpy(pts, pts0, total_samples * 2 * sizeof(*pts0));
      memcpy(pts_inref, pts_inref0, total_samples * 2 * sizeof(*pts_inref0));
      // Select the samples according to motion vector difference
      if (mi->num_proj_ref > 1) {
        mi->num_proj_ref = av1_selectSamples(&mi->mv[0].as_mv, pts, pts_inref,
                                             mi->num_proj_ref, bsize);
      }

      // Compute the warped motion parameters with a least squares fit
      //  using the collected samples
      if (!av1_find_projection(mi->num_proj_ref, pts, pts_inref, bsize,
                               mi->mv[0].as_mv.row, mi->mv[0].as_mv.col,
                               &mi->wm_params, mi_row, mi_col)) {
        if (mi->mode == NEWMV) {
          const int_mv mv0 = mi->mv[0];
          const WarpedMotionParams wm_params0 = mi->wm_params;
          const int num_proj_ref0 = mi->num_proj_ref;

          const int_mv ref_mv = av1_get_ref_mv(x, 0);
          SUBPEL_MOTION_SEARCH_PARAMS ms_params;
          av1_make_default_subpel_ms_params(&ms_params, cpi, x, bsize,
                                            &ref_mv.as_mv, NULL);

          // Refine MV in a small range.
          av1_refine_warped_mv(xd, cm, &ms_params, bsize, pts0, pts_inref0,
                               total_samples);
          if (mi->mv[0].as_int == ref_mv.as_int) {
            continue;
          }

          if (mv0.as_int != mi->mv[0].as_int) {
            // Keep the refined MV and WM parameters.
            int tmp_rate_mv = av1_mv_bit_cost(
                &mi->mv[0].as_mv, &ref_mv.as_mv, x->mv_costs->nmv_joint_cost,
                x->mv_costs->mv_cost_stack, MV_COST_WEIGHT);
            *rate_mv = tmp_rate_mv;
          } else {
            // Restore the old MV and WM parameters.
            mi->mv[0] = mv0;
            mi->wm_params = wm_params0;
            mi->num_proj_ref = num_proj_ref0;
          }
        }
        // Build the warped predictor
        av1_enc_build_inter_predictor(cm, xd, mi_row, mi_col, NULL, bsize,
                                      AOM_PLANE_Y, av1_num_planes(cm) - 1);
        if (use_model_yrd_large)
          model_skip_for_sb_y_large(cpi, bsize, mi_row, mi_col, x, xd,
                                    &pf_rd_stats[mode_index], this_early_term,
                                    1, best_sse, NULL, UINT_MAX);
        else
          model_rd_for_sb_y(cpi, bsize, x, xd, &pf_rd_stats[mode_index], NULL,
                            1, NULL);

        pf_rd_stats[mode_index].rate +=
            mode_costs->motion_mode_cost[bsize][mi->motion_mode];
        cost = RDCOST(x->rdmult, pf_rd_stats[mode_index].rate,
                      pf_rd_stats[mode_index].dist);
      } else {
        cost = INT64_MAX;
      }
    }
    if (cost < best_cost) {
      best_mode_index = mode_index;
      best_cost = cost;
      best_skip = pf_rd_stats[mode_index].skip_txfm;
      best_early_term = *this_early_term;
      best_mbmi = *mi;
    }
  }
  assert(best_mode_index >= 0 && best_mode_index < FILTER_SEARCH_SIZE);

  *mi = best_mbmi;
  this_rdc->rate = pf_rd_stats[best_mode_index].rate;
  this_rdc->dist = pf_rd_stats[best_mode_index].dist;
  this_rdc->sse = pf_rd_stats[best_mode_index].sse;
  this_rdc->skip_txfm = (best_skip || best_early_term);
  *this_early_term = best_early_term;
  if (best_mode_index < FILTER_SEARCH_SIZE - 1) {
    av1_enc_build_inter_predictor(cm, xd, mi_row, mi_col, NULL, bsize,
                                  AOM_PLANE_Y, AOM_PLANE_Y);
  }
}
#endif  // !CONFIG_REALTIME_ONLY

#define COLLECT_PICK_MODE_STAT 0
#define COLLECT_NON_SQR_STAT 0

#if COLLECT_PICK_MODE_STAT
#include "aom_ports/aom_timer.h"
typedef struct _mode_search_stat {
  int32_t num_blocks[BLOCK_SIZES];
  int64_t total_block_times[BLOCK_SIZES];
  int32_t num_searches[BLOCK_SIZES][MB_MODE_COUNT];
  int32_t num_nonskipped_searches[BLOCK_SIZES][MB_MODE_COUNT];
  int64_t search_times[BLOCK_SIZES][MB_MODE_COUNT];
  int64_t nonskipped_search_times[BLOCK_SIZES][MB_MODE_COUNT];
  int64_t ms_time[BLOCK_SIZES][MB_MODE_COUNT];
  int64_t ifs_time[BLOCK_SIZES][MB_MODE_COUNT];
  int64_t model_rd_time[BLOCK_SIZES][MB_MODE_COUNT];
  int64_t txfm_time[BLOCK_SIZES][MB_MODE_COUNT];
  struct aom_usec_timer timer1;
  struct aom_usec_timer timer2;
  struct aom_usec_timer bsize_timer;
} mode_search_stat;

static mode_search_stat ms_stat;

static AOM_INLINE void print_stage_time(const char *stage_name,
                                        int64_t stage_time,
                                        int64_t total_time) {
  printf("    %s: %ld (%f%%)\n", stage_name, stage_time,
         100 * stage_time / (float)total_time);
}

static void print_time(const mode_search_stat *const ms_stat, BLOCK_SIZE bsize,
                       int mi_rows, int mi_cols, int mi_row, int mi_col) {
  if ((mi_row + mi_size_high[bsize] >= mi_rows) &&
      (mi_col + mi_size_wide[bsize] >= mi_cols)) {
    int64_t total_time = 0l;
    int32_t total_blocks = 0;
    for (BLOCK_SIZE bs = 0; bs < BLOCK_SIZES; bs++) {
      total_time += ms_stat->total_block_times[bs];
      total_blocks += ms_stat->num_blocks[bs];
    }

    printf("\n");
    for (BLOCK_SIZE bs = 0; bs < BLOCK_SIZES; bs++) {
      if (ms_stat->num_blocks[bs] == 0) {
        continue;
      }
      if (!COLLECT_NON_SQR_STAT && block_size_wide[bs] != block_size_high[bs]) {
        continue;
      }

      printf("BLOCK_%dX%d Num %d, Time: %ld (%f%%), Avg_time %f:\n",
             block_size_wide[bs], block_size_high[bs], ms_stat->num_blocks[bs],
             ms_stat->total_block_times[bs],
             100 * ms_stat->total_block_times[bs] / (float)total_time,
             (float)ms_stat->total_block_times[bs] / ms_stat->num_blocks[bs]);
      for (int j = 0; j < MB_MODE_COUNT; j++) {
        if (ms_stat->nonskipped_search_times[bs][j] == 0) {
          continue;
        }

        int64_t total_mode_time = ms_stat->nonskipped_search_times[bs][j];
        printf("  Mode %d, %d/%d tps %f\n", j,
               ms_stat->num_nonskipped_searches[bs][j],
               ms_stat->num_searches[bs][j],
               ms_stat->num_nonskipped_searches[bs][j] > 0
                   ? (float)ms_stat->nonskipped_search_times[bs][j] /
                         ms_stat->num_nonskipped_searches[bs][j]
                   : 0l);
        if (j >= INTER_MODE_START) {
          total_mode_time = ms_stat->ms_time[bs][j] + ms_stat->ifs_time[bs][j] +
                            ms_stat->model_rd_time[bs][j] +
                            ms_stat->txfm_time[bs][j];
          print_stage_time("Motion Search Time", ms_stat->ms_time[bs][j],
                           total_time);
          print_stage_time("Filter Search Time", ms_stat->ifs_time[bs][j],
                           total_time);
          print_stage_time("Model    RD   Time", ms_stat->model_rd_time[bs][j],
                           total_time);
          print_stage_time("Tranfm Search Time", ms_stat->txfm_time[bs][j],
                           total_time);
        }
        print_stage_time("Total  Mode   Time", total_mode_time, total_time);
      }
      printf("\n");
    }
    printf("Total time = %ld. Total blocks = %d\n", total_time, total_blocks);
  }
}
#endif  // COLLECT_PICK_MODE_STAT

static void compute_intra_yprediction(const AV1_COMMON *cm,
                                      PREDICTION_MODE mode, BLOCK_SIZE bsize,
                                      MACROBLOCK *x, MACROBLOCKD *xd) {
  const SequenceHeader *seq_params = cm->seq_params;
  struct macroblockd_plane *const pd = &xd->plane[AOM_PLANE_Y];
  struct macroblock_plane *const p = &x->plane[AOM_PLANE_Y];
  uint8_t *const src_buf_base = p->src.buf;
  uint8_t *const dst_buf_base = pd->dst.buf;
  const int src_stride = p->src.stride;
  const int dst_stride = pd->dst.stride;
  int plane = 0;
  int row, col;
  // block and transform sizes, in number of 4x4 blocks log 2 ("*_b")
  // 4x4=0, 8x8=2, 16x16=4, 32x32=6, 64x64=8
  // transform size varies per plane, look it up in a common way.
  const TX_SIZE tx_size = max_txsize_lookup[bsize];
  const BLOCK_SIZE plane_bsize =
      get_plane_block_size(bsize, pd->subsampling_x, pd->subsampling_y);
  // If mb_to_right_edge is < 0 we are in a situation in which
  // the current block size extends into the UMV and we won't
  // visit the sub blocks that are wholly within the UMV.
  const int max_blocks_wide = max_block_wide(xd, plane_bsize, plane);
  const int max_blocks_high = max_block_high(xd, plane_bsize, plane);
  // Keep track of the row and column of the blocks we use so that we know
  // if we are in the unrestricted motion border.
  for (row = 0; row < max_blocks_high; row += (1 << tx_size)) {
    // Skip visiting the sub blocks that are wholly within the UMV.
    for (col = 0; col < max_blocks_wide; col += (1 << tx_size)) {
      p->src.buf = &src_buf_base[4 * (row * (int64_t)src_stride + col)];
      pd->dst.buf = &dst_buf_base[4 * (row * (int64_t)dst_stride + col)];
      av1_predict_intra_block(
          xd, seq_params->sb_size, seq_params->enable_intra_edge_filter,
          block_size_wide[bsize], block_size_high[bsize], tx_size, mode, 0, 0,
          FILTER_INTRA_MODES, pd->dst.buf, dst_stride, pd->dst.buf, dst_stride,
          0, 0, plane);
    }
  }
  p->src.buf = src_buf_base;
  pd->dst.buf = dst_buf_base;
}

static bool should_prune_intra_modes_using_neighbors(
    const MACROBLOCKD *xd, bool enable_intra_mode_pruning_using_neighbors,
    PREDICTION_MODE this_mode, PREDICTION_MODE above_mode,
    PREDICTION_MODE left_mode) {
  if (!enable_intra_mode_pruning_using_neighbors) return false;

  // Avoid pruning of DC_PRED as it is the most probable mode to win as per the
  // statistics generated for nonrd intra mode evaluations.
  if (this_mode == DC_PRED) return false;

  // Enable the pruning for current mode only if it is not the winner mode of
  // both the neighboring blocks (left/top).
  return xd->up_available && this_mode != above_mode && xd->left_available &&
         this_mode != left_mode;
}

void av1_nonrd_pick_intra_mode(AV1_COMP *cpi, MACROBLOCK *x, RD_STATS *rd_cost,
                               BLOCK_SIZE bsize, PICK_MODE_CONTEXT *ctx) {
  AV1_COMMON *const cm = &cpi->common;
  MACROBLOCKD *const xd = &x->e_mbd;
  MB_MODE_INFO *const mi = xd->mi[0];
  RD_STATS this_rdc, best_rdc;
  struct estimate_block_intra_args args = { cpi, x, DC_PRED, 1, 0 };
  const TxfmSearchParams *txfm_params = &x->txfm_search_params;
  const TX_SIZE intra_tx_size =
      AOMMIN(max_txsize_lookup[bsize],
             tx_mode_to_biggest_tx_size[txfm_params->tx_mode_search_type]);
  int *bmode_costs;
  PREDICTION_MODE best_mode = DC_PRED;
  const MB_MODE_INFO *above_mi = xd->above_mbmi;
  const MB_MODE_INFO *left_mi = xd->left_mbmi;
  const PREDICTION_MODE A = av1_above_block_mode(above_mi);
  const PREDICTION_MODE L = av1_left_block_mode(left_mi);
  const int above_ctx = intra_mode_context[A];
  const int left_ctx = intra_mode_context[L];
  const unsigned int source_variance = x->source_variance;
  bmode_costs = x->mode_costs.y_mode_costs[above_ctx][left_ctx];

  av1_invalid_rd_stats(&best_rdc);
  av1_invalid_rd_stats(&this_rdc);

  init_mbmi(mi, DC_PRED, INTRA_FRAME, NONE_FRAME, cm);
  mi->mv[0].as_int = mi->mv[1].as_int = INVALID_MV;

  // Change the limit of this loop to add other intra prediction
  // mode tests.
  for (int mode_index = 0; mode_index < RTC_INTRA_MODES; ++mode_index) {
    PREDICTION_MODE this_mode = intra_mode_list[mode_index];

    // As per the statistics generated for intra mode evaluation in the nonrd
    // path, it is found that the probability of H_PRED mode being the winner is
    // very low when the best mode so far is V_PRED (out of DC_PRED and V_PRED).
    // If V_PRED is the winner mode out of DC_PRED and V_PRED, it could imply
    // the presence of a vertically dominant pattern. Hence, H_PRED mode is not
    // evaluated.
    if (cpi->sf.rt_sf.prune_h_pred_using_best_mode_so_far &&
        this_mode == H_PRED && best_mode == V_PRED)
      continue;

    if (should_prune_intra_modes_using_neighbors(
            xd, cpi->sf.rt_sf.enable_intra_mode_pruning_using_neighbors,
            this_mode, A, L)) {
      // Prune V_PRED and H_PRED if source variance of the block is less than
      // or equal to 50. The source variance threshold is obtained empirically.
      if ((this_mode == V_PRED || this_mode == H_PRED) && source_variance <= 50)
        continue;

      // As per the statistics, probability of SMOOTH_PRED being the winner is
      // low when best mode so far is DC_PRED (out of DC_PRED, V_PRED and
      // H_PRED). Hence, SMOOTH_PRED mode is not evaluated.
      if (best_mode == DC_PRED && this_mode == SMOOTH_PRED) continue;
    }

    this_rdc.dist = this_rdc.rate = 0;
    args.mode = this_mode;
    args.skippable = 1;
    args.rdc = &this_rdc;
    mi->tx_size = intra_tx_size;
    mi->mode = this_mode;
    av1_foreach_transformed_block_in_plane(xd, bsize, AOM_PLANE_Y,
                                           estimate_block_intra, &args);
    const int skip_ctx = av1_get_skip_txfm_context(xd);
    if (args.skippable) {
      this_rdc.rate = x->mode_costs.skip_txfm_cost[skip_ctx][1];
    } else {
      this_rdc.rate += x->mode_costs.skip_txfm_cost[skip_ctx][0];
    }
    this_rdc.rate += bmode_costs[this_mode];
    this_rdc.rdcost = RDCOST(x->rdmult, this_rdc.rate, this_rdc.dist);

    if (this_rdc.rdcost < best_rdc.rdcost) {
      best_rdc = this_rdc;
      best_mode = this_mode;
      if (!this_rdc.skip_txfm) {
        memset(ctx->blk_skip, 0,
               sizeof(x->txfm_search_info.blk_skip[0]) * ctx->num_4x4_blk);
      }
    }
  }

  mi->mode = best_mode;
  // Keep DC for UV since mode test is based on Y channel only.
  mi->uv_mode = UV_DC_PRED;
  *rd_cost = best_rdc;

#if CONFIG_INTERNAL_STATS
  store_coding_context(x, ctx, mi->mode);
#else
  store_coding_context(x, ctx);
#endif  // CONFIG_INTERNAL_STATS
}

static AOM_INLINE int is_same_gf_and_last_scale(AV1_COMMON *cm) {
  struct scale_factors *const sf_last = get_ref_scale_factors(cm, LAST_FRAME);
  struct scale_factors *const sf_golden =
      get_ref_scale_factors(cm, GOLDEN_FRAME);
  return ((sf_last->x_scale_fp == sf_golden->x_scale_fp) &&
          (sf_last->y_scale_fp == sf_golden->y_scale_fp));
}

static AOM_INLINE void get_ref_frame_use_mask(AV1_COMP *cpi, MACROBLOCK *x,
                                              MB_MODE_INFO *mi, int mi_row,
                                              int mi_col, int bsize,
                                              int gf_temporal_ref,
                                              int use_ref_frame[],
                                              int *force_skip_low_temp_var) {
  AV1_COMMON *const cm = &cpi->common;
  const struct segmentation *const seg = &cm->seg;
  const int is_small_sb = (cm->seq_params->sb_size == BLOCK_64X64);

  // When the ref_frame_config is used to set the reference frame structure
  // then the usage of alt_ref is determined by the ref_frame_flags
  // (and not the speed feature use_nonrd_altref_frame).
  int use_alt_ref_frame = cpi->ppi->rtc_ref.set_ref_frame_config ||
                          cpi->sf.rt_sf.use_nonrd_altref_frame;

  int use_golden_ref_frame = 1;
  int use_last_ref_frame = 1;

  // When the ref_frame_config is used to set the reference frame structure:
  // check if LAST is used as a reference. And only remove golden and altref
  // references below if last is used as a reference.
  if (cpi->ppi->rtc_ref.set_ref_frame_config)
    use_last_ref_frame =
        cpi->ref_frame_flags & AOM_LAST_FLAG ? use_last_ref_frame : 0;

  // frame_since_golden is not used when user sets the referene structure.
  if (!cpi->ppi->rtc_ref.set_ref_frame_config && use_last_ref_frame &&
      cpi->rc.frames_since_golden == 0 && gf_temporal_ref) {
    use_golden_ref_frame = 0;
  }

  if (use_last_ref_frame && cpi->sf.rt_sf.short_circuit_low_temp_var &&
      x->nonrd_prune_ref_frame_search) {
    if (is_small_sb)
      *force_skip_low_temp_var = av1_get_force_skip_low_temp_var_small_sb(
          &x->part_search_info.variance_low[0], mi_row, mi_col, bsize);
    else
      *force_skip_low_temp_var = av1_get_force_skip_low_temp_var(
          &x->part_search_info.variance_low[0], mi_row, mi_col, bsize);
    // If force_skip_low_temp_var is set, skip golden reference.
    if (*force_skip_low_temp_var) {
      use_golden_ref_frame = 0;
      use_alt_ref_frame = 0;
    }
  }

  if (use_last_ref_frame &&
      (x->nonrd_prune_ref_frame_search > 2 || x->force_zeromv_skip_for_blk ||
       (x->nonrd_prune_ref_frame_search > 1 && bsize > BLOCK_64X64))) {
    use_golden_ref_frame = 0;
    use_alt_ref_frame = 0;
  }

  if (segfeature_active(seg, mi->segment_id, SEG_LVL_REF_FRAME) &&
      get_segdata(seg, mi->segment_id, SEG_LVL_REF_FRAME) == GOLDEN_FRAME) {
    use_golden_ref_frame = 1;
    use_alt_ref_frame = 0;
  }

  // Skip golden reference if color is set, on flat blocks with motion.
  // For screen: always skip golden (if color_sensitivity_sb_g is set)
  // except when x->nonrd_prune_ref_frame_search = 0. This latter flag
  // may be set in the variance partition when golden is a much better
  // reference than last, in which case it may not be worth skipping
  // golden completely.
  if (((cpi->oxcf.tune_cfg.content == AOM_CONTENT_SCREEN &&
        x->nonrd_prune_ref_frame_search != 0) ||
       (x->source_variance < 500 &&
        x->content_state_sb.source_sad_nonrd > kLowSad)) &&
      (x->color_sensitivity_sb_g[COLOR_SENS_IDX(AOM_PLANE_U)] == 1 ||
       x->color_sensitivity_sb_g[COLOR_SENS_IDX(AOM_PLANE_V)] == 1))
    use_golden_ref_frame = 0;

  // For non-screen: if golden and altref are not being selected as references
  // (use_golden_ref_frame/use_alt_ref_frame = 0) check to allow golden back
  // based on the sad of nearest/nearmv of LAST ref. If this block sad is large,
  // keep golden as reference. Only do this for the agrressive pruning mode and
  // avoid it when color is set for golden reference.
  if (cpi->oxcf.tune_cfg.content != AOM_CONTENT_SCREEN &&
      (cpi->ref_frame_flags & AOM_LAST_FLAG) && !use_golden_ref_frame &&
      !use_alt_ref_frame && x->pred_mv_sad[LAST_FRAME] != INT_MAX &&
      x->nonrd_prune_ref_frame_search > 2 &&
      x->color_sensitivity_sb_g[COLOR_SENS_IDX(AOM_PLANE_U)] == 0 &&
      x->color_sensitivity_sb_g[COLOR_SENS_IDX(AOM_PLANE_V)] == 0) {
    int thr = (cm->width * cm->height >= 640 * 360) ? 100 : 150;
    int pred = x->pred_mv_sad[LAST_FRAME] >>
               (b_width_log2_lookup[bsize] + b_height_log2_lookup[bsize]);
    if (pred > thr) use_golden_ref_frame = 1;
  }

  use_alt_ref_frame =
      cpi->ref_frame_flags & AOM_ALT_FLAG ? use_alt_ref_frame : 0;
  use_golden_ref_frame =
      cpi->ref_frame_flags & AOM_GOLD_FLAG ? use_golden_ref_frame : 0;

  // For spatial layers: enable golden ref if it is set by user and
  // corresponds to the lower spatial layer.
  if (cpi->svc.spatial_layer_id > 0 && (cpi->ref_frame_flags & AOM_GOLD_FLAG) &&
      x->content_state_sb.source_sad_nonrd < kHighSad) {
    const int buffslot_golden =
        cpi->ppi->rtc_ref.ref_idx[GOLDEN_FRAME - LAST_FRAME];
    if (cpi->svc.buffer_time_index[buffslot_golden] ==
        cpi->svc.current_superframe)
      use_golden_ref_frame = 1;
  }

  use_ref_frame[ALTREF_FRAME] = use_alt_ref_frame;
  use_ref_frame[GOLDEN_FRAME] = use_golden_ref_frame;
  use_ref_frame[LAST_FRAME] = use_last_ref_frame;
  // For now keep this assert on, but we should remove it for svc mode,
  // as the user may want to generate an intra-only frame (no inter-modes).
  // Remove this assert in subsequent CL when nonrd_pickmode is tested for the
  // case of intra-only frame (no references enabled).
  assert(use_last_ref_frame || use_golden_ref_frame || use_alt_ref_frame);
}

// Checks whether Intra mode needs to be pruned based on
// 'intra_y_mode_bsize_mask_nrd' and 'prune_hv_pred_modes_using_blksad'
// speed features.
static INLINE bool is_prune_intra_mode(
    AV1_COMP *cpi, int mode_index, int force_intra_check, BLOCK_SIZE bsize,
    uint8_t segment_id, SOURCE_SAD source_sad_nonrd,
    uint8_t color_sensitivity[MAX_MB_PLANE - 1]) {
  const PREDICTION_MODE this_mode = intra_mode_list[mode_index];
  if (mode_index > 2 || force_intra_check == 0) {
    if (!((1 << this_mode) & cpi->sf.rt_sf.intra_y_mode_bsize_mask_nrd[bsize]))
      return true;

    if (this_mode == DC_PRED) return false;

    if (!cpi->sf.rt_sf.prune_hv_pred_modes_using_src_sad) return false;

    const bool has_color_sensitivity =
        color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_U)] &&
        color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_V)];
    if (has_color_sensitivity &&
        (cpi->rc.frame_source_sad > 1.1 * cpi->rc.avg_source_sad ||
         cyclic_refresh_segment_id_boosted(segment_id) ||
         source_sad_nonrd > kMedSad))
      return false;

    return true;
  }
  return false;
}

/*!\brief Estimates best intra mode for inter mode search
 *
 * \ingroup nonrd_mode_search
 * \callgraph
 * \callergraph
 *
 * Using heuristics based on best inter mode, block size, and other decides
 * whether to check intra modes. If so, estimates and selects best intra mode
 * from the reduced set of intra modes (max 4 intra modes checked)
 *
 * \param[in]    cpi                      Top-level encoder structure
 * \param[in]    x                        Pointer to structure holding all the
 *                                        data for the current macroblock
 * \param[in]    bsize                    Current block size
 * \param[in]    best_early_term          Flag, indicating that TX for the
 *                                        best inter mode was skipped
 * \param[in]    ref_cost_intra           Cost of signalling intra mode
 * \param[in]    reuse_prediction         Flag, indicating prediction re-use
 * \param[in]    orig_dst                 Original destination buffer
 * \param[in]    tmp_buffers              Pointer to a temporary buffers for
 *                                        prediction re-use
 * \param[out]   this_mode_pred           Pointer to store prediction buffer
 *                                        for prediction re-use
 * \param[in]    best_rdc                 Pointer to RD cost for the best
 *                                        selected intra mode
 * \param[in]    best_pickmode            Pointer to a structure containing
 *                                        best mode picked so far
 * \param[in]    ctx                      Pointer to structure holding coding
 *                                        contexts and modes for the block
 *
 * \remark Nothing is returned. Instead, calculated RD cost is placed to
 * \c best_rdc and best selected mode is placed to \c best_pickmode
 */
static void estimate_intra_mode(
    AV1_COMP *cpi, MACROBLOCK *x, BLOCK_SIZE bsize, int best_early_term,
    unsigned int ref_cost_intra, int reuse_prediction, struct buf_2d *orig_dst,
    PRED_BUFFER *tmp_buffers, PRED_BUFFER **this_mode_pred, RD_STATS *best_rdc,
    BEST_PICKMODE *best_pickmode, PICK_MODE_CONTEXT *ctx) {
  AV1_COMMON *const cm = &cpi->common;
  MACROBLOCKD *const xd = &x->e_mbd;
  MB_MODE_INFO *const mi = xd->mi[0];
  const TxfmSearchParams *txfm_params = &x->txfm_search_params;
  const unsigned char segment_id = mi->segment_id;
  const int *const rd_threshes = cpi->rd.threshes[segment_id][bsize];
  const int *const rd_thresh_freq_fact = x->thresh_freq_fact[bsize];
  const bool is_screen_content =
      cpi->oxcf.tune_cfg.content == AOM_CONTENT_SCREEN;
  struct macroblockd_plane *const pd = &xd->plane[AOM_PLANE_Y];
  const REAL_TIME_SPEED_FEATURES *const rt_sf = &cpi->sf.rt_sf;

  const CommonQuantParams *quant_params = &cm->quant_params;

  RD_STATS this_rdc;

  int intra_cost_penalty = av1_get_intra_cost_penalty(
      quant_params->base_qindex, quant_params->y_dc_delta_q,
      cm->seq_params->bit_depth);
  int64_t inter_mode_thresh =
      RDCOST(x->rdmult, ref_cost_intra + intra_cost_penalty, 0);
  int perform_intra_pred = rt_sf->check_intra_pred_nonrd;
  int force_intra_check = 0;
  // For spatial enhancement layer: turn off intra prediction if the
  // previous spatial layer as golden ref is not chosen as best reference.
  // only do this for temporal enhancement layer and on non-key frames.
  if (cpi->svc.spatial_layer_id > 0 &&
      best_pickmode->best_ref_frame != GOLDEN_FRAME &&
      cpi->svc.temporal_layer_id > 0 &&
      !cpi->svc.layer_context[cpi->svc.temporal_layer_id].is_key_frame)
    perform_intra_pred = 0;

  int do_early_exit_rdthresh = 1;

  uint32_t spatial_var_thresh = 50;
  int motion_thresh = 32;
  // Adjust thresholds to make intra mode likely tested if the other
  // references (golden, alt) are skipped/not checked. For now always
  // adjust for svc mode.
  if (cpi->ppi->use_svc || (rt_sf->use_nonrd_altref_frame == 0 &&
                            rt_sf->nonrd_prune_ref_frame_search > 0)) {
    spatial_var_thresh = 150;
    motion_thresh = 0;
  }

  // Some adjustments to checking intra mode based on source variance.
  if (x->source_variance < spatial_var_thresh) {
    // If the best inter mode is large motion or non-LAST ref reduce intra cost
    // penalty, so intra mode is more likely tested.
    if (best_rdc->rdcost != INT64_MAX &&
        (best_pickmode->best_ref_frame != LAST_FRAME ||
         abs(mi->mv[0].as_mv.row) >= motion_thresh ||
         abs(mi->mv[0].as_mv.col) >= motion_thresh)) {
      intra_cost_penalty = intra_cost_penalty >> 2;
      inter_mode_thresh =
          RDCOST(x->rdmult, ref_cost_intra + intra_cost_penalty, 0);
      do_early_exit_rdthresh = 0;
    }
    if ((x->source_variance < AOMMAX(50, (spatial_var_thresh >> 1)) &&
         x->content_state_sb.source_sad_nonrd >= kHighSad) ||
        (is_screen_content && x->source_variance < 50 &&
         ((bsize >= BLOCK_32X32 &&
           x->content_state_sb.source_sad_nonrd != kZeroSad) ||
          x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_U)] == 1 ||
          x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_V)] == 1)))
      force_intra_check = 1;
    // For big blocks worth checking intra (since only DC will be checked),
    // even if best_early_term is set.
    if (bsize >= BLOCK_32X32) best_early_term = 0;
  } else if (rt_sf->source_metrics_sb_nonrd &&
             x->content_state_sb.source_sad_nonrd <= kLowSad) {
    perform_intra_pred = 0;
  }

  if (best_rdc->skip_txfm && best_pickmode->best_mode_initial_skip_flag) {
    if (rt_sf->skip_intra_pred == 1 && best_pickmode->best_mode != NEWMV)
      perform_intra_pred = 0;
    else if (rt_sf->skip_intra_pred == 2)
      perform_intra_pred = 0;
  }

  if (!(best_rdc->rdcost == INT64_MAX || force_intra_check ||
        (perform_intra_pred && !best_early_term &&
         bsize <= cpi->sf.part_sf.max_intra_bsize))) {
    return;
  }

  // Early exit based on RD cost calculated using known rate. When
  // is_screen_content is true, more bias is given to intra modes. Hence,
  // considered conservative threshold in early exit for the same.
  const int64_t known_rd = is_screen_content
                               ? CALC_BIASED_RDCOST(inter_mode_thresh)
                               : inter_mode_thresh;
  if (known_rd > best_rdc->rdcost) return;

  struct estimate_block_intra_args args = { cpi, x, DC_PRED, 1, 0 };
  TX_SIZE intra_tx_size = AOMMIN(
      AOMMIN(max_txsize_lookup[bsize],
             tx_mode_to_biggest_tx_size[txfm_params->tx_mode_search_type]),
      TX_16X16);
  if (is_screen_content && cpi->rc.high_source_sad &&
      x->source_variance > spatial_var_thresh && bsize <= BLOCK_16X16)
    intra_tx_size = TX_4X4;

  PRED_BUFFER *const best_pred = best_pickmode->best_pred;
  if (reuse_prediction && best_pred != NULL) {
    const int bh = block_size_high[bsize];
    const int bw = block_size_wide[bsize];
    if (best_pred->data == orig_dst->buf) {
      *this_mode_pred = &tmp_buffers[get_pred_buffer(tmp_buffers, 3)];
      aom_convolve_copy(best_pred->data, best_pred->stride,
                        (*this_mode_pred)->data, (*this_mode_pred)->stride, bw,
                        bh);
      best_pickmode->best_pred = *this_mode_pred;
    }
  }
  pd->dst = *orig_dst;

  for (int midx = 0; midx < RTC_INTRA_MODES; ++midx) {
    const PREDICTION_MODE this_mode = intra_mode_list[midx];
    const THR_MODES mode_index = mode_idx[INTRA_FRAME][mode_offset(this_mode)];
    const int64_t mode_rd_thresh = rd_threshes[mode_index];

    if (is_prune_intra_mode(cpi, midx, force_intra_check, bsize, segment_id,
                            x->content_state_sb.source_sad_nonrd,
                            x->color_sensitivity))
      continue;

    if (is_screen_content && rt_sf->source_metrics_sb_nonrd) {
      // For spatially flat blocks with zero motion only check
      // DC mode.
      if (x->content_state_sb.source_sad_nonrd == kZeroSad &&
          x->source_variance == 0 && this_mode != DC_PRED)
        continue;
      // Only test Intra for big blocks if spatial_variance is small.
      else if (bsize > BLOCK_32X32 && x->source_variance > 50)
        continue;
    }

    if (rd_less_than_thresh(best_rdc->rdcost, mode_rd_thresh,
                            rd_thresh_freq_fact[mode_index]) &&
        (do_early_exit_rdthresh || this_mode == SMOOTH_PRED)) {
      continue;
    }
    const BLOCK_SIZE uv_bsize =
        get_plane_block_size(bsize, xd->plane[AOM_PLANE_U].subsampling_x,
                             xd->plane[AOM_PLANE_U].subsampling_y);

    mi->mode = this_mode;
    mi->ref_frame[0] = INTRA_FRAME;
    mi->ref_frame[1] = NONE_FRAME;

    av1_invalid_rd_stats(&this_rdc);
    args.mode = this_mode;
    args.skippable = 1;
    args.rdc = &this_rdc;
    mi->tx_size = intra_tx_size;
    compute_intra_yprediction(cm, this_mode, bsize, x, xd);
    // Look into selecting tx_size here, based on prediction residual.
    block_yrd(x, &this_rdc, &args.skippable, bsize, mi->tx_size, 0);
    // TODO(kyslov@) Need to account for skippable
    if (x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_U)]) {
      av1_foreach_transformed_block_in_plane(xd, uv_bsize, AOM_PLANE_U,
                                             estimate_block_intra, &args);
    }
    if (x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_V)]) {
      av1_foreach_transformed_block_in_plane(xd, uv_bsize, AOM_PLANE_V,
                                             estimate_block_intra, &args);
    }

    int mode_cost = 0;
    if (av1_is_directional_mode(this_mode) && av1_use_angle_delta(bsize)) {
      mode_cost +=
          x->mode_costs.angle_delta_cost[this_mode - V_PRED]
                                        [MAX_ANGLE_DELTA +
                                         mi->angle_delta[PLANE_TYPE_Y]];
    }
    if (this_mode == DC_PRED && av1_filter_intra_allowed_bsize(cm, bsize)) {
      mode_cost += x->mode_costs.filter_intra_cost[bsize][0];
    }
    this_rdc.rate += ref_cost_intra;
    this_rdc.rate += intra_cost_penalty;
    this_rdc.rate += mode_cost;
    this_rdc.rdcost = RDCOST(x->rdmult, this_rdc.rate, this_rdc.dist);

    if (is_screen_content && rt_sf->source_metrics_sb_nonrd) {
      // For blocks with low spatial variance and color sad,
      // favor the intra-modes, only on scene/slide change.
      if (cpi->rc.high_source_sad && x->source_variance < 800 &&
          (x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_U)] ||
           x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_V)]))
        this_rdc.rdcost = CALC_BIASED_RDCOST(this_rdc.rdcost);
      // Otherwise bias against intra for blocks with zero
      // motion and no color, on non-scene/slide changes.
      else if (!cpi->rc.high_source_sad && x->source_variance > 0 &&
               x->content_state_sb.source_sad_nonrd == kZeroSad &&
               x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_U)] == 0 &&
               x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_V)] == 0)
        this_rdc.rdcost = (3 * this_rdc.rdcost) >> 1;
    }

    if (this_rdc.rdcost < best_rdc->rdcost) {
      *best_rdc = this_rdc;
      best_pickmode->best_mode = this_mode;
      best_pickmode->best_tx_size = mi->tx_size;
      best_pickmode->best_ref_frame = INTRA_FRAME;
      best_pickmode->best_second_ref_frame = NONE;
      best_pickmode->best_mode_skip_txfm = this_rdc.skip_txfm;
      if (!this_rdc.skip_txfm) {
        memcpy(ctx->blk_skip, x->txfm_search_info.blk_skip,
               sizeof(x->txfm_search_info.blk_skip[0]) * ctx->num_4x4_blk);
      }
      mi->uv_mode = this_mode;
      mi->mv[0].as_int = INVALID_MV;
      mi->mv[1].as_int = INVALID_MV;
    }
  }
  mi->tx_size = best_pickmode->best_tx_size;
}

static AOM_INLINE int is_filter_search_enabled_blk(
    AV1_COMP *cpi, MACROBLOCK *x, int mi_row, int mi_col, BLOCK_SIZE bsize,
    int segment_id, int cb_pred_filter_search, InterpFilter *filt_select) {
  const AV1_COMMON *const cm = &cpi->common;
  // filt search disabled
  if (!cpi->sf.rt_sf.use_nonrd_filter_search) return 0;
  // filt search purely based on mode properties
  if (!cb_pred_filter_search) return 1;
  MACROBLOCKD *const xd = &x->e_mbd;
  int enable_interp_search = 0;
  if (!(xd->left_mbmi && xd->above_mbmi)) {
    // neighbors info unavailable
    enable_interp_search = 2;
  } else if (!(is_inter_block(xd->left_mbmi) &&
               is_inter_block(xd->above_mbmi))) {
    // neighbor is INTRA
    enable_interp_search = 2;
  } else if (xd->left_mbmi->interp_filters.as_int !=
             xd->above_mbmi->interp_filters.as_int) {
    // filters are different
    enable_interp_search = 2;
  } else if ((cb_pred_filter_search == 1) &&
             (xd->left_mbmi->interp_filters.as_filters.x_filter !=
              EIGHTTAP_REGULAR)) {
    // not regular
    enable_interp_search = 2;
  } else {
    // enable prediction based on chessboard pattern
    if (xd->left_mbmi->interp_filters.as_filters.x_filter == EIGHTTAP_SMOOTH)
      *filt_select = EIGHTTAP_SMOOTH;
    const int bsl = mi_size_wide_log2[bsize];
    enable_interp_search =
        (bool)((((mi_row + mi_col) >> bsl) +
                get_chessboard_index(cm->current_frame.frame_number)) &
               0x1);
    if (cyclic_refresh_segment_id_boosted(segment_id)) enable_interp_search = 1;
  }
  return enable_interp_search;
}

static AOM_INLINE int skip_mode_by_threshold(
    PREDICTION_MODE mode, MV_REFERENCE_FRAME ref_frame, int_mv mv,
    int frames_since_golden, const int *const rd_threshes,
    const int *const rd_thresh_freq_fact, int64_t best_cost, int best_skip,
    int extra_shift) {
  int skip_this_mode = 0;
  const THR_MODES mode_index = mode_idx[ref_frame][INTER_OFFSET(mode)];
  int64_t mode_rd_thresh =
      best_skip ? ((int64_t)rd_threshes[mode_index]) << (extra_shift + 1)
                : ((int64_t)rd_threshes[mode_index]) << extra_shift;

  // Increase mode_rd_thresh value for non-LAST for improved encoding
  // speed
  if (ref_frame != LAST_FRAME) {
    mode_rd_thresh = mode_rd_thresh << 1;
    if (ref_frame == GOLDEN_FRAME && frames_since_golden > 4)
      mode_rd_thresh = mode_rd_thresh << (extra_shift + 1);
  }

  if (rd_less_than_thresh(best_cost, mode_rd_thresh,
                          rd_thresh_freq_fact[mode_index]))
    if (mv.as_int != 0) skip_this_mode = 1;

  return skip_this_mode;
}

static AOM_INLINE int skip_mode_by_low_temp(
    PREDICTION_MODE mode, MV_REFERENCE_FRAME ref_frame, BLOCK_SIZE bsize,
    CONTENT_STATE_SB content_state_sb, int_mv mv, int force_skip_low_temp_var) {
  // Skip non-zeromv mode search for non-LAST frame if force_skip_low_temp_var
  // is set. If nearestmv for golden frame is 0, zeromv mode will be skipped
  // later.
  if (force_skip_low_temp_var && ref_frame != LAST_FRAME && mv.as_int != 0) {
    return 1;
  }

  if (content_state_sb.source_sad_nonrd != kHighSad && bsize >= BLOCK_64X64 &&
      force_skip_low_temp_var && mode == NEWMV) {
    return 1;
  }
  return 0;
}

static AOM_INLINE int skip_mode_by_bsize_and_ref_frame(
    PREDICTION_MODE mode, MV_REFERENCE_FRAME ref_frame, BLOCK_SIZE bsize,
    int extra_prune, unsigned int sse_zeromv_norm, int more_prune) {
  const unsigned int thresh_skip_golden = 500;

  if (ref_frame != LAST_FRAME && sse_zeromv_norm < thresh_skip_golden &&
      mode == NEWMV)
    return 1;

  if (bsize == BLOCK_128X128 && mode == NEWMV) return 1;

  // Skip testing non-LAST if this flag is set.
  if (extra_prune) {
    if (extra_prune > 1 && ref_frame != LAST_FRAME &&
        (bsize > BLOCK_16X16 && mode == NEWMV))
      return 1;

    if (ref_frame != LAST_FRAME && mode == NEARMV) return 1;

    if (more_prune && bsize >= BLOCK_32X32 && mode == NEARMV) return 1;
  }
  return 0;
}

static void set_color_sensitivity(AV1_COMP *cpi, MACROBLOCK *x,
                                  BLOCK_SIZE bsize, int y_sad,
                                  unsigned int source_variance,
                                  struct buf_2d yv12_mb[MAX_MB_PLANE]) {
  const int subsampling_x = cpi->common.seq_params->subsampling_x;
  const int subsampling_y = cpi->common.seq_params->subsampling_y;
  int factor = (bsize >= BLOCK_32X32) ? 2 : 3;
  int shift = 3;
  if (cpi->oxcf.tune_cfg.content == AOM_CONTENT_SCREEN &&
      cpi->rc.high_source_sad) {
    factor = 1;
    shift = 6;
  }
  NOISE_LEVEL noise_level = kLow;
  int norm_sad =
      y_sad >> (b_width_log2_lookup[bsize] + b_height_log2_lookup[bsize]);
  unsigned int thresh_spatial = (cpi->common.width > 1920) ? 5000 : 1000;
  // If the spatial source variance is high and the normalized y_sad
  // is low, then y-channel is likely good for mode estimation, so keep
  // color_sensitivity off. For low noise content for now, since there is
  // some bdrate regression for noisy color clip.
  if (cpi->noise_estimate.enabled)
    noise_level = av1_noise_estimate_extract_level(&cpi->noise_estimate);
  if (noise_level == kLow && source_variance > thresh_spatial &&
      cpi->oxcf.tune_cfg.content != AOM_CONTENT_SCREEN && norm_sad < 50) {
    x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_U)] = 0;
    x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_V)] = 0;
    return;
  }
  const int num_planes = av1_num_planes(&cpi->common);

  for (int plane = AOM_PLANE_U; plane < num_planes; ++plane) {
    if (x->color_sensitivity[COLOR_SENS_IDX(plane)] == 2 ||
        source_variance < 50) {
      struct macroblock_plane *const p = &x->plane[plane];
      const BLOCK_SIZE bs =
          get_plane_block_size(bsize, subsampling_x, subsampling_y);

      const int uv_sad = cpi->ppi->fn_ptr[bs].sdf(
          p->src.buf, p->src.stride, yv12_mb[plane].buf, yv12_mb[plane].stride);

      const int norm_uv_sad =
          uv_sad >> (b_width_log2_lookup[bs] + b_height_log2_lookup[bs]);
      x->color_sensitivity[COLOR_SENS_IDX(plane)] =
          uv_sad > (factor * (y_sad >> shift)) && norm_uv_sad > 40;
      if (source_variance < 50 && norm_uv_sad > 100)
        x->color_sensitivity[COLOR_SENS_IDX(plane)] = 1;
    }
  }
}

static void setup_compound_prediction(const AV1_COMMON *cm, MACROBLOCK *x,
                                      struct buf_2d yv12_mb[8][MAX_MB_PLANE],
                                      const int *use_ref_frame_mask,
                                      const MV_REFERENCE_FRAME *rf,
                                      int *ref_mv_idx) {
  MACROBLOCKD *const xd = &x->e_mbd;
  MB_MODE_INFO *const mbmi = xd->mi[0];
  MB_MODE_INFO_EXT *const mbmi_ext = &x->mbmi_ext;
  MV_REFERENCE_FRAME ref_frame_comp;
  if (!use_ref_frame_mask[rf[1]]) {
    // Need to setup pred_block, if it hasn't been done in find_predictors.
    const YV12_BUFFER_CONFIG *yv12 = get_ref_frame_yv12_buf(cm, rf[1]);
    const int num_planes = av1_num_planes(cm);
    if (yv12 != NULL) {
      const struct scale_factors *const sf =
          get_ref_scale_factors_const(cm, rf[1]);
      av1_setup_pred_block(xd, yv12_mb[rf[1]], yv12, sf, sf, num_planes);
    }
  }
  ref_frame_comp = av1_ref_frame_type(rf);
  mbmi_ext->mode_context[ref_frame_comp] = 0;
  mbmi_ext->ref_mv_count[ref_frame_comp] = UINT8_MAX;
  av1_find_mv_refs(cm, xd, mbmi, ref_frame_comp, mbmi_ext->ref_mv_count,
                   xd->ref_mv_stack, xd->weight, NULL, mbmi_ext->global_mvs,
                   mbmi_ext->mode_context);
  av1_copy_usable_ref_mv_stack_and_weight(xd, mbmi_ext, ref_frame_comp);
  *ref_mv_idx = mbmi->ref_mv_idx + 1;
}

static void set_compound_mode(MACROBLOCK *x, MV_REFERENCE_FRAME ref_frame,
                              MV_REFERENCE_FRAME ref_frame2, int ref_mv_idx,
                              int_mv frame_mv[MB_MODE_COUNT][REF_FRAMES],
                              PREDICTION_MODE this_mode) {
  MACROBLOCKD *const xd = &x->e_mbd;
  MB_MODE_INFO *const mi = xd->mi[0];
  mi->ref_frame[0] = ref_frame;
  mi->ref_frame[1] = ref_frame2;
  mi->compound_idx = 1;
  mi->comp_group_idx = 0;
  mi->interinter_comp.type = COMPOUND_AVERAGE;
  MV_REFERENCE_FRAME ref_frame_comp = av1_ref_frame_type(mi->ref_frame);
  if (this_mode == GLOBAL_GLOBALMV) {
    frame_mv[this_mode][ref_frame].as_int = 0;
    frame_mv[this_mode][ref_frame2].as_int = 0;
  } else if (this_mode == NEAREST_NEARESTMV) {
    frame_mv[this_mode][ref_frame].as_int =
        xd->ref_mv_stack[ref_frame_comp][0].this_mv.as_int;
    frame_mv[this_mode][ref_frame2].as_int =
        xd->ref_mv_stack[ref_frame_comp][0].comp_mv.as_int;
  } else if (this_mode == NEAR_NEARMV) {
    frame_mv[this_mode][ref_frame].as_int =
        xd->ref_mv_stack[ref_frame_comp][ref_mv_idx].this_mv.as_int;
    frame_mv[this_mode][ref_frame2].as_int =
        xd->ref_mv_stack[ref_frame_comp][ref_mv_idx].comp_mv.as_int;
  }
}

// Prune compound mode if the single mode variance is lower than a fixed
// percentage of the median value.
static bool skip_comp_based_on_var(
    const unsigned int (*single_vars)[REF_FRAMES], BLOCK_SIZE bsize) {
  unsigned int best_var = UINT_MAX;
  for (int cur_mode_idx = 0; cur_mode_idx < RTC_INTER_MODES; cur_mode_idx++) {
    for (int ref_idx = 0; ref_idx < REF_FRAMES; ref_idx++) {
      best_var = AOMMIN(best_var, single_vars[cur_mode_idx][ref_idx]);
    }
  }
  const unsigned int thresh_64 = (unsigned int)(0.57356805f * 8659);
  const unsigned int thresh_32 = (unsigned int)(0.23964763f * 4281);

  // Currently, the thresh for 128 and 16 are not well-tuned. We are using the
  // results from 64 and 32 as an heuristic.
  switch (bsize) {
    case BLOCK_128X128: return best_var < 4 * thresh_64;
    case BLOCK_64X64: return best_var < thresh_64;
    case BLOCK_32X32: return best_var < thresh_32;
    case BLOCK_16X16: return best_var < thresh_32 / 4;
    default: return false;
  }
}

static AOM_FORCE_INLINE void fill_single_inter_mode_costs(
    int (*single_inter_mode_costs)[REF_FRAMES], int num_inter_modes,
    const REF_MODE *reference_mode_set, const ModeCosts *mode_costs,
    const int16_t *mode_context) {
  bool ref_frame_used[REF_FRAMES] = { false };
  for (int idx = 0; idx < num_inter_modes; idx++) {
    ref_frame_used[reference_mode_set[idx].ref_frame] = true;
  }

  for (int this_ref_frame = LAST_FRAME; this_ref_frame < REF_FRAMES;
       this_ref_frame++) {
    if (!ref_frame_used[this_ref_frame]) {
      continue;
    }

    const MV_REFERENCE_FRAME rf[2] = { this_ref_frame, NONE_FRAME };
    const int16_t mode_ctx = av1_mode_context_analyzer(mode_context, rf);
    for (PREDICTION_MODE this_mode = NEARESTMV; this_mode <= NEWMV;
         this_mode++) {
      single_inter_mode_costs[INTER_OFFSET(this_mode)][this_ref_frame] =
          cost_mv_ref(mode_costs, this_mode, mode_ctx);
    }
  }
}

static AOM_INLINE bool is_globalmv_better(
    PREDICTION_MODE this_mode, MV_REFERENCE_FRAME ref_frame, int rate_mv,
    const ModeCosts *mode_costs,
    const int (*single_inter_mode_costs)[REF_FRAMES],
    const MB_MODE_INFO_EXT *mbmi_ext) {
  const int globalmv_mode_cost =
      single_inter_mode_costs[INTER_OFFSET(GLOBALMV)][ref_frame];
  int this_mode_cost =
      rate_mv + single_inter_mode_costs[INTER_OFFSET(this_mode)][ref_frame];
  if (this_mode == NEWMV || this_mode == NEARMV) {
    const MV_REFERENCE_FRAME rf[2] = { ref_frame, NONE_FRAME };
    this_mode_cost += get_drl_cost(
        NEWMV, 0, mbmi_ext, mode_costs->drl_mode_cost0, av1_ref_frame_type(rf));
  }
  return this_mode_cost > globalmv_mode_cost;
}

// Set up the mv/ref_frames etc based on the comp_index. Returns 1 if it
// succeeds, 0 if it fails.
static AOM_INLINE int setup_compound_params_from_comp_idx(
    const AV1_COMP *cpi, MACROBLOCK *x, struct buf_2d yv12_mb[8][MAX_MB_PLANE],
    PREDICTION_MODE *this_mode, MV_REFERENCE_FRAME *ref_frame,
    MV_REFERENCE_FRAME *ref_frame2, int_mv frame_mv[MB_MODE_COUNT][REF_FRAMES],
    const int *use_ref_frame_mask, int comp_index,
    bool comp_use_zero_zeromv_only, MV_REFERENCE_FRAME *last_comp_ref_frame) {
  const MV_REFERENCE_FRAME *rf = comp_ref_mode_set[comp_index].ref_frame;
  *this_mode = comp_ref_mode_set[comp_index].pred_mode;
  *ref_frame = rf[0];
  *ref_frame2 = rf[1];
  assert(*ref_frame == LAST_FRAME);
  assert(*this_mode == GLOBAL_GLOBALMV || *this_mode == NEAREST_NEARESTMV);
  if (comp_use_zero_zeromv_only && *this_mode != GLOBAL_GLOBALMV) {
    return 0;
  }
  if (*ref_frame2 == GOLDEN_FRAME &&
      (cpi->sf.rt_sf.ref_frame_comp_nonrd[0] == 0 ||
       !(cpi->ref_frame_flags & AOM_GOLD_FLAG))) {
    return 0;
  } else if (*ref_frame2 == LAST2_FRAME &&
             (cpi->sf.rt_sf.ref_frame_comp_nonrd[1] == 0 ||
              !(cpi->ref_frame_flags & AOM_LAST2_FLAG))) {
    return 0;
  } else if (*ref_frame2 == ALTREF_FRAME &&
             (cpi->sf.rt_sf.ref_frame_comp_nonrd[2] == 0 ||
              !(cpi->ref_frame_flags & AOM_ALT_FLAG))) {
    return 0;
  }
  int ref_mv_idx = 0;
  if (*last_comp_ref_frame != rf[1]) {
    // Only needs to be done once per reference pair.
    setup_compound_prediction(&cpi->common, x, yv12_mb, use_ref_frame_mask, rf,
                              &ref_mv_idx);
    *last_comp_ref_frame = rf[1];
  }
  set_compound_mode(x, *ref_frame, *ref_frame2, ref_mv_idx, frame_mv,
                    *this_mode);
  if (*this_mode != GLOBAL_GLOBALMV &&
      frame_mv[*this_mode][*ref_frame].as_int == 0 &&
      frame_mv[*this_mode][*ref_frame2].as_int == 0) {
    return 0;
  }

  return 1;
}

static AOM_INLINE bool previous_mode_performed_poorly(
    PREDICTION_MODE mode, MV_REFERENCE_FRAME ref_frame,
    const unsigned int (*vars)[REF_FRAMES],
    const int64_t (*uv_dist)[REF_FRAMES]) {
  unsigned int best_var = UINT_MAX;
  int64_t best_uv_dist = INT64_MAX;
  for (int midx = 0; midx < RTC_INTER_MODES; midx++) {
    best_var = AOMMIN(best_var, vars[midx][ref_frame]);
    best_uv_dist = AOMMIN(best_uv_dist, uv_dist[midx][ref_frame]);
  }
  assert(best_var != UINT_MAX && "Invalid variance data.");
  const float mult = 1.125f;
  bool var_bad = mult * best_var < vars[INTER_OFFSET(mode)][ref_frame];
  if (uv_dist[INTER_OFFSET(mode)][ref_frame] < INT64_MAX &&
      best_uv_dist != uv_dist[INTER_OFFSET(mode)][ref_frame]) {
    // If we have chroma info, then take it into account
    var_bad &= mult * best_uv_dist < uv_dist[INTER_OFFSET(mode)][ref_frame];
  }
  return var_bad;
}

static AOM_INLINE bool prune_compoundmode_with_singlemode_var(
    PREDICTION_MODE compound_mode, MV_REFERENCE_FRAME ref_frame,
    MV_REFERENCE_FRAME ref_frame2, const int_mv (*frame_mv)[REF_FRAMES],
    const uint8_t (*mode_checked)[REF_FRAMES],
    const unsigned int (*vars)[REF_FRAMES],
    const int64_t (*uv_dist)[REF_FRAMES]) {
  const PREDICTION_MODE single_mode0 = compound_ref0_mode(compound_mode);
  const PREDICTION_MODE single_mode1 = compound_ref1_mode(compound_mode);

  bool first_ref_valid = false, second_ref_valid = false;
  bool first_ref_bad = false, second_ref_bad = false;
  if (mode_checked[single_mode0][ref_frame] &&
      frame_mv[single_mode0][ref_frame].as_int ==
          frame_mv[compound_mode][ref_frame].as_int &&
      vars[INTER_OFFSET(single_mode0)][ref_frame] < UINT_MAX) {
    first_ref_valid = true;
    first_ref_bad =
        previous_mode_performed_poorly(single_mode0, ref_frame, vars, uv_dist);
  }
  if (mode_checked[single_mode1][ref_frame2] &&
      frame_mv[single_mode1][ref_frame2].as_int ==
          frame_mv[compound_mode][ref_frame2].as_int &&
      vars[INTER_OFFSET(single_mode1)][ref_frame2] < UINT_MAX) {
    second_ref_valid = true;
    second_ref_bad =
        previous_mode_performed_poorly(single_mode1, ref_frame2, vars, uv_dist);
  }
  if (first_ref_valid && second_ref_valid) {
    return first_ref_bad && second_ref_bad;
  } else if (first_ref_valid || second_ref_valid) {
    return first_ref_bad || second_ref_bad;
  }
  return false;
}

// Function to setup parameters used for inter mode evaluation in non-rd.
static AOM_FORCE_INLINE void set_params_nonrd_pick_inter_mode(
    AV1_COMP *cpi, MACROBLOCK *x, InterModeSearchStateNonrd *search_state,
    RD_STATS *rd_cost, int *force_skip_low_temp_var, int *skip_pred_mv,
    int mi_row, int mi_col, int gf_temporal_ref, unsigned char segment_id,
    BLOCK_SIZE bsize
#if CONFIG_AV1_TEMPORAL_DENOISING
    ,
    PICK_MODE_CONTEXT *ctx, int denoise_svc_pickmode
#endif
) {
  AV1_COMMON *const cm = &cpi->common;
  MACROBLOCKD *const xd = &x->e_mbd;
  TxfmSearchInfo *txfm_info = &x->txfm_search_info;
  MB_MODE_INFO *const mi = xd->mi[0];
  const ModeCosts *mode_costs = &x->mode_costs;

  // Initialize variance and distortion (chroma) for all modes and reference
  // frames
  for (int idx = 0; idx < RTC_INTER_MODES; idx++) {
    for (int ref = 0; ref < REF_FRAMES; ref++) {
      search_state->vars[idx][ref] = UINT_MAX;
      search_state->uv_dist[idx][ref] = INT64_MAX;
    }
  }

  // Initialize values of color sensitivity with sb level color sensitivity
  av1_copy(x->color_sensitivity, x->color_sensitivity_sb);

  init_best_pickmode(&search_state->best_pickmode);

  // Estimate cost for single reference frames
  estimate_single_ref_frame_costs(cm, xd, mode_costs, segment_id, bsize,
                                  search_state->ref_costs_single);

  // Reset flag to indicate modes evaluated
  av1_zero(search_state->mode_checked);

  txfm_info->skip_txfm = 0;

  // Initialize mode decisions
  av1_invalid_rd_stats(&search_state->best_rdc);
  av1_invalid_rd_stats(&search_state->this_rdc);
  av1_invalid_rd_stats(rd_cost);
  for (int ref_idx = 0; ref_idx < REF_FRAMES; ++ref_idx) {
    x->warp_sample_info[ref_idx].num = -1;
  }

  mi->bsize = bsize;
  mi->ref_frame[0] = NONE_FRAME;
  mi->ref_frame[1] = NONE_FRAME;

#if CONFIG_AV1_TEMPORAL_DENOISING
  if (cpi->oxcf.noise_sensitivity > 0) {
    // if (cpi->ppi->use_svc) denoise_svc_pickmode =
    // av1_denoise_svc_non_key(cpi);
    if (cpi->denoiser.denoising_level > kDenLowLow && denoise_svc_pickmode)
      av1_denoiser_reset_frame_stats(ctx);
  }
#endif

  // Populate predicated motion vectors for LAST_FRAME
  if (cpi->ref_frame_flags & AOM_LAST_FLAG)
    find_predictors(cpi, x, LAST_FRAME, search_state->frame_mv,
                    search_state->yv12_mb, bsize, *force_skip_low_temp_var,
                    x->force_zeromv_skip_for_blk);

  // Update mask to use all reference frame
  get_ref_frame_use_mask(cpi, x, mi, mi_row, mi_col, bsize, gf_temporal_ref,
                         search_state->use_ref_frame_mask,
                         force_skip_low_temp_var);

  *skip_pred_mv = x->force_zeromv_skip_for_blk ||
                  (x->nonrd_prune_ref_frame_search > 2 &&
                   x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_U)] != 2 &&
                   x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_V)] != 2);

  // Populate predicated motion vectors for other single reference frame
  // Start at LAST_FRAME + 1.
  for (MV_REFERENCE_FRAME ref_frame_iter = LAST_FRAME + 1;
       ref_frame_iter <= ALTREF_FRAME; ++ref_frame_iter) {
    if (search_state->use_ref_frame_mask[ref_frame_iter]) {
      find_predictors(cpi, x, ref_frame_iter, search_state->frame_mv,
                      search_state->yv12_mb, bsize, *force_skip_low_temp_var,
                      *skip_pred_mv);
    }
  }
}

// Function to check the inter mode can be skipped based on mode statistics and
// speed features settings.
static AOM_FORCE_INLINE bool skip_inter_mode_nonrd(
    AV1_COMP *cpi, MACROBLOCK *x, InterModeSearchStateNonrd *search_state,
    int64_t *thresh_sad_pred, int *force_mv_inter_layer, int *is_single_pred,
    PREDICTION_MODE *this_mode, MV_REFERENCE_FRAME *last_comp_ref_frame,
    MV_REFERENCE_FRAME *ref_frame, MV_REFERENCE_FRAME *ref_frame2, int idx,
    int_mv svc_mv, int force_skip_low_temp_var, unsigned int sse_zeromv_norm,
    int num_inter_modes, unsigned char segment_id, BLOCK_SIZE bsize,
    bool comp_use_zero_zeromv_only, bool check_globalmv) {
  AV1_COMMON *const cm = &cpi->common;
  const struct segmentation *const seg = &cm->seg;
  const SVC *const svc = &cpi->svc;
  MACROBLOCKD *const xd = &x->e_mbd;
  MB_MODE_INFO *const mi = xd->mi[0];
  const REAL_TIME_SPEED_FEATURES *const rt_sf = &cpi->sf.rt_sf;

  // Skip compound mode based on reference frame mask and type of the mode and
  // for allowed compound modes, setup ref mv stack and reference frame.
  if (idx >= num_inter_modes) {
    const int comp_index = idx - num_inter_modes;
    if (!setup_compound_params_from_comp_idx(
            cpi, x, search_state->yv12_mb, this_mode, ref_frame, ref_frame2,
            search_state->frame_mv, search_state->use_ref_frame_mask,
            comp_index, comp_use_zero_zeromv_only, last_comp_ref_frame)) {
      return true;
    }
    *is_single_pred = 0;
  } else {
    *this_mode = ref_mode_set[idx].pred_mode;
    *ref_frame = ref_mode_set[idx].ref_frame;
    *ref_frame2 = NONE_FRAME;
  }

  // Skip the single reference mode for which mode check flag is set.
  if (*is_single_pred && search_state->mode_checked[*this_mode][*ref_frame]) {
    return true;
  }

  // Skip GLOBALMV mode if check_globalmv flag is not enabled.
  if (!check_globalmv && *this_mode == GLOBALMV) {
    return true;
  }

#if COLLECT_PICK_MODE_STAT
  aom_usec_timer_start(&ms_stat.timer1);
  ms_stat.num_searches[bsize][*this_mode]++;
#endif
  mi->mode = *this_mode;
  mi->ref_frame[0] = *ref_frame;
  mi->ref_frame[1] = *ref_frame2;

  // Skip the mode if use reference frame mask flag is not set.
  if (!search_state->use_ref_frame_mask[*ref_frame]) return true;

  // Skip mode for some modes and reference frames when
  // force_zeromv_skip_for_blk flag is true.
  if (x->force_zeromv_skip_for_blk &&
      ((!(*this_mode == NEARESTMV &&
          search_state->frame_mv[*this_mode][*ref_frame].as_int == 0) &&
        *this_mode != GLOBALMV) ||
       *ref_frame != LAST_FRAME))
    return true;

  // Skip compound mode based on variance of previously evaluated single
  // reference modes.
  if (rt_sf->prune_compoundmode_with_singlemode_var && !*is_single_pred &&
      prune_compoundmode_with_singlemode_var(
          *this_mode, *ref_frame, *ref_frame2, search_state->frame_mv,
          search_state->mode_checked, search_state->vars,
          search_state->uv_dist)) {
    return true;
  }

  *force_mv_inter_layer = 0;
  if (cpi->ppi->use_svc && svc->spatial_layer_id > 0 &&
      ((*ref_frame == LAST_FRAME && svc->skip_mvsearch_last) ||
       (*ref_frame == GOLDEN_FRAME && svc->skip_mvsearch_gf) ||
       (*ref_frame == ALTREF_FRAME && svc->skip_mvsearch_altref))) {
    // Only test mode if NEARESTMV/NEARMV is (svc_mv.mv.col, svc_mv.mv.row),
    // otherwise set NEWMV to (svc_mv.mv.col, svc_mv.mv.row).
    // Skip newmv and filter search.
    *force_mv_inter_layer = 1;
    if (*this_mode == NEWMV) {
      search_state->frame_mv[*this_mode][*ref_frame] = svc_mv;
    } else if (search_state->frame_mv[*this_mode][*ref_frame].as_int !=
               svc_mv.as_int) {
      return true;
    }
  }

  // If the segment reference frame feature is enabled then do nothing if the
  // current ref frame is not allowed.
  if (segfeature_active(seg, segment_id, SEG_LVL_REF_FRAME) &&
      get_segdata(seg, segment_id, SEG_LVL_REF_FRAME) != (int)(*ref_frame))
    return true;

  // For screen content: for base spatial layer only for now.
  if (cpi->oxcf.tune_cfg.content == AOM_CONTENT_SCREEN &&
      cpi->svc.spatial_layer_id == 0) {
    // If source_sad is computed: skip non-zero motion
    // check for stationary (super)blocks. Otherwise if superblock
    // has motion skip the modes with zero motion for flat blocks,
    // and color is not set.
    // For the latter condition: the same condition should apply
    // to newmv if (0, 0), so this latter condition is repeated
    // below after search_new_mv.
    if (rt_sf->source_metrics_sb_nonrd) {
      if ((search_state->frame_mv[*this_mode][*ref_frame].as_int != 0 &&
           x->content_state_sb.source_sad_nonrd == kZeroSad) ||
          (search_state->frame_mv[*this_mode][*ref_frame].as_int == 0 &&
           x->content_state_sb.source_sad_nonrd != kZeroSad &&
           ((x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_U)] == 0 &&
             x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_V)] == 0) ||
            cpi->rc.high_source_sad) &&
           x->source_variance == 0))
        return true;
    }
    // Skip NEWMV search for flat blocks.
    if (*this_mode == NEWMV && x->source_variance < 100) return true;
    // Skip non-LAST for color on flat blocks.
    if (*ref_frame > LAST_FRAME && x->source_variance == 0 &&
        (x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_U)] == 1 ||
         x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_V)] == 1))
      return true;
  }

  // Skip mode based on block size, reference frame mode and other block
  // properties.
  if (skip_mode_by_bsize_and_ref_frame(
          *this_mode, *ref_frame, bsize, x->nonrd_prune_ref_frame_search,
          sse_zeromv_norm, rt_sf->nonrd_aggressive_skip))
    return true;

  // Skip mode based on low temporal variance and souce sad.
  if (skip_mode_by_low_temp(*this_mode, *ref_frame, bsize, x->content_state_sb,
                            search_state->frame_mv[*this_mode][*ref_frame],
                            force_skip_low_temp_var))
    return true;

  // Disable this drop out case if the ref frame segment level feature is
  // enabled for this segment. This is to prevent the possibility that we
  // end up unable to pick any mode.
  if (!segfeature_active(seg, segment_id, SEG_LVL_REF_FRAME)) {
    // Check for skipping GOLDEN and ALTREF based pred_mv_sad.
    if (rt_sf->nonrd_prune_ref_frame_search > 0 &&
        x->pred_mv_sad[*ref_frame] != INT_MAX && *ref_frame != LAST_FRAME) {
      if ((int64_t)(x->pred_mv_sad[*ref_frame]) > *thresh_sad_pred) return true;
    }
  }

  // Check for skipping NEARMV based on pred_mv_sad.
  if (*this_mode == NEARMV && x->pred_mv1_sad[*ref_frame] != INT_MAX &&
      x->pred_mv1_sad[*ref_frame] > (x->pred_mv0_sad[*ref_frame] << 1))
    return true;

  // Skip single reference mode based on rd threshold.
  if (*is_single_pred) {
    if (skip_mode_by_threshold(
            *this_mode, *ref_frame,
            search_state->frame_mv[*this_mode][*ref_frame],
            cpi->rc.frames_since_golden, cpi->rd.threshes[segment_id][bsize],
            x->thresh_freq_fact[bsize], search_state->best_rdc.rdcost,
            search_state->best_pickmode.best_mode_skip_txfm,
            (rt_sf->nonrd_aggressive_skip ? 1 : 0)))
      return true;
  }
  return false;
}

// Function to perform inter mode evaluation for non-rd
static AOM_FORCE_INLINE bool handle_inter_mode_nonrd(
    AV1_COMP *cpi, MACROBLOCK *x, InterModeSearchStateNonrd *search_state,
    PICK_MODE_CONTEXT *ctx, PRED_BUFFER **this_mode_pred,
    PRED_BUFFER *tmp_buffer, InterPredParams inter_pred_params_sr,
    int *best_early_term, unsigned int *sse_zeromv_norm, bool *check_globalmv,
#if CONFIG_AV1_TEMPORAL_DENOISING
    int64_t *zero_last_cost_orig, int denoise_svc_pickmode,
#endif
    int idx, int force_mv_inter_layer, int is_single_pred, int skip_pred_mv,
    int gf_temporal_ref, int use_model_yrd_large, int filter_search_enabled_blk,
    BLOCK_SIZE bsize, PREDICTION_MODE this_mode, InterpFilter filt_select,
    int cb_pred_filter_search, int reuse_inter_pred) {
  AV1_COMMON *const cm = &cpi->common;
  MACROBLOCKD *const xd = &x->e_mbd;
  MB_MODE_INFO *const mi = xd->mi[0];
  const MB_MODE_INFO_EXT *const mbmi_ext = &x->mbmi_ext;
  const int mi_row = xd->mi_row;
  const int mi_col = xd->mi_col;
  struct macroblockd_plane *const pd = &xd->plane[AOM_PLANE_Y];
  const int bw = block_size_wide[bsize];
  const InterpFilter filter_ref = cm->features.interp_filter;
  const InterpFilter default_interp_filter = EIGHTTAP_REGULAR;
  TxfmSearchInfo *txfm_info = &x->txfm_search_info;
  const ModeCosts *mode_costs = &x->mode_costs;
  const REAL_TIME_SPEED_FEATURES *const rt_sf = &cpi->sf.rt_sf;
  BEST_PICKMODE *const best_pickmode = &search_state->best_pickmode;

  MV_REFERENCE_FRAME ref_frame = mi->ref_frame[0];
  MV_REFERENCE_FRAME ref_frame2 = mi->ref_frame[1];
  int_mv *const this_mv = &search_state->frame_mv[this_mode][ref_frame];
  unsigned int var = UINT_MAX;
  int this_early_term = 0;
  int rate_mv = 0;
  int is_skippable;
  int skip_this_mv = 0;
  unsigned int var_threshold = UINT_MAX;
  PREDICTION_MODE this_best_mode;
  RD_STATS nonskip_rdc;
  av1_invalid_rd_stats(&nonskip_rdc);

  if (this_mode == NEWMV && !force_mv_inter_layer) {
#if COLLECT_PICK_MODE_STAT
    aom_usec_timer_start(&ms_stat.timer2);
#endif
    // Find the best motion vector for single/compound mode.
    const bool skip_newmv = search_new_mv(
        cpi, x, search_state->frame_mv, ref_frame, gf_temporal_ref, bsize,
        mi_row, mi_col, &rate_mv, &search_state->best_rdc);
#if COLLECT_PICK_MODE_STAT
    aom_usec_timer_mark(&ms_stat.timer2);
    ms_stat.ms_time[bsize][this_mode] +=
        aom_usec_timer_elapsed(&ms_stat.timer2);
#endif
    // Skip NEWMV mode,
    //   (i). For bsize smaller than 16X16
    //  (ii). Based on sad of the predicted mv w.r.t LAST_FRAME
    // (iii). When motion vector is same as that of reference mv
    if (skip_newmv) {
      return true;
    }
  }

  // Check the current motion vector is same as that of previously evaluated
  // motion vectors.
  for (PREDICTION_MODE inter_mv_mode = NEARESTMV; inter_mv_mode <= NEWMV;
       inter_mv_mode++) {
    if (inter_mv_mode == this_mode) continue;
    if (is_single_pred &&
        search_state->mode_checked[inter_mv_mode][ref_frame] &&
        this_mv->as_int ==
            search_state->frame_mv[inter_mv_mode][ref_frame].as_int) {
      skip_this_mv = 1;
      break;
    }
  }

  // Skip single mode if current motion vector is same that of previously
  // evaluated motion vectors.
  if (skip_this_mv && is_single_pred) return true;

  // For screen: for spatially flat blocks with non-zero motion,
  // skip newmv if the motion vector is (0, 0), and color is not set.
  if (this_mode == NEWMV && cpi->oxcf.tune_cfg.content == AOM_CONTENT_SCREEN &&
      cpi->svc.spatial_layer_id == 0 && rt_sf->source_metrics_sb_nonrd) {
    if (this_mv->as_int == 0 &&
        x->content_state_sb.source_sad_nonrd != kZeroSad &&
        ((x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_U)] == 0 &&
          x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_V)] == 0) ||
         cpi->rc.high_source_sad) &&
        x->source_variance == 0)
      return true;
  }

  mi->mode = this_mode;
  mi->mv[0].as_int = this_mv->as_int;
  mi->mv[1].as_int = 0;
  if (!is_single_pred)
    mi->mv[1].as_int = search_state->frame_mv[this_mode][ref_frame2].as_int;

  // Set buffers to store predicted samples for reuse
  if (reuse_inter_pred) {
    if (!*this_mode_pred) {
      *this_mode_pred = &tmp_buffer[3];
    } else {
      *this_mode_pred = &tmp_buffer[get_pred_buffer(tmp_buffer, 3)];
      pd->dst.buf = (*this_mode_pred)->data;
      pd->dst.stride = bw;
    }
  }

  if (idx == 0 && !skip_pred_mv) {
    // Set color sensitivity on first tested mode only.
    // Use y-sad already computed in find_predictors: take the sad with motion
    // vector closest to 0; the uv-sad computed below in set_color_sensitivity
    // is for zeromv.
    // For screen: first check if golden reference is being used, if so,
    // force color_sensitivity on if the color sensitivity for sb_g is on.
    if (cpi->oxcf.tune_cfg.content == AOM_CONTENT_SCREEN &&
        search_state->use_ref_frame_mask[GOLDEN_FRAME]) {
      if (x->color_sensitivity_sb_g[COLOR_SENS_IDX(AOM_PLANE_U)] == 1)
        x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_U)] = 1;
      if (x->color_sensitivity_sb_g[COLOR_SENS_IDX(AOM_PLANE_V)] == 1)
        x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_V)] = 1;
    } else {
      int y_sad = x->pred_mv0_sad[LAST_FRAME];
      if (x->pred_mv1_sad[LAST_FRAME] != INT_MAX &&
          (abs(search_state->frame_mv[NEARMV][LAST_FRAME].as_mv.col) +
           abs(search_state->frame_mv[NEARMV][LAST_FRAME].as_mv.row)) <
              (abs(search_state->frame_mv[NEARESTMV][LAST_FRAME].as_mv.col) +
               abs(search_state->frame_mv[NEARESTMV][LAST_FRAME].as_mv.row)))
        y_sad = x->pred_mv1_sad[LAST_FRAME];
      set_color_sensitivity(cpi, x, bsize, y_sad, x->source_variance,
                            search_state->yv12_mb[LAST_FRAME]);
    }
  }

  mi->motion_mode = SIMPLE_TRANSLATION;
#if !CONFIG_REALTIME_ONLY
  if (cpi->oxcf.motion_mode_cfg.allow_warped_motion) {
    calc_num_proj_ref(cpi, x, mi);
  }
#endif
  // set variance threshold for compound mode pruning
  if (rt_sf->prune_compoundmode_with_singlecompound_var && !is_single_pred &&
      use_model_yrd_large) {
    const PREDICTION_MODE single_mode0 = compound_ref0_mode(this_mode);
    const PREDICTION_MODE single_mode1 = compound_ref1_mode(this_mode);
    var_threshold =
        AOMMIN(var_threshold,
               search_state->vars[INTER_OFFSET(single_mode0)][ref_frame]);
    var_threshold =
        AOMMIN(var_threshold,
               search_state->vars[INTER_OFFSET(single_mode1)][ref_frame2]);
  }

  // decide interpolation filter, build prediction signal, get sse
  const bool is_mv_subpel =
      (mi->mv[0].as_mv.row & 0x07) || (mi->mv[0].as_mv.col & 0x07);
  const bool enable_filt_search_this_mode =
      (filter_search_enabled_blk == 2)
          ? true
          : (filter_search_enabled_blk && !force_mv_inter_layer &&
             is_single_pred &&
             (ref_frame == LAST_FRAME || !x->nonrd_prune_ref_frame_search));
  if (is_mv_subpel && enable_filt_search_this_mode) {
#if COLLECT_PICK_MODE_STAT
    aom_usec_timer_start(&ms_stat.timer2);
#endif
    search_filter_ref(
        cpi, x, &search_state->this_rdc, &inter_pred_params_sr, mi_row, mi_col,
        tmp_buffer, bsize, reuse_inter_pred, this_mode_pred, &this_early_term,
        &var, use_model_yrd_large, best_pickmode->best_sse, is_single_pred);
#if COLLECT_PICK_MODE_STAT
    aom_usec_timer_mark(&ms_stat.timer2);
    ms_stat.ifs_time[bsize][this_mode] +=
        aom_usec_timer_elapsed(&ms_stat.timer2);
#endif
#if !CONFIG_REALTIME_ONLY
  } else if (cpi->oxcf.motion_mode_cfg.allow_warped_motion &&
             this_mode == NEWMV) {
    // Find the best motion mode when current mode is NEWMV
    search_motion_mode(cpi, x, &search_state->this_rdc, mi_row, mi_col, bsize,
                       &this_early_term, use_model_yrd_large, &rate_mv,
                       best_pickmode->best_sse);
    if (this_mode == NEWMV) {
      this_mv[0] = mi->mv[0];
    }
#endif
  } else {
    mi->interp_filters =
        (filter_ref == SWITCHABLE)
            ? av1_broadcast_interp_filter(default_interp_filter)
            : av1_broadcast_interp_filter(filter_ref);
    if (force_mv_inter_layer)
      mi->interp_filters = av1_broadcast_interp_filter(EIGHTTAP_REGULAR);

    // If it is sub-pel motion and cb_pred_filter_search is enabled, select
    // the pre-decided filter
    if (is_mv_subpel && cb_pred_filter_search)
      mi->interp_filters = av1_broadcast_interp_filter(filt_select);

#if COLLECT_PICK_MODE_STAT
    aom_usec_timer_start(&ms_stat.timer2);
#endif
    if (is_single_pred) {
      SubpelParams subpel_params;
      // Initialize inter mode level params for single reference mode.
      init_inter_mode_params(&mi->mv[0].as_mv, &inter_pred_params_sr,
                             &subpel_params, xd->block_ref_scale_factors[0],
                             pd->pre->width, pd->pre->height);
      av1_enc_build_inter_predictor_y_nonrd(xd, &inter_pred_params_sr,
                                            &subpel_params);
    } else {
      av1_enc_build_inter_predictor(cm, xd, mi_row, mi_col, NULL, bsize,
                                    AOM_PLANE_Y, AOM_PLANE_Y);
    }

    if (use_model_yrd_large) {
      model_skip_for_sb_y_large(cpi, bsize, mi_row, mi_col, x, xd,
                                &search_state->this_rdc, &this_early_term, 0,
                                best_pickmode->best_sse, &var, var_threshold);
    } else {
      model_rd_for_sb_y(cpi, bsize, x, xd, &search_state->this_rdc, &var, 0,
                        &this_early_term);
    }
#if COLLECT_PICK_MODE_STAT
    aom_usec_timer_mark(&ms_stat.timer2);
    ms_stat.model_rd_time[bsize][this_mode] +=
        aom_usec_timer_elapsed(&ms_stat.timer2);
#endif
  }

  // update variance for single mode
  if (is_single_pred) {
    search_state->vars[INTER_OFFSET(this_mode)][ref_frame] = var;
    if (this_mv->as_int == 0) {
      search_state->vars[INTER_OFFSET(GLOBALMV)][ref_frame] = var;
    }
  }
  // prune compound mode based on single mode var threshold
  if (!is_single_pred && var > var_threshold) {
    if (reuse_inter_pred) free_pred_buffer(*this_mode_pred);
    return true;
  }

  if (ref_frame == LAST_FRAME && this_mv->as_int == 0) {
    *sse_zeromv_norm = (unsigned int)(search_state->this_rdc.sse >>
                                      (b_width_log2_lookup[bsize] +
                                       b_height_log2_lookup[bsize]));
  }

  // Perform early termination based on sse.
  if (rt_sf->sse_early_term_inter_search &&
      early_term_inter_search_with_sse(rt_sf->sse_early_term_inter_search,
                                       bsize, search_state->this_rdc.sse,
                                       best_pickmode->best_sse, this_mode)) {
    if (reuse_inter_pred) free_pred_buffer(*this_mode_pred);
    return true;
  }

#if COLLECT_PICK_MODE_STAT
  ms_stat.num_nonskipped_searches[bsize][this_mode]++;
#endif

  const int skip_ctx = av1_get_skip_txfm_context(xd);
  const int skip_txfm_cost = mode_costs->skip_txfm_cost[skip_ctx][1];
  const int no_skip_txfm_cost = mode_costs->skip_txfm_cost[skip_ctx][0];
  const int64_t sse_y = search_state->this_rdc.sse;

  if (this_early_term) {
    search_state->this_rdc.skip_txfm = 1;
    search_state->this_rdc.rate = skip_txfm_cost;
    search_state->this_rdc.dist = search_state->this_rdc.sse << 4;
  } else {
#if COLLECT_PICK_MODE_STAT
    aom_usec_timer_start(&ms_stat.timer2);
#endif
    // Calculates RD Cost using Hadamard transform.
    block_yrd(x, &search_state->this_rdc, &is_skippable, bsize, mi->tx_size, 1);
    if (search_state->this_rdc.skip_txfm ||
        RDCOST(x->rdmult, search_state->this_rdc.rate,
               search_state->this_rdc.dist) >=
            RDCOST(x->rdmult, 0, search_state->this_rdc.sse)) {
      if (!search_state->this_rdc.skip_txfm) {
        // Need to store "real" rdc for possible future use if UV rdc
        // disallows tx skip
        nonskip_rdc = search_state->this_rdc;
        nonskip_rdc.rate += no_skip_txfm_cost;
      }
      search_state->this_rdc.rate = skip_txfm_cost;
      search_state->this_rdc.skip_txfm = 1;
      search_state->this_rdc.dist = search_state->this_rdc.sse;
    } else {
      search_state->this_rdc.rate += no_skip_txfm_cost;
    }

    // Populate predicted sample for chroma planes based on color sensitivity.
    if ((x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_U)] ||
         x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_V)])) {
      RD_STATS rdc_uv;
      const BLOCK_SIZE uv_bsize =
          get_plane_block_size(bsize, xd->plane[AOM_PLANE_U].subsampling_x,
                               xd->plane[AOM_PLANE_U].subsampling_y);
      if (x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_U)]) {
        av1_enc_build_inter_predictor(cm, xd, mi_row, mi_col, NULL, bsize,
                                      AOM_PLANE_U, AOM_PLANE_U);
      }
      if (x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_V)]) {
        av1_enc_build_inter_predictor(cm, xd, mi_row, mi_col, NULL, bsize,
                                      AOM_PLANE_V, AOM_PLANE_V);
      }
      // Compute sse for chroma planes.
      const int64_t sse_uv = model_rd_for_sb_uv(cpi, uv_bsize, x, xd, &rdc_uv,
                                                AOM_PLANE_U, AOM_PLANE_V);
      search_state->this_rdc.sse += sse_uv;
      // Restore Y rdc if UV rdc disallows txfm skip
      if (search_state->this_rdc.skip_txfm && !rdc_uv.skip_txfm &&
          nonskip_rdc.rate != INT_MAX)
        search_state->this_rdc = nonskip_rdc;
      if (is_single_pred) {
        search_state->uv_dist[INTER_OFFSET(this_mode)][ref_frame] = rdc_uv.dist;
      }
      search_state->this_rdc.rate += rdc_uv.rate;
      search_state->this_rdc.dist += rdc_uv.dist;
      search_state->this_rdc.skip_txfm =
          search_state->this_rdc.skip_txfm && rdc_uv.skip_txfm;
    }
#if COLLECT_PICK_MODE_STAT
    aom_usec_timer_mark(&ms_stat.timer2);
    ms_stat.txfm_time[bsize][this_mode] +=
        aom_usec_timer_elapsed(&ms_stat.timer2);
#endif
  }

  this_best_mode = this_mode;
  // TODO(kyslov) account for UV prediction cost
  search_state->this_rdc.rate += rate_mv;
  if (!is_single_pred) {
    const int16_t mode_ctx =
        av1_mode_context_analyzer(mbmi_ext->mode_context, mi->ref_frame);
    search_state->this_rdc.rate += cost_mv_ref(mode_costs, this_mode, mode_ctx);
  } else {
    // If the current mode has zeromv but is not GLOBALMV, compare the rate
    // cost. If GLOBALMV is cheaper, use GLOBALMV instead.
    if (this_mode != GLOBALMV &&
        this_mv->as_int == search_state->frame_mv[GLOBALMV][ref_frame].as_int) {
      if (is_globalmv_better(this_mode, ref_frame, rate_mv, mode_costs,
                             search_state->single_inter_mode_costs, mbmi_ext)) {
        this_best_mode = GLOBALMV;
      }
    }

    search_state->this_rdc.rate +=
        search_state
            ->single_inter_mode_costs[INTER_OFFSET(this_best_mode)][ref_frame];
  }

  if (is_single_pred && this_mv->as_int == 0 && var < UINT_MAX) {
    search_state->vars[INTER_OFFSET(GLOBALMV)][ref_frame] = var;
  }

  search_state->this_rdc.rate += search_state->ref_costs_single[ref_frame];

  search_state->this_rdc.rdcost = RDCOST(x->rdmult, search_state->this_rdc.rate,
                                         search_state->this_rdc.dist);
  if (cpi->oxcf.rc_cfg.mode == AOM_CBR && is_single_pred) {
    newmv_diff_bias(xd, this_best_mode, &search_state->this_rdc, bsize,
                    search_state->frame_mv[this_best_mode][ref_frame].as_mv.row,
                    search_state->frame_mv[this_best_mode][ref_frame].as_mv.col,
                    cpi->speed, x->source_variance, x->content_state_sb);
  }

#if CONFIG_AV1_TEMPORAL_DENOISING
  if (cpi->oxcf.noise_sensitivity > 0 && denoise_svc_pickmode &&
      cpi->denoiser.denoising_level > kDenLowLow) {
    av1_denoiser_update_frame_stats(mi, sse_y, this_mode, ctx);
    // Keep track of zero_last cost.
    if (ref_frame == LAST_FRAME && this_mv->as_int == 0)
      *zero_last_cost_orig = search_state->this_rdc.rdcost;
  }
#else
  (void)(sse_y);
#endif

  search_state->mode_checked[this_mode][ref_frame] = 1;
  search_state->mode_checked[this_best_mode][ref_frame] = 1;

  if (*check_globalmv) {
    int32_t abs_mv =
        abs(search_state->frame_mv[this_best_mode][ref_frame].as_mv.row) +
        abs(search_state->frame_mv[this_best_mode][ref_frame].as_mv.col);
    // Early exit check: if the magnitude of this_best_mode's mv is small
    // enough, we skip GLOBALMV check in the next loop iteration.
    if (abs_mv < 2) {
      *check_globalmv = false;
    }
  }
#if COLLECT_PICK_MODE_STAT
  aom_usec_timer_mark(&ms_stat.timer1);
  ms_stat.nonskipped_search_times[bsize][this_mode] +=
      aom_usec_timer_elapsed(&ms_stat.timer1);
#endif

  // Copy best mode params to search state
  if (search_state->this_rdc.rdcost < search_state->best_rdc.rdcost) {
    search_state->best_rdc = search_state->this_rdc;
    *best_early_term = this_early_term;
    update_search_state_nonrd(search_state, mi, txfm_info, &nonskip_rdc, ctx,
                              this_best_mode, sse_y);

    // This is needed for the compound modes.
    search_state->frame_mv_best[this_best_mode][ref_frame].as_int =
        search_state->frame_mv[this_best_mode][ref_frame].as_int;
    if (ref_frame2 > NONE_FRAME) {
      search_state->frame_mv_best[this_best_mode][ref_frame2].as_int =
          search_state->frame_mv[this_best_mode][ref_frame2].as_int;
    }

    if (reuse_inter_pred) {
      free_pred_buffer(best_pickmode->best_pred);
      best_pickmode->best_pred = *this_mode_pred;
    }
  } else {
    if (reuse_inter_pred) free_pred_buffer(*this_mode_pred);
  }

  if (*best_early_term && (idx > 0 || rt_sf->nonrd_aggressive_skip)) {
    txfm_info->skip_txfm = 1;
    return false;
  }
  return true;
}

// Function to perform screen content mode evaluation for non-rd
static AOM_FORCE_INLINE void handle_screen_content_mode_nonrd(
    AV1_COMP *cpi, MACROBLOCK *x, InterModeSearchStateNonrd *search_state,
    PRED_BUFFER *this_mode_pred, PICK_MODE_CONTEXT *ctx,
    PRED_BUFFER *tmp_buffer, struct buf_2d *orig_dst, int skip_idtx_palette,
    int try_palette, BLOCK_SIZE bsize) {
  AV1_COMMON *const cm = &cpi->common;
  const REAL_TIME_SPEED_FEATURES *const rt_sf = &cpi->sf.rt_sf;
  MACROBLOCKD *const xd = &x->e_mbd;
  MB_MODE_INFO *const mi = xd->mi[0];
  struct macroblockd_plane *const pd = &xd->plane[0];
  const int mi_row = xd->mi_row;
  const int mi_col = xd->mi_col;
  const int bw = block_size_wide[bsize];
  const int num_8x8_blocks = ctx->num_4x4_blk / 4;
  TxfmSearchInfo *txfm_info = &x->txfm_search_info;
  BEST_PICKMODE *const best_pickmode = &search_state->best_pickmode;

  // Check for IDTX: based only on Y channel, so avoid when color_sensitivity
  // is set.
  // TODO(marpan): Only allow for 8 bit-depth for now, re-enable for 10/12 bit
  // when issue 3359 is fixed.
  if (cm->seq_params->bit_depth == 8 &&
      cpi->oxcf.tune_cfg.content == AOM_CONTENT_SCREEN && !skip_idtx_palette &&
      !cpi->oxcf.txfm_cfg.use_inter_dct_only && !x->force_zeromv_skip_for_blk &&
      is_inter_mode(best_pickmode->best_mode) &&
      (!rt_sf->prune_idtx_nonrd ||
       (rt_sf->prune_idtx_nonrd && bsize <= BLOCK_32X32 &&
        best_pickmode->best_mode_skip_txfm != 1 && x->source_variance > 200))) {
    RD_STATS idtx_rdc;
    av1_init_rd_stats(&idtx_rdc);
    int is_skippable;
    this_mode_pred = &tmp_buffer[get_pred_buffer(tmp_buffer, 3)];
    pd->dst.buf = this_mode_pred->data;
    pd->dst.stride = bw;

    // Build inter predicted samples and compute RD cost using Identity
    // transform.
    av1_enc_build_inter_predictor(cm, xd, mi_row, mi_col, NULL, bsize,
                                  AOM_PLANE_Y, AOM_PLANE_Y);
    block_yrd_idtx(x, &idtx_rdc, &is_skippable, bsize, mi->tx_size);
    int64_t idx_rdcost = RDCOST(x->rdmult, idtx_rdc.rate, idtx_rdc.dist);
    if (idx_rdcost < search_state->best_rdc.rdcost) {
      // Keep the skip_txfm off if the color_sensitivity is set.
      if (x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_U)] ||
          x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_V)])
        idtx_rdc.skip_txfm = 0;
      best_pickmode->tx_type = IDTX;
      search_state->best_rdc.rdcost = idx_rdcost;
      best_pickmode->best_mode_skip_txfm = idtx_rdc.skip_txfm;
      if (!idtx_rdc.skip_txfm) {
        memcpy(best_pickmode->blk_skip, txfm_info->blk_skip,
               sizeof(txfm_info->blk_skip[0]) * num_8x8_blocks);
      }
      xd->tx_type_map[0] = best_pickmode->tx_type;
      memset(ctx->tx_type_map, best_pickmode->tx_type, ctx->num_4x4_blk);
      memset(xd->tx_type_map, best_pickmode->tx_type, ctx->num_4x4_blk);
    }
    pd->dst = *orig_dst;
  }

  if (!try_palette) return;
  const unsigned int intra_ref_frame_cost =
      search_state->ref_costs_single[INTRA_FRAME];

  // Search palette mode for Luma plane in inter frame.
  av1_search_palette_mode_luma(cpi, x, bsize, intra_ref_frame_cost, ctx,
                               &search_state->this_rdc,
                               search_state->best_rdc.rdcost);
  // Update best mode data in search_state
  if (search_state->this_rdc.rdcost < search_state->best_rdc.rdcost) {
    best_pickmode->pmi = mi->palette_mode_info;
    best_pickmode->best_mode = DC_PRED;
    mi->mv[0].as_int = 0;
    search_state->best_rdc.rate = search_state->this_rdc.rate;
    search_state->best_rdc.dist = search_state->this_rdc.dist;
    search_state->best_rdc.rdcost = search_state->this_rdc.rdcost;
    best_pickmode->best_mode_skip_txfm = search_state->this_rdc.skip_txfm;
    // Keep the skip_txfm off if the color_sensitivity is set.
    if (x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_U)] ||
        x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_V)])
      search_state->this_rdc.skip_txfm = 0;
    if (!search_state->this_rdc.skip_txfm) {
      memcpy(ctx->blk_skip, txfm_info->blk_skip,
             sizeof(txfm_info->blk_skip[0]) * ctx->num_4x4_blk);
    }
    if (xd->tx_type_map[0] != DCT_DCT)
      av1_copy_array(ctx->tx_type_map, xd->tx_type_map, ctx->num_4x4_blk);
  }
}

/*!\brief AV1 inter mode selection based on Non-RD optimized model.
 *
 * \ingroup nonrd_mode_search
 * \callgraph
 * Top level function for Non-RD optimized inter mode selection.
 * This finction will loop over subset of inter modes and select the best one
 * based on calculated modelled RD cost. While making decisions which modes to
 * check, this function applies heuristics based on previously checked modes,
 * block residual variance, block size, and other factors to prune certain
 * modes and reference frames. Currently only single reference frame modes
 * are checked. Additional heuristics are applied to decide if intra modes
 *  need to be checked.
 *  *
 * \param[in]    cpi            Top-level encoder structure
 * \param[in]    tile_data      Pointer to struct holding adaptive
                                data/contexts/models for the tile during
                                encoding
 * \param[in]    x              Pointer to structure holding all the data for
                                the current macroblock
 * \param[in]    rd_cost        Struct to keep track of the RD information
 * \param[in]    bsize          Current block size
 * \param[in]    ctx            Structure to hold snapshot of coding context
                                during the mode picking process
 *
 * \remark Nothing is returned. Instead, the MB_MODE_INFO struct inside x
 * is modified to store information about the best mode computed
 * in this function. The rd_cost struct is also updated with the RD stats
 * corresponding to the best mode found.
 */
void av1_nonrd_pick_inter_mode_sb(AV1_COMP *cpi, TileDataEnc *tile_data,
                                  MACROBLOCK *x, RD_STATS *rd_cost,
                                  BLOCK_SIZE bsize, PICK_MODE_CONTEXT *ctx) {
  AV1_COMMON *const cm = &cpi->common;
  SVC *const svc = &cpi->svc;
  MACROBLOCKD *const xd = &x->e_mbd;
  MB_MODE_INFO *const mi = xd->mi[0];
  struct macroblockd_plane *const pd = &xd->plane[AOM_PLANE_Y];
  const MB_MODE_INFO_EXT *const mbmi_ext = &x->mbmi_ext;
  MV_REFERENCE_FRAME ref_frame, ref_frame2;
  const unsigned char segment_id = mi->segment_id;
  int best_early_term = 0;
  int force_skip_low_temp_var = 0;
  unsigned int sse_zeromv_norm = UINT_MAX;
  int skip_pred_mv = 0;
  const int num_inter_modes = NUM_INTER_MODES;
  const REAL_TIME_SPEED_FEATURES *const rt_sf = &cpi->sf.rt_sf;
  bool check_globalmv = rt_sf->check_globalmv_on_single_ref;
  PRED_BUFFER tmp_buffer[4];
  DECLARE_ALIGNED(16, uint8_t, pred_buf[MAX_MB_PLANE * MAX_SB_SQUARE]);
  PRED_BUFFER *this_mode_pred = NULL;
  const int reuse_inter_pred =
      rt_sf->reuse_inter_pred_nonrd && cm->seq_params->bit_depth == AOM_BITS_8;
  InterModeSearchStateNonrd search_state;
  av1_zero(search_state.use_ref_frame_mask);
  BEST_PICKMODE *const best_pickmode = &search_state.best_pickmode;
  (void)tile_data;

  const int bh = block_size_high[bsize];
  const int bw = block_size_wide[bsize];
  const int pixels_in_block = bh * bw;
  const int num_8x8_blocks = ctx->num_4x4_blk / 4;
  struct buf_2d orig_dst = pd->dst;
  const TxfmSearchParams *txfm_params = &x->txfm_search_params;
  TxfmSearchInfo *txfm_info = &x->txfm_search_info;
#if COLLECT_PICK_MODE_STAT
  aom_usec_timer_start(&ms_stat.bsize_timer);
#endif
  int64_t thresh_sad_pred = INT64_MAX;
  const int mi_row = xd->mi_row;
  const int mi_col = xd->mi_col;
  int_mv svc_mv = { .as_int = 0 };
  int force_mv_inter_layer = 0;
  bool comp_use_zero_zeromv_only = 0;
  int tot_num_comp_modes = NUM_COMP_INTER_MODES_RT;
#if CONFIG_AV1_TEMPORAL_DENOISING
  const int denoise_recheck_zeromv = 1;
  AV1_PICKMODE_CTX_DEN ctx_den;
  int64_t zero_last_cost_orig = INT64_MAX;
  int denoise_svc_pickmode = 1;
  const int resize_pending = is_frame_resize_pending(cpi);
#endif
  const ModeCosts *mode_costs = &x->mode_costs;

  if (reuse_inter_pred) {
    for (int buf_idx = 0; buf_idx < 3; buf_idx++) {
      tmp_buffer[buf_idx].data = &pred_buf[pixels_in_block * buf_idx];
      tmp_buffer[buf_idx].stride = bw;
      tmp_buffer[buf_idx].in_use = 0;
    }
    tmp_buffer[3].data = pd->dst.buf;
    tmp_buffer[3].stride = pd->dst.stride;
    tmp_buffer[3].in_use = 0;
  }

  const int gf_temporal_ref = is_same_gf_and_last_scale(cm);

  // If the lower spatial layer uses an averaging filter for downsampling
  // (phase = 8), the target decimated pixel is shifted by (1/2, 1/2) relative
  // to source, so use subpel motion vector to compensate. The nonzero motion
  // is half pixel shifted to left and top, so (-4, -4). This has more effect
  // on higher resolutions, so condition it on that for now.
  if (cpi->ppi->use_svc && svc->spatial_layer_id > 0 &&
      svc->downsample_filter_phase[svc->spatial_layer_id - 1] == 8 &&
      cm->width * cm->height > 640 * 480) {
    svc_mv.as_mv.row = -4;
    svc_mv.as_mv.col = -4;
  }

  // Setup parameters used for inter mode evaluation.
  set_params_nonrd_pick_inter_mode(
      cpi, x, &search_state, rd_cost, &force_skip_low_temp_var, &skip_pred_mv,
      mi_row, mi_col, gf_temporal_ref, segment_id, bsize
#if CONFIG_AV1_TEMPORAL_DENOISING
      ,
      ctx, denoise_svc_pickmode
#endif
  );

  if (rt_sf->use_comp_ref_nonrd && is_comp_ref_allowed(bsize)) {
    // Only search compound if bsize \gt BLOCK_16X16.
    if (bsize > BLOCK_16X16) {
      comp_use_zero_zeromv_only = rt_sf->check_only_zero_zeromv_on_large_blocks;
    } else {
      tot_num_comp_modes = 0;
    }
  } else {
    tot_num_comp_modes = 0;
  }

  if (x->pred_mv_sad[LAST_FRAME] != INT_MAX) {
    thresh_sad_pred = ((int64_t)x->pred_mv_sad[LAST_FRAME]) << 1;
    // Increase threshold for less aggressive pruning.
    if (rt_sf->nonrd_prune_ref_frame_search == 1)
      thresh_sad_pred += (x->pred_mv_sad[LAST_FRAME] >> 2);
  }

  const int use_model_yrd_large = get_model_rd_flag(cpi, xd, bsize);

  // decide block-level interp filter search flags:
  // filter_search_enabled_blk:
  // 0: disabled
  // 1: filter search depends on mode properties
  // 2: filter search forced since prediction is unreliable
  // cb_pred_filter_search 0: disabled cb prediction
  InterpFilter filt_select = EIGHTTAP_REGULAR;
  const int cb_pred_filter_search =
      x->content_state_sb.source_sad_nonrd > kVeryLowSad
          ? cpi->sf.interp_sf.cb_pred_filter_search
          : 0;
  const int filter_search_enabled_blk =
      is_filter_search_enabled_blk(cpi, x, mi_row, mi_col, bsize, segment_id,
                                   cb_pred_filter_search, &filt_select);

#if COLLECT_PICK_MODE_STAT
  ms_stat.num_blocks[bsize]++;
#endif
  init_mbmi(mi, DC_PRED, NONE_FRAME, NONE_FRAME, cm);
  mi->tx_size = AOMMIN(
      AOMMIN(max_txsize_lookup[bsize],
             tx_mode_to_biggest_tx_size[txfm_params->tx_mode_search_type]),
      TX_16X16);

  fill_single_inter_mode_costs(search_state.single_inter_mode_costs,
                               num_inter_modes, ref_mode_set, mode_costs,
                               mbmi_ext->mode_context);

  MV_REFERENCE_FRAME last_comp_ref_frame = NONE_FRAME;

  // Initialize inter prediction params at block level for single reference
  // mode.
  InterPredParams inter_pred_params_sr;
  init_inter_block_params(&inter_pred_params_sr, pd->width, pd->height,
                          mi_row * MI_SIZE, mi_col * MI_SIZE, pd->subsampling_x,
                          pd->subsampling_y, xd->bd, is_cur_buf_hbd(xd),
                          /*is_intrabc=*/0);
  inter_pred_params_sr.conv_params =
      get_conv_params(/*do_average=*/0, AOM_PLANE_Y, xd->bd);

  for (int idx = 0; idx < num_inter_modes + tot_num_comp_modes; ++idx) {
    // If we are at the first compound mode, and the single modes already
    // perform well, then end the search.
    if (rt_sf->skip_compound_based_on_var && idx == num_inter_modes &&
        skip_comp_based_on_var(search_state.vars, bsize)) {
      break;
    }

    int is_single_pred = 1;
    PREDICTION_MODE this_mode;

    // Check the inter mode can be skipped based on mode statistics and speed
    // features settings.
    if (skip_inter_mode_nonrd(cpi, x, &search_state, &thresh_sad_pred,
                              &force_mv_inter_layer, &is_single_pred,
                              &this_mode, &last_comp_ref_frame, &ref_frame,
                              &ref_frame2, idx, svc_mv, force_skip_low_temp_var,
                              sse_zeromv_norm, num_inter_modes, segment_id,
                              bsize, comp_use_zero_zeromv_only, check_globalmv))
      continue;

    // Select prediction reference frames.
    for (int plane = 0; plane < MAX_MB_PLANE; plane++) {
      xd->plane[plane].pre[0] = search_state.yv12_mb[ref_frame][plane];
      if (!is_single_pred)
        xd->plane[plane].pre[1] = search_state.yv12_mb[ref_frame2][plane];
    }

    mi->ref_frame[0] = ref_frame;
    mi->ref_frame[1] = ref_frame2;
    set_ref_ptrs(cm, xd, ref_frame, ref_frame2);
    memset(txfm_info->blk_skip, 0,
           sizeof(txfm_info->blk_skip[0]) * num_8x8_blocks);

    // Perform inter mode evaluation for non-rd
    if (!handle_inter_mode_nonrd(
            cpi, x, &search_state, ctx, &this_mode_pred, tmp_buffer,
            inter_pred_params_sr, &best_early_term, &sse_zeromv_norm,
            &check_globalmv,
#if CONFIG_AV1_TEMPORAL_DENOISING
            &zero_last_cost_orig, denoise_svc_pickmode,
#endif
            idx, force_mv_inter_layer, is_single_pred, skip_pred_mv,
            gf_temporal_ref, use_model_yrd_large, filter_search_enabled_blk,
            bsize, this_mode, filt_select, cb_pred_filter_search,
            reuse_inter_pred)) {
      break;
    }
  }

  // Restore mode data of best inter mode
  mi->mode = best_pickmode->best_mode;
  mi->motion_mode = best_pickmode->best_motion_mode;
  mi->wm_params = best_pickmode->wm_params;
  mi->num_proj_ref = best_pickmode->num_proj_ref;
  mi->interp_filters = best_pickmode->best_pred_filter;
  mi->tx_size = best_pickmode->best_tx_size;
  memset(mi->inter_tx_size, mi->tx_size, sizeof(mi->inter_tx_size));
  mi->ref_frame[0] = best_pickmode->best_ref_frame;
  mi->mv[0].as_int = search_state
                         .frame_mv_best[best_pickmode->best_mode]
                                       [best_pickmode->best_ref_frame]
                         .as_int;
  mi->mv[1].as_int = 0;
  if (best_pickmode->best_second_ref_frame > INTRA_FRAME) {
    mi->ref_frame[1] = best_pickmode->best_second_ref_frame;
    mi->mv[1].as_int = search_state
                           .frame_mv_best[best_pickmode->best_mode]
                                         [best_pickmode->best_second_ref_frame]
                           .as_int;
  }
  // Perform intra prediction search, if the best SAD is above a certain
  // threshold.
  mi->angle_delta[PLANE_TYPE_Y] = 0;
  mi->angle_delta[PLANE_TYPE_UV] = 0;
  mi->filter_intra_mode_info.use_filter_intra = 0;

#if COLLECT_PICK_MODE_STAT
  aom_usec_timer_start(&ms_stat.timer1);
  ms_stat.num_searches[bsize][DC_PRED]++;
  ms_stat.num_nonskipped_searches[bsize][DC_PRED]++;
#endif

  // Evaluate Intra modes in inter frame
  if (!x->force_zeromv_skip_for_blk)
    estimate_intra_mode(cpi, x, bsize, best_early_term,
                        search_state.ref_costs_single[INTRA_FRAME],
                        reuse_inter_pred, &orig_dst, tmp_buffer,
                        &this_mode_pred, &search_state.best_rdc, best_pickmode,
                        ctx);

  int skip_idtx_palette = (x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_U)] ||
                           x->color_sensitivity[COLOR_SENS_IDX(AOM_PLANE_V)]) &&
                          x->content_state_sb.source_sad_nonrd != kZeroSad &&
                          !cpi->rc.high_source_sad;

  int try_palette =
      !skip_idtx_palette && cpi->oxcf.tool_cfg.enable_palette &&
      av1_allow_palette(cpi->common.features.allow_screen_content_tools,
                        mi->bsize);
  try_palette = try_palette && is_mode_intra(best_pickmode->best_mode) &&
                x->source_variance > 0 && !x->force_zeromv_skip_for_blk &&
                (cpi->rc.high_source_sad || x->source_variance > 500);

  // Perform screen content mode evaluation for non-rd
  handle_screen_content_mode_nonrd(cpi, x, &search_state, this_mode_pred, ctx,
                                   tmp_buffer, &orig_dst, skip_idtx_palette,
                                   try_palette, bsize);

#if COLLECT_PICK_MODE_STAT
  aom_usec_timer_mark(&ms_stat.timer1);
  ms_stat.nonskipped_search_times[bsize][DC_PRED] +=
      aom_usec_timer_elapsed(&ms_stat.timer1);
#endif

  pd->dst = orig_dst;
  // Best mode is finalized. Restore the mode data to mbmi
  if (try_palette) mi->palette_mode_info = best_pickmode->pmi;
  mi->mode = best_pickmode->best_mode;
  mi->ref_frame[0] = best_pickmode->best_ref_frame;
  mi->ref_frame[1] = best_pickmode->best_second_ref_frame;
  txfm_info->skip_txfm = best_pickmode->best_mode_skip_txfm;
  if (!txfm_info->skip_txfm) {
    // For inter modes: copy blk_skip from best_pickmode, which is
    // defined for 8x8 blocks. If palette or intra mode was selected
    // as best then blk_skip is already copied into the ctx.
    if (best_pickmode->best_mode >= INTRA_MODE_END)
      memcpy(ctx->blk_skip, best_pickmode->blk_skip,
             sizeof(best_pickmode->blk_skip[0]) * num_8x8_blocks);
  }
  if (has_second_ref(mi)) {
    mi->comp_group_idx = 0;
    mi->compound_idx = 1;
    mi->interinter_comp.type = COMPOUND_AVERAGE;
  }

  if (!is_inter_block(mi)) {
    mi->interp_filters = av1_broadcast_interp_filter(SWITCHABLE_FILTERS);
  }

  // Restore the predicted samples of best mode to final buffer
  if (reuse_inter_pred && best_pickmode->best_pred != NULL) {
    PRED_BUFFER *const best_pred = best_pickmode->best_pred;
    if (best_pred->data != orig_dst.buf && is_inter_mode(mi->mode)) {
      aom_convolve_copy(best_pred->data, best_pred->stride, pd->dst.buf,
                        pd->dst.stride, bw, bh);
    }
  }

#if CONFIG_AV1_TEMPORAL_DENOISING
  if (cpi->oxcf.noise_sensitivity > 0 && resize_pending == 0 &&
      denoise_svc_pickmode && cpi->denoiser.denoising_level > kDenLowLow &&
      cpi->denoiser.reset == 0) {
    AV1_DENOISER_DECISION decision = COPY_BLOCK;
    ctx->sb_skip_denoising = 0;
    av1_pickmode_ctx_den_update(
        &ctx_den, zero_last_cost_orig, search_state.ref_costs_single,
        search_state.frame_mv, reuse_inter_pred, best_pickmode);
    av1_denoiser_denoise(cpi, x, mi_row, mi_col, bsize, ctx, &decision,
                         gf_temporal_ref);
    if (denoise_recheck_zeromv)
      recheck_zeromv_after_denoising(
          cpi, mi, x, xd, decision, &ctx_den, search_state.yv12_mb,
          &search_state.best_rdc, best_pickmode, bsize, mi_row, mi_col);
    best_pickmode->best_ref_frame = ctx_den.best_ref_frame;
  }
#endif

  // Update the factors used for RD thresholding for all modes.
  if (cpi->sf.inter_sf.adaptive_rd_thresh && !has_second_ref(mi)) {
    THR_MODES best_mode_idx =
        mode_idx[best_pickmode->best_ref_frame][mode_offset(mi->mode)];
    if (best_pickmode->best_ref_frame == INTRA_FRAME) {
      // Only consider the modes that are included in the intra_mode_list.
      int intra_modes = sizeof(intra_mode_list) / sizeof(PREDICTION_MODE);
      for (int mode_index = 0; mode_index < intra_modes; mode_index++) {
        update_thresh_freq_fact(cpi, x, bsize, INTRA_FRAME, best_mode_idx,
                                intra_mode_list[mode_index]);
      }
    } else {
      PREDICTION_MODE this_mode;
      for (this_mode = NEARESTMV; this_mode <= NEWMV; ++this_mode) {
        update_thresh_freq_fact(cpi, x, bsize, best_pickmode->best_ref_frame,
                                best_mode_idx, this_mode);
      }
    }
  }

#if CONFIG_INTERNAL_STATS
  store_coding_context(x, ctx, mi->mode);
#else
  store_coding_context(x, ctx);
#endif  // CONFIG_INTERNAL_STATS

#if COLLECT_PICK_MODE_STAT
  aom_usec_timer_mark(&ms_stat.bsize_timer);
  ms_stat.total_block_times[bsize] +=
      aom_usec_timer_elapsed(&ms_stat.bsize_timer);
  print_time(&ms_stat, bsize, cm->mi_params.mi_rows, cm->mi_params.mi_cols,
             mi_row, mi_col);
#endif  // COLLECT_PICK_MODE_STAT

  *rd_cost = search_state.best_rdc;
}
