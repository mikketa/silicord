/*
 * Conformance check against the Opus test vectors of RFC 8251:
 *
 *   opus_vectors testvector01.bit testvector01.dec [ours.pcm]
 *
 * Decodes the .bit file (records of a 32-bit big-endian length, the
 * reference encoder's final range coder state, then the packet) to 48 kHz
 * stereo, requires every packet's final range to match, and compares the
 * audio with the reference decoding. Fails below MIN_SNR_DB.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "opus.h"

#define MIN_SNR_DB 50.0

static unsigned char *load(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)*n + 1);
    if (buf && fread(buf, 1, (size_t)*n, f) != (size_t)*n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

static unsigned be32(const unsigned char *p)
{
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}

int main(int argc, char **argv)
{
    static opus_decoder_t dec;
    static float pcm[5760 * 2];
    unsigned char *bits, *ref;
    long nbits, nref, at = 0, pos = 0, packets = 0, mismatches = 0, first_bad = -1, reported = 0;
    int prev_config = -1;
    double signal = 0, error = 0, snr;
    FILE *dump = argc > 3 ? fopen(argv[3], "wb") : NULL; /* our decoding, for a closer look */

    if (argc < 3) {
        fprintf(stderr, "usage: opus_vectors in.bit reference.dec\n");
        return 2;
    }
    bits = load(argv[1], &nbits);
    ref = load(argv[2], &nref);
    if (!bits || !ref) {
        fprintf(stderr, "cannot read %s or %s\n", argv[1], argv[2]);
        return 2;
    }
    opus_decoder_init(&dec, 2);
    while (at + 8 <= nbits) {
        unsigned len = be32(bits + at), range = be32(bits + at + 4);
        int got;
        at += 8;
        if (len > (unsigned long)(nbits - at))
            break;
        got = opus_decode(&dec, len ? bits + at : NULL, len, pcm, 960);
        if (got < 0) {
            printf("packet %ld is malformed\n", packets);
            got = 0;
        }
        if (len && dec.final_range != range) {
            mismatches++;
            if (first_bad < 0)
                first_bad = packets;
        }
        {
            double psig = 0, perr = 0;
            for (int i = 0; i < got * 2 && pos + 1 < nref; i++, pos += 2) {
                float v = pcm[i] * 32768.f;
                int s = v > 32767.f ? 32767 : v < -32768.f ? -32768 : (int)lrintf(v);
                int r = (short)(ref[pos] | ref[pos + 1] << 8);
                if (dump) {
                    short w = (short)s;
                    fwrite(&w, sizeof w, 1, dump);
                }
                psig += (double)r * r;
                perr += (double)(s - r) * (s - r);
            }
            signal += psig;
            error += perr;
            /* Where the audio parts from the reference: the first few packets far below the target. */
            if (perr > 0 && 10 * log10((psig + 1) / (perr + 1)) < MIN_SNR_DB && reported < 8) {
                printf("  packet %ld (config %d after %d, %u bytes): %.1f dB\n", packets, len ? bits[at] >> 3 : -1,
                       prev_config, len, 10 * log10((psig + 1) / (perr + 1)));
                reported++;
            }
            if (len)
                prev_config = bits[at] >> 3;
        }
        at += (long)len;
        packets++;
    }
    snr = 10 * log10((signal + 1) / (error + 1));
    printf("%s: %ld packets, %ld range mismatches (first at %ld), SNR %.1f dB\n", argv[1], packets, mismatches,
           first_bad, snr);
    if (dump)
        fclose(dump);
    free(bits);
    free(ref);
    return mismatches || snr < MIN_SNR_DB || pos + 1 < nref ? 1 : 0;
}
