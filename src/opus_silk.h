#pragma once
#include "opus_rc.h"

/*
 * The SILK layer of Opus (RFC 6716, section 4.2): linear prediction at 8,
 * 12 or 16 kHz, resampled to 48 kHz. Integer arithmetic, bit-exact with
 * the reference decoder, including its resampler.
 */

#define SILK_MAX_FRAME 320 /* 20 ms at 16 kHz */
#define SILK_MAX_SUBFR 80
#define SILK_MAX_ORDER 16

typedef struct {
    int sIIR[6];
    short sFIR[8];
    short delay_buf[16];
    int batch_size, inv_ratio_Q16, fs_in_kHz, fs_out_kHz, input_delay;
} silk_resampler_t;

typedef struct {
    int pitchL_Q8;
    short LTPCoef_Q14[5];
    short prevLPC_Q12[SILK_MAX_ORDER];
    int last_frame_lost, rand_seed;
    short randScale_Q14;
    int conc_energy, conc_energy_shift;
    short prevLTP_scale_Q14;
    int prevGain_Q16[2];
    int fs_kHz, nb_subfr, subfr_length;
} silk_plc_t;

typedef struct {
    int exc_buf_Q14[SILK_MAX_FRAME];
    short smth_NLSF_Q15[SILK_MAX_ORDER];
    int synth_state[SILK_MAX_ORDER];
    int smth_gain_Q16, rand_seed, fs_kHz;
} silk_cng_t;

typedef struct {
    signed char signal_type, quant_offset_type, gains[4], nlsf[SILK_MAX_ORDER + 1], nlsf_interp_Q2;
    short lag_index;
    signed char contour_index, per_index, ltp_index[4], ltp_scale_index, seed;
} silk_indices_t;

typedef struct {
    int prev_gain_Q16;
    int exc_Q14[SILK_MAX_FRAME];
    int sLPC_Q14_buf[SILK_MAX_ORDER];
    short out_buf[SILK_MAX_FRAME + 2 * SILK_MAX_SUBFR];
    int lag_prev;
    signed char last_gain_index;
    int fs_kHz, fs_api_hz, nb_subfr, frame_length, subfr_length, ltp_mem_length, lpc_order;
    short prev_NLSF_Q15[SILK_MAX_ORDER];
    int first_frame_after_reset;
    const unsigned char *pitch_lag_low_bits_iCDF, *pitch_contour_iCDF;
    int wb; /* the wideband NLSF codebook (16 kHz), else narrowband and mediumband */
    int frames_decoded, frames_per_packet;
    int ec_prev_signal_type;
    short ec_prev_lag_index;
    int vad_flags[3], lbrr_flag, lbrr_flags[3];
    silk_resampler_t resampler;
    silk_indices_t idx;
    silk_cng_t cng;
    int loss_cnt, prev_signal_type;
    silk_plc_t plc;
} silk_channel_t;

/* Buffers too large for the stack without a C runtime's probes. */
typedef struct {
    int pulses[SILK_MAX_FRAME];
    short sLTP[SILK_MAX_FRAME];
    int sLTP_Q15[2 * SILK_MAX_FRAME];
    int res_Q14[SILK_MAX_SUBFR];
    int sLPC_Q14[SILK_MAX_SUBFR + SILK_MAX_ORDER];
    int cng_sig_Q10[SILK_MAX_FRAME + SILK_MAX_ORDER];
    short out1[2][SILK_MAX_FRAME + 2];
    short out2[960];
} silk_scratch_t;

typedef struct {
    silk_channel_t ch[2];
    short pred_prev_Q13[2], s_mid[2], s_side[2];
    int channels_api, channels_internal, prev_decode_only_middle;
    silk_scratch_t tmp;
} silk_decoder_t;

void silk_init(silk_decoder_t *d);
/*
 * Decodes one SILK frame (10 or 20 ms) of a packet at 48 kHz into `out`
 * (interleaved for 2 API channels). `internal_hz` is 8000, 12000 or 16000;
 * `payload_ms` the packet's duration (10, 20, 40 or 60); `lost` conceals a
 * lost packet instead. Returns the samples per channel, or -1.
 */
int silk_decode(silk_decoder_t *d, opus_rc_t *rc, int channels_api, int channels_internal, int internal_hz,
                int payload_ms, int lost, int new_packet, short *out);
