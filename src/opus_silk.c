#include <string.h>
#include "opus_silk.h"
#include "opus_silk_tables.h"

#define LTP_ORDER 5
#define MAX_NB_SUBFR 4
#define TYPE_NO_VOICE 0
#define TYPE_UNVOICED 1
#define TYPE_VOICED 2
#define CODE_INDEPENDENTLY 0
#define CODE_INDEPENDENTLY_NO_LTP_SCALING 1
#define CODE_CONDITIONALLY 2
#define NLSF_QUANT_MAX_AMPLITUDE 4
#define N_LEVELS_QGAIN 64
#define MAX_DELTA_GAIN_QUANT 36
#define MIN_DELTA_GAIN_QUANT (-4)
#define GAIN_OFFSET 2090        /* (MIN_QGAIN_DB * 128) / 6 + 16 * 128 */
#define GAIN_INV_SCALE_Q16 1907825
#define QUANT_LEVEL_ADJUST_Q10 80
#define MAX_PULSES 16
#define BWE_AFTER_LOSS_Q16 63570
#define STEREO_INTERP_LEN_MS 8
#define INT32_MAX_ 0x7FFFFFFF
#define INT32_MIN_ (-0x7FFFFFFF - 1)

/* ---- The reference's fixed-point primitives, with their exact rounding ---- */

static int lsh(int a, int s)
{
    return (int)((unsigned)a << s);
}

static int smulwb(int a, int b)
{
    return (int)(((long long)a * (short)b) >> 16);
}

static int smlawb(int a, int b, int c)
{
    return a + smulwb(b, c);
}

static int smulbb(int a, int b)
{
    return (short)a * (short)b;
}

static int smlabb(int a, int b, int c)
{
    return a + (short)b * (short)c;
}

static int smlabb_ovflw(int a, int b, int c)
{
    return (int)((unsigned)a + (unsigned)((short)b * (short)c));
}

static int rshift_round(int a, int s)
{
    return s == 1 ? (a >> 1) + (a & 1) : ((a >> (s - 1)) + 1) >> 1;
}

static long long rshift_round64(long long a, int s)
{
    return s == 1 ? (a >> 1) + (a & 1) : ((a >> (s - 1)) + 1) >> 1;
}

static int smulww(int a, int b)
{
    return smulwb(a, b) + a * rshift_round(b, 16);
}

static int smlaww(int a, int b, int c)
{
    return smlawb(a, b, c) + b * rshift_round(c, 16);
}

static int smmul(int a, int b)
{
    return (int)(((long long)a * b) >> 32);
}

static int sat16(int a)
{
    return a > 32767 ? 32767 : a < -32768 ? -32768 : a;
}

static int limit(int a, int l1, int l2)
{
    if (l1 > l2)
        return a > l1 ? l1 : a < l2 ? l2 : a;
    return a > l2 ? l2 : a < l1 ? l1 : a;
}

static int imin(int a, int b)
{
    return a < b ? a : b;
}

static int imax(int a, int b)
{
    return a > b ? a : b;
}

static int iabs(int a)
{
    return a < 0 ? -a : a;
}

static int sub_sat32(int a, int b)
{
    long long r = (long long)a - b;

    return r > INT32_MAX_ ? INT32_MAX_ : r < INT32_MIN_ ? INT32_MIN_ : (int)r;
}

static int add_sat16(int a, int b)
{
    return sat16(a + b);
}

static int rand_next(int seed)
{
    return (int)(907633515u + (unsigned)seed * 196314165u);
}

static int clz32(int in)
{
    unsigned x = (unsigned)in;
    int n = 0;

    if (!x)
        return 32;
    while (!(x & 0x80000000u)) {
        x <<= 1;
        n++;
    }
    return n;
}

static int ror32(int a, int rot)
{
    unsigned x = (unsigned)a;

    if (rot == 0)
        return a;
    if (rot < 0)
        return (int)((x << -rot) | (x >> (32 + rot)));
    return (int)((x << (32 - rot)) | (x >> rot));
}

static int lshift_sat32(int a, int s)
{
    return lsh(limit(a, INT32_MIN_ >> s, INT32_MAX_ >> s), s);
}

static int sqrt_approx(int x)
{
    int y, lz, frac;

    if (x <= 0)
        return 0;
    lz = clz32(x);
    frac = ror32(x, 24 - lz) & 0x7f;
    y = lz & 1 ? 32768 : 46214;
    y >>= lz >> 1;
    return smlawb(y, y, smulbb(213, frac));
}

static int div32_varQ(int a, int b, int q)
{
    int a_headrm = clz32(iabs(a)) - 1, b_headrm = clz32(iabs(b)) - 1, b_inv, result, lshift;
    int a_nrm = lsh(a, a_headrm), b_nrm = lsh(b, b_headrm);

    b_inv = (INT32_MAX_ >> 2) / (b_nrm >> 16);
    result = smulwb(a_nrm, b_inv);
    a_nrm = (int)((unsigned)a_nrm - (unsigned)lsh(smmul(b_nrm, result), 3));
    result = smlawb(result, a_nrm, b_inv);
    lshift = 29 + a_headrm - b_headrm - q;
    if (lshift < 0)
        return lshift_sat32(result, -lshift);
    return lshift < 32 ? result >> lshift : 0;
}

static int inverse32_varQ(int b, int q)
{
    int b_headrm = clz32(iabs(b)) - 1, b_nrm = lsh(b, b_headrm), b_inv, result, err_Q32, lshift;

    b_inv = (INT32_MAX_ >> 2) / (b_nrm >> 16);
    result = lsh(b_inv, 16);
    err_Q32 = lsh((1 << 29) - smulwb(b_nrm, b_inv), 3);
    result = smlaww(result, err_Q32, b_inv);
    lshift = 61 - b_headrm - q;
    if (lshift <= 0)
        return lshift_sat32(result, -lshift);
    return lshift < 32 ? result >> lshift : 0;
}

static int log2lin(int in_Q7)
{
    int out, frac;

    if (in_Q7 < 0)
        return 0;
    out = 1 << (in_Q7 >> 7);
    frac = in_Q7 & 0x7F;
    if (in_Q7 < 2048)
        return out + ((out * smlawb(frac, smulbb(frac, 128 - frac), -174)) >> 7);
    return out + (out >> 7) * smlawb(frac, smulbb(frac, 128 - frac), -174);
}

static void sum_sqr_shift(int *energy, int *shift, const short *x, int len)
{
    int i, shft = 0, nrg = 0, nrg_tmp;

    len--;
    for (i = 0; i < len; i += 2) {
        nrg = smlabb_ovflw(nrg, x[i], x[i]);
        nrg = smlabb_ovflw(nrg, x[i + 1], x[i + 1]);
        if (nrg < 0) {
            nrg = (int)((unsigned)nrg >> 2);
            shft = 2;
            break;
        }
    }
    for (; i < len; i += 2) {
        nrg_tmp = smulbb(x[i], x[i]);
        nrg_tmp = smlabb_ovflw(nrg_tmp, x[i + 1], x[i + 1]);
        nrg = (int)((unsigned)nrg + ((unsigned)nrg_tmp >> shft));
        if (nrg < 0) {
            nrg = (int)((unsigned)nrg >> 2);
            shft += 2;
        }
    }
    if (i == len) {
        nrg_tmp = smulbb(x[i], x[i]);
        nrg = (int)((unsigned)nrg + ((unsigned)nrg_tmp >> shft));
    }
    if (nrg & 0xC0000000) {
        nrg = (int)((unsigned)nrg >> 2);
        shft += 2;
    }
    *shift = shft;
    *energy = nrg;
}

/* ---- Codebooks ---- */

typedef struct {
    int order, quant_step_Q16;
    const unsigned char *cb1_Q8, *cb1_iCDF, *pred_Q8, *ec_sel, *ec_iCDF;
    const short *delta_min_Q15;
} nlsf_cb_t;

static const nlsf_cb_t k_nlsf_nb = {10,
                                    11796,
                                    k_NLSF_CB1_NB_MB_Q8,
                                    k_NLSF_CB1_iCDF_NB_MB,
                                    k_NLSF_PRED_NB_MB_Q8,
                                    k_NLSF_CB2_SELECT_NB_MB,
                                    k_NLSF_CB2_iCDF_NB_MB,
                                    k_NLSF_DELTA_MIN_NB_MB_Q15};
static const nlsf_cb_t k_nlsf_wb = {16,
                                    9830,
                                    k_NLSF_CB1_WB_Q8,
                                    k_NLSF_CB1_iCDF_WB,
                                    k_NLSF_PRED_WB_Q8,
                                    k_NLSF_CB2_SELECT_WB,
                                    k_NLSF_CB2_iCDF_WB,
                                    k_NLSF_DELTA_MIN_WB_Q15};

static const nlsf_cb_t *nlsf_cb(const silk_channel_t *ch)
{
    return ch->wb ? &k_nlsf_wb : &k_nlsf_nb;
}

static const unsigned char *ltp_gain_iCDF(int per)
{
    return per == 0 ? k_LTP_gain_iCDF_0 : per == 1 ? k_LTP_gain_iCDF_1 : k_LTP_gain_iCDF_2;
}

static const signed char *ltp_vq(int per, int ix)
{
    return per == 0 ? k_LTP_gain_vq_0[ix] : per == 1 ? k_LTP_gain_vq_1[ix] : k_LTP_gain_vq_2[ix];
}

/* ---- Resampler to 48 kHz: 2x all-pass upsampling, then a fractional FIR ---- */

static void resampler_init(silk_resampler_t *s, int fs_in_hz)
{
    static const int delay[3] = {0, 4, 7}; /* 8, 12, 16 kHz to 48 kHz */

    memset(s, 0, sizeof *s);
    s->input_delay = delay[fs_in_hz == 8000 ? 0 : fs_in_hz == 12000 ? 1 : 2];
    s->fs_in_kHz = fs_in_hz / 1000;
    s->fs_out_kHz = 48;
    s->batch_size = s->fs_in_kHz * 10;
    s->inv_ratio_Q16 = lsh((lsh(fs_in_hz, 15)) / 48000, 2);
    while (smulww(s->inv_ratio_Q16, 48000) < lsh(fs_in_hz, 1))
        s->inv_ratio_Q16++;
}

static void up2_hq(int *S, short *out, const short *in, int len)
{
    static const short c0[3] = {1746, 14986, 39083 - 65536}, c1[3] = {6854, 25769, 55542 - 65536};

    for (int k = 0; k < len; k++) {
        int in32 = lsh(in[k], 10), y, x, o1, o2;
        y = in32 - S[0];
        x = smulwb(y, c0[0]);
        o1 = S[0] + x;
        S[0] = in32 + x;
        y = o1 - S[1];
        x = smulwb(y, c0[1]);
        o2 = S[1] + x;
        S[1] = o1 + x;
        y = o2 - S[2];
        x = smlawb(y, y, c0[2]);
        o1 = S[2] + x;
        S[2] = o2 + x;
        out[2 * k] = (short)sat16(rshift_round(o1, 10));
        y = in32 - S[3];
        x = smulwb(y, c1[0]);
        o1 = S[3] + x;
        S[3] = in32 + x;
        y = o1 - S[4];
        x = smulwb(y, c1[1]);
        o2 = S[4] + x;
        S[4] = o1 + x;
        y = o2 - S[5];
        x = smlawb(y, y, c1[2]);
        o1 = S[5] + x;
        S[5] = o2 + x;
        out[2 * k + 1] = (short)sat16(rshift_round(o1, 10));
    }
}

static short *fir_interpol(short *out, const short *buf, int max_index_Q16, int step_Q16)
{
    for (int index_Q16 = 0; index_Q16 < max_index_Q16; index_Q16 += step_Q16) {
        int t = smulwb(index_Q16 & 0xFFFF, 12), r;
        const short *b = &buf[index_Q16 >> 16];
        r = smulbb(b[0], k_resampler_frac_FIR_12[t][0]);
        r = smlabb(r, b[1], k_resampler_frac_FIR_12[t][1]);
        r = smlabb(r, b[2], k_resampler_frac_FIR_12[t][2]);
        r = smlabb(r, b[3], k_resampler_frac_FIR_12[t][3]);
        r = smlabb(r, b[4], k_resampler_frac_FIR_12[11 - t][3]);
        r = smlabb(r, b[5], k_resampler_frac_FIR_12[11 - t][2]);
        r = smlabb(r, b[6], k_resampler_frac_FIR_12[11 - t][1]);
        r = smlabb(r, b[7], k_resampler_frac_FIR_12[11 - t][0]);
        *out++ = (short)sat16(rshift_round(r, 15));
    }
    return out;
}

static void iir_fir(silk_resampler_t *s, short *out, const short *in, int len)
{
    short buf[2 * 160 + 8];
    int n = 0;

    memcpy(buf, s->sFIR, sizeof s->sFIR);
    for (;;) {
        n = imin(len, s->batch_size);
        up2_hq(s->sIIR, &buf[8], in, n);
        out = fir_interpol(out, buf, lsh(n, 17), s->inv_ratio_Q16);
        in += n;
        len -= n;
        if (len <= 0)
            break;
        memmove(buf, &buf[n << 1], 8 * sizeof(short));
    }
    memcpy(s->sFIR, &buf[n << 1], sizeof s->sFIR);
}

static void resample(silk_resampler_t *s, short *out, const short *in, int len)
{
    int n = s->fs_in_kHz - s->input_delay;

    memcpy(&s->delay_buf[s->input_delay], in, sizeof(short) * (size_t)n);
    iir_fir(s, out, s->delay_buf, s->fs_in_kHz);
    iir_fir(s, &out[s->fs_out_kHz], &in[n], len - s->fs_in_kHz);
    memcpy(s->delay_buf, &in[len - s->input_delay], sizeof(short) * (size_t)s->input_delay);
}

/* ---- Channel state ---- */

static void cng_reset(silk_channel_t *ch)
{
    int step = 32767 / (ch->lpc_order + 1), acc = 0;

    for (int i = 0; i < ch->lpc_order; i++) {
        acc += step;
        ch->cng.smth_NLSF_Q15[i] = (short)acc;
    }
    ch->cng.smth_gain_Q16 = 0;
    ch->cng.rand_seed = 3176576;
}

static void plc_reset(silk_channel_t *ch)
{
    ch->plc.pitchL_Q8 = lsh(ch->frame_length, 7);
    ch->plc.prevGain_Q16[0] = ch->plc.prevGain_Q16[1] = 65536;
    ch->plc.subfr_length = 20;
    ch->plc.nb_subfr = 2;
}

static void channel_init(silk_channel_t *ch)
{
    memset(ch, 0, sizeof *ch);
    ch->first_frame_after_reset = 1;
    ch->prev_gain_Q16 = 65536;
    cng_reset(ch);
    plc_reset(ch);
}

static void set_fs(silk_channel_t *ch, int fs_kHz)
{
    int frame_length;

    ch->subfr_length = 5 * fs_kHz;
    frame_length = ch->nb_subfr * ch->subfr_length;
    if (ch->fs_kHz != fs_kHz || ch->fs_api_hz != 48000) {
        resampler_init(&ch->resampler, fs_kHz * 1000);
        ch->fs_api_hz = 48000;
    }
    if (ch->fs_kHz != fs_kHz || frame_length != ch->frame_length) {
        if (fs_kHz == 8)
            ch->pitch_contour_iCDF = ch->nb_subfr == MAX_NB_SUBFR ? k_pitch_contour_NB_iCDF : k_pitch_contour_10_ms_NB_iCDF;
        else
            ch->pitch_contour_iCDF = ch->nb_subfr == MAX_NB_SUBFR ? k_pitch_contour_iCDF : k_pitch_contour_10_ms_iCDF;
        if (ch->fs_kHz != fs_kHz) {
            ch->ltp_mem_length = 20 * fs_kHz;
            ch->lpc_order = fs_kHz == 16 ? 16 : 10;
            ch->wb = fs_kHz == 16;
            ch->pitch_lag_low_bits_iCDF = fs_kHz == 16 ? k_uniform8_iCDF : fs_kHz == 12 ? k_uniform6_iCDF : k_uniform4_iCDF;
            ch->first_frame_after_reset = 1;
            ch->lag_prev = 100;
            ch->last_gain_index = 10;
            ch->prev_signal_type = TYPE_NO_VOICE;
            memset(ch->out_buf, 0, sizeof ch->out_buf);
            memset(ch->sLPC_Q14_buf, 0, sizeof ch->sLPC_Q14_buf);
        }
        ch->fs_kHz = fs_kHz;
        ch->frame_length = frame_length;
    }
}

/* ---- Side information ---- */

static void nlsf_unpack(short *ec_ix, unsigned char *pred_Q8, const nlsf_cb_t *cb, int cb1_index)
{
    const unsigned char *sel = &cb->ec_sel[cb1_index * cb->order / 2];

    for (int i = 0; i < cb->order; i += 2) {
        unsigned char entry = *sel++;
        ec_ix[i] = (short)smulbb((entry >> 1) & 7, 2 * NLSF_QUANT_MAX_AMPLITUDE + 1);
        pred_Q8[i] = cb->pred_Q8[i + (entry & 1) * (cb->order - 1)];
        ec_ix[i + 1] = (short)smulbb((entry >> 5) & 7, 2 * NLSF_QUANT_MAX_AMPLITUDE + 1);
        pred_Q8[i + 1] = cb->pred_Q8[i + ((entry >> 4) & 1) * (cb->order - 1) + 1];
    }
}

static void decode_indices(silk_channel_t *ch, opus_rc_t *rc, int frame, int decode_lbrr, int cond)
{
    silk_indices_t *ix = &ch->idx;
    const nlsf_cb_t *cb = nlsf_cb(ch);
    short ec_ix[SILK_MAX_ORDER];
    unsigned char pred_Q8[SILK_MAX_ORDER];
    int v;

    if (decode_lbrr || ch->vad_flags[frame])
        v = rc_icdf(rc, k_type_offset_VAD_iCDF, 8) + 2;
    else
        v = rc_icdf(rc, k_type_offset_no_VAD_iCDF, 8);
    ix->signal_type = (signed char)(v >> 1);
    ix->quant_offset_type = (signed char)(v & 1);

    if (cond == CODE_CONDITIONALLY) {
        ix->gains[0] = (signed char)rc_icdf(rc, k_delta_gain_iCDF, 8);
    } else {
        ix->gains[0] = (signed char)lsh(rc_icdf(rc, k_gain_iCDF[ix->signal_type], 8), 3);
        ix->gains[0] = (signed char)(ix->gains[0] + rc_icdf(rc, k_uniform8_iCDF, 8));
    }
    for (int i = 1; i < ch->nb_subfr; i++)
        ix->gains[i] = (signed char)rc_icdf(rc, k_delta_gain_iCDF, 8);

    ix->nlsf[0] = (signed char)rc_icdf(rc, &cb->cb1_iCDF[(ix->signal_type >> 1) * 32], 8);
    nlsf_unpack(ec_ix, pred_Q8, cb, ix->nlsf[0]);
    for (int i = 0; i < cb->order; i++) {
        v = rc_icdf(rc, &cb->ec_iCDF[ec_ix[i]], 8);
        if (v == 0)
            v -= rc_icdf(rc, k_NLSF_EXT_iCDF, 8);
        else if (v == 2 * NLSF_QUANT_MAX_AMPLITUDE)
            v += rc_icdf(rc, k_NLSF_EXT_iCDF, 8);
        ix->nlsf[i + 1] = (signed char)(v - NLSF_QUANT_MAX_AMPLITUDE);
    }
    ix->nlsf_interp_Q2 = ch->nb_subfr == MAX_NB_SUBFR ? (signed char)rc_icdf(rc, k_NLSF_interpolation_factor_iCDF, 8) : 4;

    if (ix->signal_type == TYPE_VOICED) {
        int absolute = 1;
        if (cond == CODE_CONDITIONALLY && ch->ec_prev_signal_type == TYPE_VOICED) {
            int delta = rc_icdf(rc, k_pitch_delta_iCDF, 8);
            if (delta > 0) {
                ix->lag_index = (short)(ch->ec_prev_lag_index + delta - 9);
                absolute = 0;
            }
        }
        if (absolute) {
            ix->lag_index = (short)(rc_icdf(rc, k_pitch_lag_iCDF, 8) * (ch->fs_kHz >> 1));
            ix->lag_index = (short)(ix->lag_index + rc_icdf(rc, ch->pitch_lag_low_bits_iCDF, 8));
        }
        ch->ec_prev_lag_index = ix->lag_index;
        ix->contour_index = (signed char)rc_icdf(rc, ch->pitch_contour_iCDF, 8);
        ix->per_index = (signed char)rc_icdf(rc, k_LTP_per_index_iCDF, 8);
        for (int k = 0; k < ch->nb_subfr; k++)
            ix->ltp_index[k] = (signed char)rc_icdf(rc, ltp_gain_iCDF(ix->per_index), 8);
        ix->ltp_scale_index = cond == CODE_INDEPENDENTLY ? (signed char)rc_icdf(rc, k_LTPscale_iCDF, 8) : 0;
    }
    ch->ec_prev_signal_type = ix->signal_type;
    ix->seed = (signed char)rc_icdf(rc, k_uniform4_iCDF, 8);
}

/* ---- Excitation pulses ---- */

static void decode_split(int *c1, int *c2, opus_rc_t *rc, int p, const unsigned char *table)
{
    if (p > 0) {
        *c1 = rc_icdf(rc, &table[k_shell_code_table_offsets[p]], 8);
        *c2 = p - *c1;
    } else {
        *c1 = *c2 = 0;
    }
}

static void shell_decoder(int *p0, opus_rc_t *rc, int p4)
{
    int p3[2], p2[4], p1[8];

    decode_split(&p3[0], &p3[1], rc, p4, k_shell_code_table3);
    decode_split(&p2[0], &p2[1], rc, p3[0], k_shell_code_table2);
    decode_split(&p1[0], &p1[1], rc, p2[0], k_shell_code_table1);
    decode_split(&p0[0], &p0[1], rc, p1[0], k_shell_code_table0);
    decode_split(&p0[2], &p0[3], rc, p1[1], k_shell_code_table0);
    decode_split(&p1[2], &p1[3], rc, p2[1], k_shell_code_table1);
    decode_split(&p0[4], &p0[5], rc, p1[2], k_shell_code_table0);
    decode_split(&p0[6], &p0[7], rc, p1[3], k_shell_code_table0);
    decode_split(&p2[2], &p2[3], rc, p3[1], k_shell_code_table2);
    decode_split(&p1[4], &p1[5], rc, p2[2], k_shell_code_table1);
    decode_split(&p0[8], &p0[9], rc, p1[4], k_shell_code_table0);
    decode_split(&p0[10], &p0[11], rc, p1[5], k_shell_code_table0);
    decode_split(&p1[6], &p1[7], rc, p2[3], k_shell_code_table1);
    decode_split(&p0[12], &p0[13], rc, p1[6], k_shell_code_table0);
    decode_split(&p0[14], &p0[15], rc, p1[7], k_shell_code_table0);
}

static void decode_pulses(opus_rc_t *rc, int *pulses, int signal_type, int quant_offset_type, int frame_length)
{
    int sum[SILK_MAX_FRAME / 16 + 1], nlshifts[SILK_MAX_FRAME / 16 + 1], iter, rate_level;
    const unsigned char *cdf;

    rate_level = rc_icdf(rc, k_rate_levels_iCDF[signal_type >> 1], 8);
    iter = frame_length >> 4;
    if (iter * 16 < frame_length)
        iter++; /* 10 ms at 12 kHz */
    cdf = k_pulses_per_block_iCDF[rate_level];
    for (int i = 0; i < iter; i++) {
        nlshifts[i] = 0;
        sum[i] = rc_icdf(rc, cdf, 8);
        while (sum[i] == MAX_PULSES + 1) {
            nlshifts[i]++;
            sum[i] = rc_icdf(rc, k_pulses_per_block_iCDF[9] + (nlshifts[i] == 10), 8);
        }
    }
    for (int i = 0; i < iter; i++) {
        if (sum[i] > 0)
            shell_decoder(&pulses[i * 16], rc, sum[i]);
        else
            memset(&pulses[i * 16], 0, 16 * sizeof(int));
    }
    for (int i = 0; i < iter; i++) {
        if (nlshifts[i] > 0) {
            int *p = &pulses[i * 16];
            for (int k = 0; k < 16; k++) {
                int q = p[k];
                for (int j = 0; j < nlshifts[i]; j++)
                    q = lsh(q, 1) + rc_icdf(rc, k_lsb_iCDF, 8);
                p[k] = q;
            }
            sum[i] |= nlshifts[i] << 5;
        }
    }
    /* Signs */
    {
        unsigned char icdf[2] = {0, 0};
        const unsigned char *icdf_ptr = &k_sign_iCDF[7 * (quant_offset_type + (signal_type << 1))];
        int *q = pulses, blocks = (frame_length + 8) >> 4;
        for (int i = 0; i < blocks; i++) {
            if (sum[i] > 0) {
                icdf[0] = icdf_ptr[imin(sum[i] & 0x1F, 6)];
                for (int j = 0; j < 16; j++)
                    if (q[j] > 0)
                        q[j] *= (rc_icdf(rc, icdf, 8) << 1) - 1;
            }
            q += 16;
        }
    }
}

/* ---- Parameters ---- */

typedef struct {
    int pitchL[MAX_NB_SUBFR];
    int gains_Q16[MAX_NB_SUBFR];
    short pred_coef_Q12[2][SILK_MAX_ORDER];
    short ltp_coef_Q14[LTP_ORDER * MAX_NB_SUBFR];
    int ltp_scale_Q14;
} silk_ctrl_t;

static void gains_dequant(int *gain_Q16, const signed char *ind, signed char *prev_ind, int conditional, int nb_subfr)
{
    for (int k = 0; k < nb_subfr; k++) {
        int p = *prev_ind;
        if (k == 0 && conditional == 0) {
            p = imax(ind[k], p - 16);
        } else {
            int tmp = ind[k] + MIN_DELTA_GAIN_QUANT, threshold = 2 * MAX_DELTA_GAIN_QUANT - N_LEVELS_QGAIN + p;
            if (tmp > threshold)
                p += lsh(tmp, 1) - threshold;
            else
                p += tmp;
        }
        p = limit(p, 0, N_LEVELS_QGAIN - 1);
        *prev_ind = (signed char)p;
        gain_Q16[k] = log2lin(imin(smulwb(GAIN_INV_SCALE_Q16, p) + GAIN_OFFSET, 3967));
    }
}

static void nlsf_stabilize(short *nlsf, const short *delta_min, int L)
{
    int loops, i, k, idx = 0;

    for (loops = 0; loops < 20; loops++) {
        int min_diff = nlsf[0] - delta_min[0], diff;
        idx = 0;
        for (i = 1; i <= L - 1; i++) {
            diff = nlsf[i] - (nlsf[i - 1] + delta_min[i]);
            if (diff < min_diff) {
                min_diff = diff;
                idx = i;
            }
        }
        diff = (1 << 15) - (nlsf[L - 1] + delta_min[L]);
        if (diff < min_diff) {
            min_diff = diff;
            idx = L;
        }
        if (min_diff >= 0)
            return;
        if (idx == 0) {
            nlsf[0] = delta_min[0];
        } else if (idx == L) {
            nlsf[L - 1] = (short)((1 << 15) - delta_min[L]);
        } else {
            int min_center = 0, max_center = 1 << 15, center;
            for (k = 0; k < idx; k++)
                min_center += delta_min[k];
            min_center += delta_min[idx] >> 1;
            for (k = L; k > idx; k--)
                max_center -= delta_min[k];
            max_center -= delta_min[idx] >> 1;
            center = (short)limit(rshift_round(nlsf[idx - 1] + nlsf[idx], 1), min_center, max_center);
            nlsf[idx - 1] = (short)(center - (delta_min[idx] >> 1));
            nlsf[idx] = (short)(nlsf[idx - 1] + delta_min[idx]);
        }
    }
    /* Fallback: sort, then enforce the distances both ways (RFC 8251 saturates the sum). */
    for (i = 1; i < L; i++) {
        int value = nlsf[i], j;
        for (j = i - 1; j >= 0 && value < nlsf[j]; j--)
            nlsf[j + 1] = nlsf[j];
        nlsf[j + 1] = (short)value;
    }
    nlsf[0] = (short)imax(nlsf[0], delta_min[0]);
    for (i = 1; i < L; i++)
        nlsf[i] = (short)imax(nlsf[i], add_sat16(nlsf[i - 1], delta_min[i]));
    nlsf[L - 1] = (short)imin(nlsf[L - 1], (1 << 15) - delta_min[L]);
    for (i = L - 2; i >= 0; i--)
        nlsf[i] = (short)imin(nlsf[i], nlsf[i + 1] - delta_min[i + 1]);
}

static void nlsf_decode(short *nlsf, const signed char *indices, const nlsf_cb_t *cb)
{
    unsigned char pred_Q8[SILK_MAX_ORDER];
    short ec_ix[SILK_MAX_ORDER], res_Q10[SILK_MAX_ORDER], w[SILK_MAX_ORDER];
    const unsigned char *cb1 = &cb->cb1_Q8[indices[0] * cb->order];
    int out_Q10 = 0, d = cb->order, t1, t2;

    for (int i = 0; i < d; i++)
        nlsf[i] = (short)lsh(cb1[i], 7);
    nlsf_unpack(ec_ix, pred_Q8, cb, indices[0]);
    /* Predictive residual dequantization, backwards. */
    for (int i = d - 1; i >= 0; i--) {
        int pred_Q10 = smulbb(out_Q10, pred_Q8[i]) >> 8;
        out_Q10 = lsh(indices[i + 1], 10);
        if (out_Q10 > 0)
            out_Q10 -= 102;
        else if (out_Q10 < 0)
            out_Q10 += 102;
        out_Q10 = smlawb(pred_Q10, out_Q10, cb->quant_step_Q16);
        res_Q10[i] = (short)out_Q10;
    }
    /* Laroia weights of the codebook vector. */
    t1 = (1 << 17) / imax(nlsf[0], 1);
    t2 = (1 << 17) / imax(nlsf[1] - nlsf[0], 1);
    w[0] = (short)imin(t1 + t2, 32767);
    for (int k = 1; k < d - 1; k += 2) {
        t1 = (1 << 17) / imax(nlsf[k + 1] - nlsf[k], 1);
        w[k] = (short)imin(t1 + t2, 32767);
        t2 = (1 << 17) / imax(nlsf[k + 2] - nlsf[k + 1], 1);
        w[k + 1] = (short)imin(t1 + t2, 32767);
    }
    t1 = (1 << 17) / imax((1 << 15) - nlsf[d - 1], 1);
    w[d - 1] = (short)imin(t1 + t2, 32767);
    for (int i = 0; i < d; i++) {
        int w_Q9 = sqrt_approx(lsh(w[i], 16));
        nlsf[i] = (short)limit(nlsf[i] + lsh(res_Q10[i], 14) / w_Q9, 0, 32767);
    }
    nlsf_stabilize(nlsf, cb->delta_min_Q15, d);
}

static void bwexpander(short *ar, int d, int chirp_Q16)
{
    int chirp_minus_one = chirp_Q16 - 65536;

    for (int i = 0; i < d - 1; i++) {
        ar[i] = (short)rshift_round(chirp_Q16 * ar[i], 16);
        chirp_Q16 += rshift_round(chirp_Q16 * chirp_minus_one, 16);
    }
    ar[d - 1] = (short)rshift_round(chirp_Q16 * ar[d - 1], 16);
}

static void bwexpander_32(int *ar, int d, int chirp_Q16)
{
    int chirp_minus_one = chirp_Q16 - 65536;

    for (int i = 0; i < d - 1; i++) {
        ar[i] = smulww(chirp_Q16, ar[i]);
        chirp_Q16 += rshift_round(chirp_Q16 * chirp_minus_one, 16);
    }
    ar[d - 1] = smulww(chirp_Q16, ar[d - 1]);
}

/* Inverse prediction gain in Q30, or 0 when the filter is unstable. */
static int lpc_inverse_pred_gain(const short *A_Q12, int order)
{
    enum { QA = 24 };
    const int a_limit = 16773022; /* 0.99975 in Q24 */
    int A[2][SILK_MAX_ORDER], *anew = A[order & 1], *aold, dc = 0, inv_gain = 1 << 30, rc_Q31, mult1;

    for (int k = 0; k < order; k++) {
        dc += A_Q12[k];
        anew[k] = lsh(A_Q12[k], QA - 12);
    }
    if (dc >= 4096)
        return 0;
    for (int k = order - 1; k > 0; k--) {
        int mult2Q, mult2;
        if (anew[k] > a_limit || anew[k] < -a_limit)
            return 0;
        rc_Q31 = -lsh(anew[k], 31 - QA);
        mult1 = (1 << 30) - smmul(rc_Q31, rc_Q31);
        mult2Q = 32 - clz32(iabs(mult1));
        mult2 = inverse32_varQ(mult1, mult2Q + 30);
        inv_gain = lsh(smmul(inv_gain, mult1), 2);
        aold = anew;
        anew = A[k & 1];
        for (int n = 0; n < k; n++) {
            long long t64;
            int tmp = sub_sat32(aold[n], (int)rshift_round64((long long)aold[k - n - 1] * rc_Q31, 31));
            t64 = rshift_round64((long long)tmp * mult2, mult2Q);
            if (t64 > INT32_MAX_ || t64 < INT32_MIN_) /* RFC 8251 */
                return 0;
            anew[n] = (int)t64;
        }
    }
    if (anew[0] > a_limit || anew[0] < -a_limit)
        return 0;
    rc_Q31 = -lsh(anew[0], 31 - QA);
    mult1 = (1 << 30) - smmul(rc_Q31, rc_Q31);
    return lsh(smmul(inv_gain, mult1), 2);
}

static void nlsf2a_find_poly(int *out, const int *c, int dd)
{
    out[0] = 1 << 16;
    out[1] = -c[0];
    for (int k = 1; k < dd; k++) {
        int f = c[2 * k];
        out[k + 1] = lsh(out[k - 1], 1) - (int)rshift_round64((long long)f * out[k], 16);
        for (int n = k; n > 1; n--)
            out[n] += out[n - 2] - (int)rshift_round64((long long)f * out[n - 1], 16);
        out[1] -= f;
    }
}

static void nlsf2a(short *a_Q12, const short *nlsf, int d)
{
    static const unsigned char order16[16] = {0, 15, 8, 7, 4, 11, 12, 3, 2, 13, 10, 5, 6, 9, 14, 1};
    static const unsigned char order10[10] = {0, 9, 6, 3, 4, 5, 8, 1, 2, 7};
    const unsigned char *ordering = d == 16 ? order16 : order10;
    int cos_QA[SILK_MAX_ORDER], P[SILK_MAX_ORDER / 2 + 1], Q[SILK_MAX_ORDER / 2 + 1], a32[SILK_MAX_ORDER];
    int dd = d >> 1, i, k, idx = 0;

    for (k = 0; k < d; k++) {
        int f_int = nlsf[k] >> 8, f_frac = nlsf[k] - lsh(f_int, 8);
        int cos_val = k_LSFCosTab_FIX_Q12[f_int], delta = k_LSFCosTab_FIX_Q12[f_int + 1] - cos_val;
        cos_QA[ordering[k]] = rshift_round(lsh(cos_val, 8) + delta * f_frac, 4);
    }
    nlsf2a_find_poly(P, &cos_QA[0], dd);
    nlsf2a_find_poly(Q, &cos_QA[1], dd);
    for (k = 0; k < dd; k++) {
        int ptmp = P[k + 1] + P[k], qtmp = Q[k + 1] - Q[k];
        a32[k] = -qtmp - ptmp;
        a32[d - k - 1] = qtmp - ptmp;
    }
    for (i = 0; i < 10; i++) {
        int maxabs = 0;
        for (k = 0; k < d; k++)
            if (iabs(a32[k]) > maxabs) {
                maxabs = iabs(a32[k]);
                idx = k;
            }
        maxabs = rshift_round(maxabs, 5);
        if (maxabs > 32767) {
            int sc_Q16;
            maxabs = imin(maxabs, 163838);
            sc_Q16 = 65470 - lsh(maxabs - 32767, 14) / ((maxabs * (idx + 1)) >> 2);
            bwexpander_32(a32, d, sc_Q16);
        } else {
            break;
        }
    }
    if (i == 10) {
        for (k = 0; k < d; k++) {
            a_Q12[k] = (short)sat16(rshift_round(a32[k], 5));
            a32[k] = lsh(a_Q12[k], 5);
        }
    } else {
        for (k = 0; k < d; k++)
            a_Q12[k] = (short)rshift_round(a32[k], 5);
    }
    for (i = 0; i < 16; i++) {
        if (lpc_inverse_pred_gain(a_Q12, d) < 107374) { /* 1 / MAX_PREDICTION_POWER_GAIN in Q30 */
            bwexpander_32(a32, d, 65536 - lsh(2, i));
            for (k = 0; k < d; k++)
                a_Q12[k] = (short)rshift_round(a32[k], 5);
        } else {
            break;
        }
    }
}

static void decode_pitch(int lag_index, int contour, int *lags, int fs_kHz, int nb_subfr)
{
    int min_lag = 2 * fs_kHz, max_lag = 18 * fs_kHz, lag = min_lag + lag_index;

    for (int k = 0; k < nb_subfr; k++) {
        int d;
        if (fs_kHz == 8)
            d = nb_subfr == MAX_NB_SUBFR ? k_CB_lags_stage2[k][contour] : k_CB_lags_stage2_10_ms[k][contour];
        else
            d = nb_subfr == MAX_NB_SUBFR ? k_CB_lags_stage3[k][contour] : k_CB_lags_stage3_10_ms[k][contour];
        lags[k] = limit(lag + d, min_lag, max_lag);
    }
}

static void decode_parameters(silk_channel_t *ch, silk_ctrl_t *ctrl, int cond)
{
    silk_indices_t *ix = &ch->idx;
    short nlsf[SILK_MAX_ORDER], nlsf0[SILK_MAX_ORDER];

    gains_dequant(ctrl->gains_Q16, ix->gains, &ch->last_gain_index, cond == CODE_CONDITIONALLY, ch->nb_subfr);
    nlsf_decode(nlsf, ix->nlsf, nlsf_cb(ch));
    nlsf2a(ctrl->pred_coef_Q12[1], nlsf, ch->lpc_order);
    if (ch->first_frame_after_reset == 1)
        ix->nlsf_interp_Q2 = 4;
    if (ix->nlsf_interp_Q2 < 4) {
        for (int i = 0; i < ch->lpc_order; i++)
            nlsf0[i] = (short)(ch->prev_NLSF_Q15[i] + ((ix->nlsf_interp_Q2 * (nlsf[i] - ch->prev_NLSF_Q15[i])) >> 2));
        nlsf2a(ctrl->pred_coef_Q12[0], nlsf0, ch->lpc_order);
    } else {
        memcpy(ctrl->pred_coef_Q12[0], ctrl->pred_coef_Q12[1], sizeof(short) * (size_t)ch->lpc_order);
    }
    memcpy(ch->prev_NLSF_Q15, nlsf, sizeof(short) * (size_t)ch->lpc_order);
    if (ch->loss_cnt) {
        bwexpander(ctrl->pred_coef_Q12[0], ch->lpc_order, BWE_AFTER_LOSS_Q16);
        bwexpander(ctrl->pred_coef_Q12[1], ch->lpc_order, BWE_AFTER_LOSS_Q16);
    }
    if (ix->signal_type == TYPE_VOICED) {
        decode_pitch(ix->lag_index, ix->contour_index, ctrl->pitchL, ch->fs_kHz, ch->nb_subfr);
        for (int k = 0; k < ch->nb_subfr; k++) {
            const signed char *cb = ltp_vq(ix->per_index, ix->ltp_index[k]);
            for (int i = 0; i < LTP_ORDER; i++)
                ctrl->ltp_coef_Q14[k * LTP_ORDER + i] = (short)lsh(cb[i], 7);
        }
        ctrl->ltp_scale_Q14 = k_LTPScales_table_Q14[ix->ltp_scale_index];
    } else {
        memset(ctrl->pitchL, 0, sizeof ctrl->pitchL);
        memset(ctrl->ltp_coef_Q14, 0, sizeof ctrl->ltp_coef_Q14);
        ix->per_index = 0;
        ctrl->ltp_scale_Q14 = 0;
    }
}

/* ---- Synthesis ---- */

static void lpc_analysis_filter(short *out, const short *in, const short *B, int len, int d)
{
    for (int ix = d; ix < len; ix++) {
        const short *p = &in[ix - 1];
        int o = smulbb(p[0], B[0]);
        for (int j = 1; j < d; j++)
            o = smlabb_ovflw(o, p[-j], B[j]);
        o = (int)((unsigned)lsh(p[1], 12) - (unsigned)o);
        out[ix] = (short)sat16(rshift_round(o, 12));
    }
    memset(out, 0, sizeof(short) * (size_t)d);
}

static int lpc_predict(const int *s, const short *A, int order)
{
    int pred = order >> 1;

    for (int j = 0; j < order; j++)
        pred = smlawb(pred, s[-1 - j], A[j]);
    return pred;
}

static void decode_core(silk_channel_t *ch, silk_ctrl_t *ctrl, short *xq, const int *pulses, silk_scratch_t *tmp)
{
    silk_indices_t *ix = &ch->idx;
    short *sLTP = tmp->sLTP;
    int *sLTP_Q15 = tmp->sLTP_Q15, *sLPC_Q14 = tmp->sLPC_Q14, *res_Q14 = tmp->res_Q14;
    int offset_Q10 = k_Quantization_Offsets_Q10[ix->signal_type >> 1][ix->quant_offset_type];
    int interp_flag = ix->nlsf_interp_Q2 < 4, seed = ix->seed, lag = 0, ltp_idx;
    const int *pexc;
    short *pxq = xq;

    for (int i = 0; i < ch->frame_length; i++) {
        int e;
        seed = rand_next(seed);
        e = lsh(pulses[i], 14);
        if (e > 0)
            e -= QUANT_LEVEL_ADJUST_Q10 << 4;
        else if (e < 0)
            e += QUANT_LEVEL_ADJUST_Q10 << 4;
        e += offset_Q10 << 4;
        if (seed < 0)
            e = -e;
        ch->exc_Q14[i] = e;
        seed = (int)((unsigned)seed + (unsigned)pulses[i]);
    }
    memcpy(sLPC_Q14, ch->sLPC_Q14_buf, sizeof ch->sLPC_Q14_buf);
    pexc = ch->exc_Q14;
    ltp_idx = ch->ltp_mem_length;
    for (int k = 0; k < ch->nb_subfr; k++) {
        const short *A = ctrl->pred_coef_Q12[k >> 1];
        short *B = &ctrl->ltp_coef_Q14[k * LTP_ORDER];
        int signal_type = ix->signal_type, gain_Q10 = ctrl->gains_Q16[k] >> 6;
        int inv_gain_Q31 = inverse32_varQ(ctrl->gains_Q16[k], 47), gain_adj_Q16;
        const int *pres;

        if (ctrl->gains_Q16[k] != ch->prev_gain_Q16) {
            gain_adj_Q16 = div32_varQ(ch->prev_gain_Q16, ctrl->gains_Q16[k], 16);
            for (int i = 0; i < SILK_MAX_ORDER; i++)
                sLPC_Q14[i] = smulww(gain_adj_Q16, sLPC_Q14[i]);
        } else {
            gain_adj_Q16 = 1 << 16;
        }
        ch->prev_gain_Q16 = ctrl->gains_Q16[k];

        /* No abrupt switch from voiced concealment to unvoiced decoding. */
        if (ch->loss_cnt && ch->prev_signal_type == TYPE_VOICED && ix->signal_type != TYPE_VOICED &&
            k < MAX_NB_SUBFR / 2) {
            memset(B, 0, LTP_ORDER * sizeof(short));
            B[LTP_ORDER / 2] = 4096; /* 0.25 in Q14 */
            signal_type = TYPE_VOICED;
            ctrl->pitchL[k] = ch->lag_prev;
        }
        if (signal_type == TYPE_VOICED) {
            lag = ctrl->pitchL[k];
            if (k == 0 || (k == 2 && interp_flag)) {
                /* Re-whiten the past output with this subframe's filter. */
                int start = ch->ltp_mem_length - lag - ch->lpc_order - LTP_ORDER / 2;
                if (k == 2)
                    memcpy(&ch->out_buf[ch->ltp_mem_length], xq, 2 * (size_t)ch->subfr_length * sizeof(short));
                lpc_analysis_filter(&sLTP[start], &ch->out_buf[start + k * ch->subfr_length], A,
                                    ch->ltp_mem_length - start, ch->lpc_order);
                if (k == 0)
                    inv_gain_Q31 = lsh(smulwb(inv_gain_Q31, ctrl->ltp_scale_Q14), 2);
                for (int i = 0; i < lag + LTP_ORDER / 2; i++)
                    sLTP_Q15[ltp_idx - i - 1] = smulwb(inv_gain_Q31, sLTP[ch->ltp_mem_length - i - 1]);
            } else if (gain_adj_Q16 != 1 << 16) {
                for (int i = 0; i < lag + LTP_ORDER / 2; i++)
                    sLTP_Q15[ltp_idx - i - 1] = smulww(gain_adj_Q16, sLTP_Q15[ltp_idx - i - 1]);
            }
            {
                const int *pred = &sLTP_Q15[ltp_idx - lag + LTP_ORDER / 2];
                for (int i = 0; i < ch->subfr_length; i++) {
                    int p = 2;
                    p = smlawb(p, pred[0], B[0]);
                    p = smlawb(p, pred[-1], B[1]);
                    p = smlawb(p, pred[-2], B[2]);
                    p = smlawb(p, pred[-3], B[3]);
                    p = smlawb(p, pred[-4], B[4]);
                    pred++;
                    res_Q14[i] = pexc[i] + lsh(p, 1);
                    sLTP_Q15[ltp_idx++] = lsh(res_Q14[i], 1);
                }
            }
            pres = res_Q14;
        } else {
            pres = pexc;
        }
        for (int i = 0; i < ch->subfr_length; i++) {
            int pred = lpc_predict(&sLPC_Q14[SILK_MAX_ORDER + i], A, ch->lpc_order);
            sLPC_Q14[SILK_MAX_ORDER + i] = pres[i] + lsh(pred, 4);
            pxq[i] = (short)sat16(rshift_round(smulww(sLPC_Q14[SILK_MAX_ORDER + i], gain_Q10), 8));
        }
        memmove(sLPC_Q14, &sLPC_Q14[ch->subfr_length], SILK_MAX_ORDER * sizeof(int));
        pexc += ch->subfr_length;
        pxq += ch->subfr_length;
    }
    memcpy(ch->sLPC_Q14_buf, sLPC_Q14, sizeof ch->sLPC_Q14_buf);
}

/* ---- Loss concealment and comfort noise ---- */

static void plc_update(silk_channel_t *ch, const silk_ctrl_t *ctrl)
{
    silk_plc_t *plc = &ch->plc;
    int ltp_gain = 0;

    ch->prev_signal_type = ch->idx.signal_type;
    if (ch->idx.signal_type == TYPE_VOICED) {
        for (int j = 0; j * ch->subfr_length < ctrl->pitchL[ch->nb_subfr - 1] && j != ch->nb_subfr; j++) {
            int t = 0;
            for (int i = 0; i < LTP_ORDER; i++)
                t += ctrl->ltp_coef_Q14[(ch->nb_subfr - 1 - j) * LTP_ORDER + i];
            if (t > ltp_gain) {
                ltp_gain = t;
                memcpy(plc->LTPCoef_Q14, &ctrl->ltp_coef_Q14[(ch->nb_subfr - 1 - j) * LTP_ORDER], sizeof plc->LTPCoef_Q14);
                plc->pitchL_Q8 = lsh(ctrl->pitchL[ch->nb_subfr - 1 - j], 8);
            }
        }
        memset(plc->LTPCoef_Q14, 0, sizeof plc->LTPCoef_Q14);
        plc->LTPCoef_Q14[LTP_ORDER / 2] = (short)ltp_gain;
        if (ltp_gain < 11469) {
            int scale_Q10 = lsh(11469, 10) / imax(ltp_gain, 1);
            for (int i = 0; i < LTP_ORDER; i++)
                plc->LTPCoef_Q14[i] = (short)(smulbb(plc->LTPCoef_Q14[i], scale_Q10) >> 10);
        } else if (ltp_gain > 15565) {
            int scale_Q14 = lsh(15565, 14) / imax(ltp_gain, 1);
            for (int i = 0; i < LTP_ORDER; i++)
                plc->LTPCoef_Q14[i] = (short)(smulbb(plc->LTPCoef_Q14[i], scale_Q14) >> 14);
        }
    } else {
        plc->pitchL_Q8 = lsh(smulbb(ch->fs_kHz, 18), 8);
        memset(plc->LTPCoef_Q14, 0, sizeof plc->LTPCoef_Q14);
    }
    memcpy(plc->prevLPC_Q12, ctrl->pred_coef_Q12[1], sizeof(short) * (size_t)ch->lpc_order);
    plc->prevLTP_scale_Q14 = (short)ctrl->ltp_scale_Q14;
    memcpy(plc->prevGain_Q16, &ctrl->gains_Q16[ch->nb_subfr - 2], 2 * sizeof(int));
    plc->subfr_length = ch->subfr_length;
    plc->nb_subfr = ch->nb_subfr;
}

static void plc_conceal(silk_channel_t *ch, silk_ctrl_t *ctrl, short *frame, silk_scratch_t *tmp)
{
    static const short harm_att[2] = {32440, 31130}, rand_att_v[2] = {31130, 26214}, rand_att_uv[2] = {32440, 29491};
    silk_plc_t *plc = &ch->plc;
    short exc_buf[2 * SILK_MAX_SUBFR], A_Q12[SILK_MAX_ORDER], *sLTP = tmp->sLTP, *B = plc->LTPCoef_Q14;
    int *sLTP_Q14 = tmp->sLTP_Q15, *rand_ptr, *sLPC;
    int prev_gain_Q10[2] = {plc->prevGain_Q16[0] >> 6, plc->prevGain_Q16[1] >> 6};
    int e1, e2, s1, s2, harm_gain, rand_gain, rand_seed, lag, idx, ltp_idx, inv_gain_Q30;
    short rand_scale_Q14 = plc->randScale_Q14;
    int att = imin(1, ch->loss_cnt);

    if (ch->first_frame_after_reset)
        memset(plc->prevLPC_Q12, 0, sizeof plc->prevLPC_Q12);
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < plc->subfr_length; i++)
            exc_buf[k * plc->subfr_length + i] = (short)sat16(
                smulww(ch->exc_Q14[i + (k + plc->nb_subfr - 2) * plc->subfr_length], prev_gain_Q10[k]) >> 8);
    sum_sqr_shift(&e1, &s1, exc_buf, plc->subfr_length);
    sum_sqr_shift(&e2, &s2, &exc_buf[plc->subfr_length], plc->subfr_length);
    if (e1 >> s2 < e2 >> s1)
        rand_ptr = &ch->exc_Q14[imax(0, (plc->nb_subfr - 1) * plc->subfr_length - 128)];
    else
        rand_ptr = &ch->exc_Q14[imax(0, plc->nb_subfr * plc->subfr_length - 128)];

    harm_gain = harm_att[att];
    rand_gain = ch->prev_signal_type == TYPE_VOICED ? rand_att_v[att] : rand_att_uv[att];
    bwexpander(plc->prevLPC_Q12, ch->lpc_order, 64881); /* 0.99 in Q16 */
    memcpy(A_Q12, plc->prevLPC_Q12, sizeof(short) * (size_t)ch->lpc_order);
    if (ch->loss_cnt == 0) {
        rand_scale_Q14 = 1 << 14;
        if (ch->prev_signal_type == TYPE_VOICED) {
            for (int i = 0; i < LTP_ORDER; i++)
                rand_scale_Q14 = (short)(rand_scale_Q14 - B[i]);
            rand_scale_Q14 = (short)imax(3277, rand_scale_Q14);
            rand_scale_Q14 = (short)(smulbb(rand_scale_Q14, plc->prevLTP_scale_Q14) >> 14);
        } else {
            int inv_gain = lpc_inverse_pred_gain(plc->prevLPC_Q12, ch->lpc_order), down;
            down = imin((1 << 30) >> 3, inv_gain);
            down = imax((1 << 30) >> 8, down);
            down = lsh(down, 3);
            rand_gain = smulwb(down, rand_gain) >> 14;
        }
    }
    rand_seed = plc->rand_seed;
    lag = rshift_round(plc->pitchL_Q8, 8);
    ltp_idx = ch->ltp_mem_length;
    idx = ch->ltp_mem_length - lag - ch->lpc_order - LTP_ORDER / 2;
    lpc_analysis_filter(&sLTP[idx], &ch->out_buf[idx], A_Q12, ch->ltp_mem_length - idx, ch->lpc_order);
    inv_gain_Q30 = imin(inverse32_varQ(plc->prevGain_Q16[1], 46), INT32_MAX_ >> 1);
    for (int i = idx + ch->lpc_order; i < ch->ltp_mem_length; i++)
        sLTP_Q14[i] = smulwb(inv_gain_Q30, sLTP[i]);

    for (int k = 0; k < ch->nb_subfr; k++) {
        const int *pred = &sLTP_Q14[ltp_idx - lag + LTP_ORDER / 2];
        for (int i = 0; i < ch->subfr_length; i++) {
            int p = 2;
            p = smlawb(p, pred[0], B[0]);
            p = smlawb(p, pred[-1], B[1]);
            p = smlawb(p, pred[-2], B[2]);
            p = smlawb(p, pred[-3], B[3]);
            p = smlawb(p, pred[-4], B[4]);
            pred++;
            rand_seed = rand_next(rand_seed);
            idx = (rand_seed >> 25) & 127;
            sLTP_Q14[ltp_idx++] = lsh(smlawb(p, rand_ptr[idx], rand_scale_Q14), 2);
        }
        for (int j = 0; j < LTP_ORDER; j++)
            B[j] = (short)(smulbb(harm_gain, B[j]) >> 15);
        rand_scale_Q14 = (short)(smulbb(rand_scale_Q14, rand_gain) >> 15);
        plc->pitchL_Q8 = smlawb(plc->pitchL_Q8, plc->pitchL_Q8, 655);
        plc->pitchL_Q8 = imin(plc->pitchL_Q8, lsh(smulbb(18, ch->fs_kHz), 8));
        lag = rshift_round(plc->pitchL_Q8, 8);
    }

    sLPC = &sLTP_Q14[ch->ltp_mem_length - SILK_MAX_ORDER];
    memcpy(sLPC, ch->sLPC_Q14_buf, sizeof ch->sLPC_Q14_buf);
    for (int i = 0; i < ch->frame_length; i++) {
        int pred = lpc_predict(&sLPC[SILK_MAX_ORDER + i], A_Q12, ch->lpc_order);
        sLPC[SILK_MAX_ORDER + i] += lsh(pred, 4);
        frame[i] = (short)sat16(rshift_round(smulww(sLPC[SILK_MAX_ORDER + i], prev_gain_Q10[1]), 8));
    }
    memcpy(ch->sLPC_Q14_buf, &sLPC[ch->frame_length], sizeof ch->sLPC_Q14_buf);
    plc->rand_seed = rand_seed;
    plc->randScale_Q14 = rand_scale_Q14;
    for (int i = 0; i < MAX_NB_SUBFR; i++)
        ctrl->pitchL[i] = lag;
}

static void plc(silk_channel_t *ch, silk_ctrl_t *ctrl, short *frame, int lost, silk_scratch_t *tmp)
{
    if (ch->fs_kHz != ch->plc.fs_kHz) {
        plc_reset(ch);
        ch->plc.fs_kHz = ch->fs_kHz;
    }
    if (lost) {
        plc_conceal(ch, ctrl, frame, tmp);
        ch->loss_cnt++;
    } else {
        plc_update(ch, ctrl);
    }
}

static void plc_glue_frames(silk_channel_t *ch, short *frame, int length)
{
    silk_plc_t *p = &ch->plc;

    if (ch->loss_cnt) {
        sum_sqr_shift(&p->conc_energy, &p->conc_energy_shift, frame, length);
        p->last_frame_lost = 1;
        return;
    }
    if (p->last_frame_lost) {
        int energy, shift;
        sum_sqr_shift(&energy, &shift, frame, length);
        if (shift > p->conc_energy_shift)
            p->conc_energy >>= shift - p->conc_energy_shift;
        else if (shift < p->conc_energy_shift)
            energy >>= p->conc_energy_shift - shift;
        if (energy > p->conc_energy) {
            int lz = clz32(p->conc_energy) - 1, frac_Q24, gain_Q16, slope_Q16;
            p->conc_energy = lsh(p->conc_energy, lz);
            energy >>= imax(24 - lz, 0);
            frac_Q24 = p->conc_energy / imax(energy, 1);
            gain_Q16 = lsh(sqrt_approx(frac_Q24), 4);
            slope_Q16 = ((1 << 16) - gain_Q16) / length;
            slope_Q16 = lsh(slope_Q16, 2);
            for (int i = 0; i < length; i++) {
                frame[i] = (short)smulwb(gain_Q16, frame[i]);
                gain_Q16 += slope_Q16;
                if (gain_Q16 > 1 << 16)
                    break;
            }
        }
    }
    p->last_frame_lost = 0;
}

static void cng(silk_channel_t *ch, const silk_ctrl_t *ctrl, short *frame, int length, silk_scratch_t *tmp)
{
    silk_cng_t *c = &ch->cng;

    if (ch->fs_kHz != c->fs_kHz) {
        cng_reset(ch);
        c->fs_kHz = ch->fs_kHz;
    }
    if (ch->loss_cnt == 0 && ch->prev_signal_type == TYPE_NO_VOICE) {
        int max_gain = 0, subfr = 0;
        for (int i = 0; i < ch->lpc_order; i++)
            c->smth_NLSF_Q15[i] = (short)(c->smth_NLSF_Q15[i] + smulwb(ch->prev_NLSF_Q15[i] - c->smth_NLSF_Q15[i], 16348));
        for (int i = 0; i < ch->nb_subfr; i++)
            if (ctrl->gains_Q16[i] > max_gain) {
                max_gain = ctrl->gains_Q16[i];
                subfr = i;
            }
        memmove(&c->exc_buf_Q14[ch->subfr_length], c->exc_buf_Q14,
                (size_t)((ch->nb_subfr - 1) * ch->subfr_length) * sizeof(int));
        memcpy(c->exc_buf_Q14, &ch->exc_Q14[subfr * ch->subfr_length], (size_t)ch->subfr_length * sizeof(int));
        for (int i = 0; i < ch->nb_subfr; i++)
            c->smth_gain_Q16 += smulwb(ctrl->gains_Q16[i] - c->smth_gain_Q16, 4634);
    }
    if (ch->loss_cnt) {
        int *sig = tmp->cng_sig_Q10, mask = 255, seed = c->rand_seed;
        short A_Q12[SILK_MAX_ORDER];
        while (mask > length)
            mask >>= 1;
        for (int i = 0; i < length; i++) {
            seed = rand_next(seed);
            sig[SILK_MAX_ORDER + i] = (short)sat16(smulww(c->exc_buf_Q14[(seed >> 24) & mask], c->smth_gain_Q16 >> 4));
        }
        c->rand_seed = seed;
        nlsf2a(A_Q12, c->smth_NLSF_Q15, ch->lpc_order);
        memcpy(sig, c->synth_state, sizeof c->synth_state);
        for (int i = 0; i < length; i++) {
            int sum = lpc_predict(&sig[SILK_MAX_ORDER + i], A_Q12, ch->lpc_order);
            sig[SILK_MAX_ORDER + i] += lsh(sum, 4);
            frame[i] = (short)add_sat16(frame[i], rshift_round(sum, 6));
        }
        memcpy(c->synth_state, &sig[length], sizeof c->synth_state);
    } else {
        memset(c->synth_state, 0, sizeof(int) * (size_t)ch->lpc_order);
    }
}

/* ---- One frame of one channel ---- */

static void decode_frame(silk_channel_t *ch, opus_rc_t *rc, short *out, int lost, int cond, silk_scratch_t *tmp)
{
    silk_ctrl_t ctrl;
    int L = ch->frame_length, mv;

    ctrl.ltp_scale_Q14 = 0;
    if (lost == SILK_DECODE_NORMAL || (lost == SILK_DECODE_FEC && ch->lbrr_flags[ch->frames_decoded] == 1)) {
        decode_indices(ch, rc, ch->frames_decoded, lost, cond);
        decode_pulses(rc, tmp->pulses, ch->idx.signal_type, ch->idx.quant_offset_type, ch->frame_length);
        decode_parameters(ch, &ctrl, cond);
        L = ch->frame_length;
        decode_core(ch, &ctrl, out, tmp->pulses, tmp);
        plc(ch, &ctrl, out, 0, tmp);
        ch->loss_cnt = 0;
        ch->prev_signal_type = ch->idx.signal_type;
        ch->first_frame_after_reset = 0;
    } else {
        plc(ch, &ctrl, out, 1, tmp);
    }
    mv = ch->ltp_mem_length - ch->frame_length;
    memmove(ch->out_buf, &ch->out_buf[ch->frame_length], (size_t)mv * sizeof(short));
    memcpy(&ch->out_buf[mv], out, (size_t)ch->frame_length * sizeof(short));
    plc_glue_frames(ch, out, L);
    cng(ch, &ctrl, out, L, tmp);
    ch->lag_prev = ctrl.pitchL[ch->nb_subfr - 1];
}

/* ---- Stereo ---- */

static void stereo_decode_pred(opus_rc_t *rc, int *pred_Q13)
{
    int n = rc_icdf(rc, k_stereo_pred_joint_iCDF, 8), ix[2][3];

    ix[0][2] = n / 5;
    ix[1][2] = n - 5 * ix[0][2];
    for (n = 0; n < 2; n++) {
        ix[n][0] = rc_icdf(rc, k_uniform3_iCDF, 8);
        ix[n][1] = rc_icdf(rc, k_uniform5_iCDF, 8);
    }
    for (n = 0; n < 2; n++) {
        int low, step;
        ix[n][0] += 3 * ix[n][2];
        low = k_stereo_pred_quant_Q13[ix[n][0]];
        step = smulwb(k_stereo_pred_quant_Q13[ix[n][0] + 1] - low, 6554); /* 0.5 / 5 in Q16 */
        pred_Q13[n] = smlabb(low, step, 2 * ix[n][1] + 1);
    }
    pred_Q13[0] -= pred_Q13[1];
}

static void stereo_ms_to_lr(silk_decoder_t *d, short *x1, short *x2, const int *pred_Q13, int fs_kHz, int len)
{
    int pred0 = d->pred_prev_Q13[0], pred1 = d->pred_prev_Q13[1], denom_Q16, delta0, delta1, n;

    memcpy(x1, d->s_mid, 2 * sizeof(short));
    memcpy(x2, d->s_side, 2 * sizeof(short));
    memcpy(d->s_mid, &x1[len], 2 * sizeof(short));
    memcpy(d->s_side, &x2[len], 2 * sizeof(short));
    denom_Q16 = (1 << 16) / (STEREO_INTERP_LEN_MS * fs_kHz);
    delta0 = rshift_round(smulbb(pred_Q13[0] - d->pred_prev_Q13[0], denom_Q16), 16);
    delta1 = rshift_round(smulbb(pred_Q13[1] - d->pred_prev_Q13[1], denom_Q16), 16);
    for (n = 0; n < len; n++) {
        int sum;
        if (n < STEREO_INTERP_LEN_MS * fs_kHz) {
            pred0 += delta0;
            pred1 += delta1;
        } else {
            pred0 = pred_Q13[0];
            pred1 = pred_Q13[1];
        }
        sum = lsh(x1[n] + x1[n + 2] + lsh(x1[n + 1], 1), 9);
        sum = smlawb(lsh(x2[n + 1], 8), sum, pred0);
        sum = smlawb(sum, lsh(x1[n + 1], 11), pred1);
        x2[n + 1] = (short)sat16(rshift_round(sum, 8));
    }
    d->pred_prev_Q13[0] = (short)pred_Q13[0];
    d->pred_prev_Q13[1] = (short)pred_Q13[1];
    for (n = 0; n < len; n++) {
        int sum = x1[n + 1] + x2[n + 1], diff = x1[n + 1] - x2[n + 1];
        x1[n + 1] = (short)sat16(sum);
        x2[n + 1] = (short)sat16(diff);
    }
}

/* ---- The decoder ---- */

void silk_init(silk_decoder_t *d)
{
    memset(d, 0, sizeof *d);
    channel_init(&d->ch[0]);
    channel_init(&d->ch[1]);
}

int silk_decode(silk_decoder_t *d, opus_rc_t *rc, int channels_api, int channels_internal, int internal_hz,
                int payload_ms, int lost, int new_packet, short *out)
{
    silk_channel_t *ch = d->ch;
    silk_scratch_t *tmp = &d->tmp;
    int decode_only_middle = 0, pred_Q13[2] = {0, 0}, n_dec = 0, n_out, has_side, stereo_to_mono;

    if (new_packet)
        for (int n = 0; n < channels_internal; n++)
            ch[n].frames_decoded = 0;
    if (channels_internal > d->channels_internal)
        channel_init(&ch[1]);
    stereo_to_mono = channels_internal == 1 && d->channels_internal == 2 && internal_hz == 1000 * ch[0].fs_kHz;

    if (ch[0].frames_decoded == 0) {
        for (int n = 0; n < channels_internal; n++) {
            int fs_kHz = (internal_hz >> 10) + 1;
            if (payload_ms == 0 || payload_ms == 10) {
                ch[n].frames_per_packet = 1;
                ch[n].nb_subfr = 2;
            } else if (payload_ms == 20 || payload_ms == 40 || payload_ms == 60) {
                ch[n].frames_per_packet = payload_ms / 20;
                ch[n].nb_subfr = 4;
            } else {
                return -1;
            }
            if (fs_kHz != 8 && fs_kHz != 12 && fs_kHz != 16)
                return -1;
            set_fs(&ch[n], fs_kHz);
        }
    }
    if (channels_api == 2 && channels_internal == 2 && (d->channels_api == 1 || d->channels_internal == 1)) {
        memset(d->pred_prev_Q13, 0, sizeof d->pred_prev_Q13);
        memset(d->s_side, 0, sizeof d->s_side);
        ch[1].resampler = ch[0].resampler;
    }
    d->channels_api = channels_api;
    d->channels_internal = channels_internal;

    if (lost != SILK_PACKET_LOST && ch[0].frames_decoded == 0) {
        /* The packet's VAD and LBRR flags. */
        for (int n = 0; n < channels_internal; n++) {
            for (int i = 0; i < ch[n].frames_per_packet; i++)
                ch[n].vad_flags[i] = rc_bit_logp(rc, 1);
            ch[n].lbrr_flag = rc_bit_logp(rc, 1);
        }
        for (int n = 0; n < channels_internal; n++) {
            memset(ch[n].lbrr_flags, 0, sizeof ch[n].lbrr_flags);
            if (ch[n].lbrr_flag) {
                if (ch[n].frames_per_packet == 1) {
                    ch[n].lbrr_flags[0] = 1;
                } else {
                    int sym = rc_icdf(rc, ch[n].frames_per_packet == 2 ? k_LBRR_flags_2_iCDF : k_LBRR_flags_3_iCDF, 8) + 1;
                    for (int i = 0; i < ch[n].frames_per_packet; i++)
                        ch[n].lbrr_flags[i] = (sym >> i) & 1;
                }
            }
        }
        if (lost == SILK_DECODE_NORMAL) {
            /* Skip the redundant (LBRR) data. */
            for (int i = 0; i < ch[0].frames_per_packet; i++)
                for (int n = 0; n < channels_internal; n++)
                    if (ch[n].lbrr_flags[i]) {
                        int cond;
                        if (channels_internal == 2 && n == 0) {
                            stereo_decode_pred(rc, pred_Q13);
                            if (ch[1].lbrr_flags[i] == 0)
                                decode_only_middle = rc_icdf(rc, k_stereo_only_code_mid_iCDF, 8);
                        }
                        cond = i > 0 && ch[n].lbrr_flags[i - 1] ? CODE_CONDITIONALLY : CODE_INDEPENDENTLY;
                        decode_indices(&ch[n], rc, i, 1, cond);
                        decode_pulses(rc, tmp->pulses, ch[n].idx.signal_type, ch[n].idx.quant_offset_type,
                                      ch[n].frame_length);
                    }
        }
    }

    if (channels_internal == 2) {
        if (lost == SILK_DECODE_NORMAL || (lost == SILK_DECODE_FEC && ch[0].lbrr_flags[ch[0].frames_decoded] == 1)) {
            stereo_decode_pred(rc, pred_Q13);
            if ((lost == SILK_DECODE_NORMAL && ch[1].vad_flags[ch[0].frames_decoded] == 0) ||
                (lost == SILK_DECODE_FEC && ch[1].lbrr_flags[ch[0].frames_decoded] == 0))
                decode_only_middle = rc_icdf(rc, k_stereo_only_code_mid_iCDF, 8);
            else
                decode_only_middle = 0;
        } else {
            pred_Q13[0] = d->pred_prev_Q13[0];
            pred_Q13[1] = d->pred_prev_Q13[1];
        }
    }
    if (channels_internal == 2 && decode_only_middle == 0 && d->prev_decode_only_middle == 1) {
        memset(ch[1].out_buf, 0, sizeof ch[1].out_buf);
        memset(ch[1].sLPC_Q14_buf, 0, sizeof ch[1].sLPC_Q14_buf);
        ch[1].lag_prev = 100;
        ch[1].last_gain_index = 10;
        ch[1].prev_signal_type = TYPE_NO_VOICE;
        ch[1].first_frame_after_reset = 1;
    }
    if (lost == SILK_DECODE_NORMAL)
        has_side = !decode_only_middle;
    else
        has_side = !d->prev_decode_only_middle ||
                   (channels_internal == 2 && lost == SILK_DECODE_FEC && ch[1].lbrr_flags[ch[1].frames_decoded] == 1);

    for (int n = 0; n < channels_internal; n++) {
        if (n == 0 || has_side) {
            int frame_index = ch[0].frames_decoded - n, cond;
            if (frame_index <= 0)
                cond = CODE_INDEPENDENTLY;
            else if (lost == SILK_DECODE_FEC)
                cond = ch[n].lbrr_flags[frame_index - 1] ? CODE_CONDITIONALLY : CODE_INDEPENDENTLY;
            else if (n > 0 && d->prev_decode_only_middle)
                cond = CODE_INDEPENDENTLY_NO_LTP_SCALING;
            else
                cond = CODE_CONDITIONALLY;
            decode_frame(&ch[n], rc, &tmp->out1[n][2], lost, cond, tmp);
            n_dec = ch[n].frame_length;
        } else {
            memset(&tmp->out1[n][2], 0, (size_t)n_dec * sizeof(short));
        }
        ch[n].frames_decoded++;
    }

    if (channels_api == 2 && channels_internal == 2) {
        stereo_ms_to_lr(d, tmp->out1[0], tmp->out1[1], pred_Q13, ch[0].fs_kHz, n_dec);
    } else {
        memcpy(tmp->out1[0], d->s_mid, 2 * sizeof(short));
        memcpy(d->s_mid, &tmp->out1[0][n_dec], 2 * sizeof(short));
    }

    n_out = n_dec * 48 / ch[0].fs_kHz;
    for (int n = 0; n < imin(channels_api, channels_internal); n++) {
        resample(&ch[n].resampler, tmp->out2, &tmp->out1[n][1], n_dec);
        for (int i = 0; i < n_out; i++)
            out[n + channels_api * i] = tmp->out2[i];
    }
    if (channels_api == 2 && channels_internal == 1) {
        if (stereo_to_mono) {
            resample(&ch[1].resampler, tmp->out2, &tmp->out1[0][1], n_dec);
            for (int i = 0; i < n_out; i++)
                out[1 + 2 * i] = tmp->out2[i];
        } else {
            for (int i = 0; i < n_out; i++)
                out[1 + 2 * i] = out[2 * i];
        }
    }
    if (lost == SILK_PACKET_LOST) {
        for (int i = 0; i < d->channels_internal; i++)
            ch[i].last_gain_index = 10;
    } else {
        d->prev_decode_only_middle = decode_only_middle;
    }
    return n_out;
}
