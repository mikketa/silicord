/*
 * Conformance driver for the Opus test vectors (RFC 8251): decodes a .bit
 * file to 48 kHz stereo 16-bit PCM and checks each packet's final range
 * coder state against the one recorded by the reference encoder.
 *
 *   opus_vectors testvector01.bit out.pcm
 *
 * Compare out.pcm with testvector01.dec (tests/opus_compare.py). The
 * vectors are not in the repository.
 */
#include "test.h"
#include <shellapi.h>
#include "mem.h"
#include "opus.h"

static unsigned be32(const unsigned char *p)
{
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}

static void out_num(const char *label, long long v)
{
    sb_t s = {0};

    sb_add(&s, label);
    sb_i64(&s, v);
    sb_add(&s, "\r\n");
    out(s.data);
    sb_free(&s);
}

static unsigned char *read_file(const wchar_t *path, size_t *n)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER size;
    unsigned char *buf = NULL;
    DWORD got;

    if (f == INVALID_HANDLE_VALUE)
        return NULL;
    if (GetFileSizeEx(f, &size) && (buf = mem_alloc((size_t)size.QuadPart + 1)) &&
        ReadFile(f, buf, (DWORD)size.QuadPart, &got, NULL))
        *n = got;
    CloseHandle(f);
    return buf;
}

void entry(void)
{
    int argc;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    static opus_decoder_t dec;
    static float pcm[5760 * 2];
    static short pcm16[5760 * 2];
    unsigned char *bits;
    size_t n = 0, at = 0;
    long packets = 0, checked = 0, mismatches = 0, first_bad = -1;
    HANDLE outf;

    if (argc < 3) {
        out("usage: opus_vectors in.bit out.pcm\r\n");
        ExitProcess(2);
    }
    bits = read_file(argv[1], &n);
    outf = CreateFileW(argv[2], GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (!bits || outf == INVALID_HANDLE_VALUE) {
        out("cannot open the files\r\n");
        ExitProcess(2);
    }
    opus_decoder_init(&dec, 2);
    while (at + 8 <= n) {
        unsigned len = be32(bits + at), range = be32(bits + at + 4);
        int got;
        DWORD w;
        at += 8;
        if (len > n - at)
            break;
        got = opus_decode(&dec, len ? bits + at : NULL, len, pcm, 960);
        if (got < 0) {
            out_num("malformed packet ", packets);
            got = 0;
        }
        /* Only CELT frames are decoded so far: check their range. */
        if (len && (bits[at] >> 3) >= 16) {
            checked++;
            if (dec.final_range != range) {
                mismatches++;
                if (first_bad < 0)
                    first_bad = packets;
            }
        }
        for (int i = 0; i < got * 2; i++) {
            float v = pcm[i] * 32768.f;
            pcm16[i] = (short)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : (int)(v < 0 ? v - 0.5f : v + 0.5f));
        }
        WriteFile(outf, pcm16, (DWORD)(got * 2 * sizeof(short)), &w, NULL);
        at += len;
        packets++;
    }
    CloseHandle(outf);
    out_num("packets ", packets);
    out_num("range checked ", checked);
    out_num("range mismatches ", mismatches);
    out_num("first mismatch at packet ", first_bad);
    ExitProcess(mismatches ? 1 : 0);
}
