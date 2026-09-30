/* Lottie animations drawn frame by frame as SVG: shapes, fills, keyframes, parents and precomps. */
#include "test.h"
#include "lottie.h"

static int has(const sb_t *sb, const char *s)
{
    size_t n = sc_strlen(s);

    for (size_t i = 0; sb->data && i + n <= sb->len; i++) {
        size_t k = 0;
        while (k < n && sb->data[i + k] == s[k])
            k++;
        if (k == n)
            return 1;
    }
    return 0;
}

static int svg_of(const char *json, double frame, sb_t *svg)
{
    lottie_t *L = lottie_load(json, sc_strlen(json));

    if (!L)
        return 0;
    lottie_svg(L, frame, svg);
    lottie_free(L);
    return 1;
}

#define LINE "\"shapes\":[{\"ty\":\"sh\",\"ks\":{\"a\":0,\"k\":{\"v\":[[0,0],[10,0]],\"i\":[[0,0],[0,0]],\"o\":[[1,0],[0,0]],\"c\":false}}}," \
             "{\"ty\":\"fl\",\"c\":{\"a\":0,\"k\":[1,0,0.5,1]},\"o\":{\"a\":0,\"k\":50}}]"

void entry(void)
{
    sb_t svg = {0};
    double first = -1, last = -1, fps = -1;

    {
        const char *doc = "{\"w\":100,\"h\":50,\"ip\":0,\"op\":60,\"fr\":30,\"layers\":[{\"ty\":4,\"ind\":1,\"ip\":0,\"op\":60,"
                          "\"ks\":{\"p\":{\"a\":0,\"k\":[10,20.5]}}," LINE "}]}";
        lottie_t *L = lottie_load(doc, sc_strlen(doc));
        check(L != NULL, "loads");
        if (L)
            lottie_info(L, &first, &last, &fps);
        check(L && first == 0 && last == 60 && fps == 30 && lottie_bytes(L) > 0, "info");
        lottie_free(L);
        check(svg_of(doc, 0, &svg) &&
                  str_eq(&svg, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"50\" viewBox=\"0 0 100 50\">"
                               "<g transform=\"matrix(1 0 0 1 10 20.5)\">"
                               "<path d=\"M0,0C1,0 10,0 10,0\" fill=\"#ff0080\" fill-opacity=\"0.5\"/></g></svg>"),
              "a static path, filled");
        check(svg_of(doc, 60, &svg) && !has(&svg, "<path"), "layers end at their out point");
    }
    {
        /* position moves 0 -> 10 over frames 0-10, then holds at 10 until 20, then jumps to 30 */
        const char *doc = "{\"w\":10,\"h\":10,\"layers\":[{\"ty\":4,\"ks\":{\"p\":{\"a\":1,\"k\":["
                          "{\"t\":0,\"s\":[0,0]},{\"t\":10,\"s\":[10,0],\"h\":1},{\"t\":20,\"s\":[30,0]}]}}," LINE "}]}";
        check(svg_of(doc, 5, &svg) && has(&svg, "matrix(1 0 0 1 5 0)"), "keyframes: linear in between");
        check(svg_of(doc, 15, &svg) && has(&svg, "matrix(1 0 0 1 10 0)"), "keyframes: holds");
        check(svg_of(doc, 25, &svg) && has(&svg, "matrix(1 0 0 1 30 0)"), "keyframes: the last value stays");
        check(svg_of(doc, -5, &svg) && has(&svg, "matrix(1 0 0 1 0 0)"), "keyframes: the first value before");
    }
    {
        /* ease-in (handles at x .9 y 0 and x 1 y 1): halfway in time is well short of halfway */
        const char *doc = "{\"w\":10,\"h\":10,\"layers\":[{\"ty\":4,\"ks\":{\"o\":{\"a\":1,\"k\":["
                          "{\"t\":0,\"s\":[0],\"o\":{\"x\":[0.9],\"y\":[0]},\"i\":{\"x\":[1],\"y\":[1]}},{\"t\":10,\"s\":[100]}]}}," LINE
                          "}]}";
        check(svg_of(doc, 5, &svg) && has(&svg, " opacity=\"0.13"), "keyframes: eased");
    }
    {
        /* a child scaled 200% under a parent moved by (5, 5), inside a precomp that starts at frame 10 */
        const char *doc = "{\"w\":10,\"h\":10,\"assets\":[{\"id\":\"c\",\"layers\":["
                          "{\"ty\":3,\"ind\":1,\"ks\":{\"p\":{\"a\":0,\"k\":[5,5]}}},"
                          "{\"ty\":4,\"ind\":2,\"parent\":1,\"ip\":0,\"op\":5,\"ks\":{\"s\":{\"a\":0,\"k\":[200,200]}}," LINE "}]}],"
                          "\"layers\":[{\"ty\":0,\"refId\":\"c\",\"st\":10,\"ks\":{}}]}";
        check(svg_of(doc, 12, &svg) && has(&svg, "matrix(2 0 0 2 5 5)"), "parents and precomps");
        check(svg_of(doc, 16, &svg) && !has(&svg, "<path"), "precomps keep their own clock");
    }
    {
        const char *doc = "{\"w\":10,\"h\":10,\"layers\":[{\"ty\":4,\"shapes\":[{\"ty\":\"gr\",\"it\":["
                          "{\"ty\":\"el\",\"p\":{\"a\":0,\"k\":[5,5]},\"s\":{\"a\":0,\"k\":[4,2]}},"
                          "{\"ty\":\"st\",\"c\":{\"a\":0,\"k\":[0,0,0]},\"o\":{\"a\":0,\"k\":100},\"w\":{\"a\":0,\"k\":1.5},\"lc\":2,\"lj\":1},"
                          "{\"ty\":\"tr\",\"r\":{\"a\":0,\"k\":90}}]}]}]}";
        check(svg_of(doc, 0, &svg) && has(&svg, "matrix(0 1 -1 0 0 0)") && has(&svg, "M5,4C") &&
                  has(&svg, "fill=\"none\" stroke=\"#000000\" stroke-width=\"1.5\" stroke-linecap=\"round\" stroke-linejoin=\"miter\""),
              "groups: ellipses, strokes, rotation");
    }
    check(!lottie_load("<svg/>", 6) && !lottie_load("{\"a\":1}", 7) && !lottie_load("{\"w\":1,\"h\":1}", 13),
          "only Lottie is Lottie");
    sb_free(&svg);
    finish();
}
