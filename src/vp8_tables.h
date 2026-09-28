#pragma once

/* VP8's constant tables (RFC 6386): default and update probabilities, quantizer steps, split partitions. */

extern const unsigned char vp8_coeff_update_probs[4][8][3][11];
extern const unsigned char vp8_default_coeff_probs[4][8][3][11];
extern const unsigned char vp8_default_y_mode_probs[4];
extern const unsigned char vp8_default_uv_mode_probs[3];
extern const unsigned char vp8_mv_update_probs[2][19];
extern const unsigned char vp8_default_mv_probs[2][19];
extern const unsigned char vp8_kf_y_mode_probs[4];
extern const unsigned char vp8_kf_uv_mode_probs[3];
extern const unsigned char vp8_kf_b_mode_probs[10][10][9];
extern const unsigned char vp8_default_b_mode_probs[9];
extern const unsigned char vp8_mv_counts_to_probs[6][4];
extern const unsigned char vp8_split_mv_probs[3];
extern const unsigned char vp8_submv_ref_probs[5][3];
extern const unsigned char vp8_mv_partitions[4][16];
extern const short vp8_dc_q[128];
extern const short vp8_ac_q[128];
