/*
 * Copyright (c) 2022, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#ifndef AOM_AV1_ENCODER_NONRD_OPT_H_
#define AOM_AV1_ENCODER_NONRD_OPT_H_

#include "av1/encoder/rdopt_utils.h"

#define RTC_INTER_MODES (4)
#define RTC_INTRA_MODES (4)
#define RTC_MODES (AOMMAX(RTC_INTER_MODES, RTC_INTRA_MODES))

static const PREDICTION_MODE intra_mode_list[] = { DC_PRED, V_PRED, H_PRED,
                                                   SMOOTH_PRED };

static const PREDICTION_MODE inter_mode_list[] = { NEARESTMV, NEARMV, GLOBALMV,
                                                   NEWMV };

static const THR_MODES mode_idx[REF_FRAMES][RTC_MODES] = {
  { THR_DC, THR_V_PRED, THR_H_PRED, THR_SMOOTH },
  { THR_NEARESTMV, THR_NEARMV, THR_GLOBALMV, THR_NEWMV },
  { THR_NEARESTL2, THR_NEARL2, THR_GLOBALL2, THR_NEWL2 },
  { THR_NEARESTL3, THR_NEARL3, THR_GLOBALL3, THR_NEWL3 },
  { THR_NEARESTG, THR_NEARG, THR_GLOBALG, THR_NEWG },
  { THR_NEARESTB, THR_NEARB, THR_GLOBALB, THR_NEWB },
  { THR_NEARESTA2, THR_NEARA2, THR_GLOBALA2, THR_NEWA2 },
  { THR_NEARESTA, THR_NEARA, THR_GLOBALA, THR_NEWA },
};

// Indicates the blocks for which RD model should be based on special logic
static INLINE int get_model_rd_flag(const AV1_COMP *cpi, const MACROBLOCKD *xd,
                                    BLOCK_SIZE bsize) {
  const int large_block = bsize >= BLOCK_32X32;
  const AV1_COMMON *const cm = &cpi->common;
  return cpi->oxcf.rc_cfg.mode == AOM_CBR && large_block &&
         !cyclic_refresh_segment_id_boosted(xd->mi[0]->segment_id) &&
         cm->quant_params.base_qindex &&
         cm->seq_params->bit_depth == AOM_BITS_8;
}
/*!\brief Finds predicted motion vectors for a block.
 *
 * \ingroup nonrd_mode_search
 * \callgraph
 * \callergraph
 * Finds predicted motion vectors for a block from a certain reference frame.
 * First, it fills reference MV stack, then picks the test from the stack and
 * predicts the final MV for a block for each mode.
 * \param[in]    cpi                      Top-level encoder structure
 * \param[in]    x                        Pointer to structure holding all the
 *                                        data for the current macroblock
 * \param[in]    ref_frame                Reference frame for which to find
 *                                        ref MVs
 * \param[in]    frame_mv                 Predicted MVs for a block
 * \param[in]    yv12_mb                  Buffer to hold predicted block
 * \param[in]    bsize                    Current block size
 * \param[in]    force_skip_low_temp_var  Flag indicating possible mode search
 *                                        prune for low temporal variance block
 * \param[in]    skip_pred_mv             Flag indicating to skip av1_mv_pred
 *
 * \remark Nothing is returned. Instead, predicted MVs are placed into
 * \c frame_mv array
 */
static INLINE void find_predictors(AV1_COMP *cpi, MACROBLOCK *x,
                                   MV_REFERENCE_FRAME ref_frame,
                                   int_mv frame_mv[MB_MODE_COUNT][REF_FRAMES],
                                   struct buf_2d yv12_mb[8][MAX_MB_PLANE],
                                   BLOCK_SIZE bsize,
                                   int force_skip_low_temp_var,
                                   int skip_pred_mv) {
  AV1_COMMON *const cm = &cpi->common;
  MACROBLOCKD *const xd = &x->e_mbd;
  MB_MODE_INFO *const mbmi = xd->mi[0];
  MB_MODE_INFO_EXT *const mbmi_ext = &x->mbmi_ext;
  const YV12_BUFFER_CONFIG *yv12 = get_ref_frame_yv12_buf(cm, ref_frame);
  const int num_planes = av1_num_planes(cm);

  x->pred_mv_sad[ref_frame] = INT_MAX;
  x->pred_mv0_sad[ref_frame] = INT_MAX;
  x->pred_mv1_sad[ref_frame] = INT_MAX;
  frame_mv[NEWMV][ref_frame].as_int = INVALID_MV;
  // TODO(kyslov) this needs various further optimizations. to be continued..
  assert(yv12 != NULL);
  if (yv12 != NULL) {
    const struct scale_factors *const sf =
        get_ref_scale_factors_const(cm, ref_frame);
    av1_setup_pred_block(xd, yv12_mb[ref_frame], yv12, sf, sf, num_planes);
    av1_find_mv_refs(cm, xd, mbmi, ref_frame, mbmi_ext->ref_mv_count,
                     xd->ref_mv_stack, xd->weight, NULL, mbmi_ext->global_mvs,
                     mbmi_ext->mode_context);
    // TODO(Ravi): Populate mbmi_ext->ref_mv_stack[ref_frame][4] and
    // mbmi_ext->weight[ref_frame][4] inside av1_find_mv_refs.
    av1_copy_usable_ref_mv_stack_and_weight(xd, mbmi_ext, ref_frame);
    av1_find_best_ref_mvs_from_stack(
        cm->features.allow_high_precision_mv, mbmi_ext, ref_frame,
        &frame_mv[NEARESTMV][ref_frame], &frame_mv[NEARMV][ref_frame], 0);
    frame_mv[GLOBALMV][ref_frame] = mbmi_ext->global_mvs[ref_frame];
    // Early exit for non-LAST frame if force_skip_low_temp_var is set.
    if (!av1_is_scaled(sf) && bsize >= BLOCK_8X8 && !skip_pred_mv &&
        !(force_skip_low_temp_var && ref_frame != LAST_FRAME)) {
      av1_mv_pred(cpi, x, yv12_mb[ref_frame][0].buf, yv12->y_stride, ref_frame,
                  bsize);
    }
  }
  if (cm->features.switchable_motion_mode) {
    av1_count_overlappable_neighbors(cm, xd);
  }
  mbmi->num_proj_ref = 1;
}

#endif  // AOM_AV1_ENCODER_NONRD_OPT_H_
