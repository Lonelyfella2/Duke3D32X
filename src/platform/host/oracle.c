/*
 * oracle.c - desktop render oracle: loads the little-endian data blob, puts
 * the camera at a map's start (or a given pose) and writes a PNG of the 3D
 * view rendered exactly like the 32X does (two column bands).
 *
 *   oracle gen/gamedata_le.bin <map#> out.ppm [x y z ang horiz]
 */
#include <stdio.h>
#include <stdlib.h>
#include "render.h"

static pixel_t fb[RMAXYDIM][RMAXXDIM];

static void *loadfile(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
    void *p = malloc(*len);
    if (fread(p, 1, *len, f) != (size_t)*len) { perror("read"); exit(1); }
    fclose(f);
    return p;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: oracle blob map out.ppm [x y z ang horiz]\n"); return 1; }
    long len;
    void *blob = loadfile(argv[1], &len);
    if (engine_init(blob)) { fprintf(stderr, "bad blob\n"); return 1; }
    render_init();
    int32_t px, py, pz; int16_t pang, psect;
    int m = atoi(argv[2]);
    if (engine_loadboard(m, &px, &py, &pz, &pang, &psect)) { fprintf(stderr, "loadboard failed\n"); return 1; }
    if (argc >= 9) { px = atoi(argv[4]); py = atoi(argv[5]); pz = atoi(argv[6]); pang = atoi(argv[7]); }
    int horiz = argc >= 9 ? atoi(argv[8]) : 100;
    updatesector(px, py, &psect);
    const dk_map_t *mi = engine_mapinfo(m);
    fprintf(stderr, "%s '%s': %d sectors %d walls, start (%d,%d,%d) ang %d sect %d\n",
            mi->name, mi->title, numsectors, numwalls, px, py, pz, pang, psect);

    rview.posx = px; rview.posy = py; rview.posz = pz;
    rview.ang = pang; rview.horiz = horiz; rview.sectnum = psect;
    rview.xdim = 160; rview.ydim = 96;
    rview.frame = &fb[0][0]; rview.pitch = RMAXXDIM;
    rview.skypicnum = 89;
    rview.parallaxyscale = 16384 + 1024;
    {   /* LA sky (Duke setupbackdrop) */
        static const int8_t la[8] = { 1, 2, 1, 3, 4, 0, 2, 3 };
        memcpy(rview.pskyoff, la, 8);
    }
    for (int y = 0; y < RMAXYDIM; y++) for (int x = 0; x < RMAXXDIM; x++) fb[y][x] = 0xF0F0;
    render_frame_m(0, 79);
    render_frame_s(80, 159);
    fprintf(stderr, "master: walls %d bunches %d sprites %d spans %d cols %d\n",
            rstats_m.walls, rstats_m.bunches, rstats_m.sprites, rstats_m.spans, rstats_m.cols);
    fprintf(stderr, "slave : walls %d bunches %d sprites %d spans %d cols %d\n",
            rstats_s.walls, rstats_s.bunches, rstats_s.sprites, rstats_s.spans, rstats_s.cols);

    FILE *o = fopen(argv[3], "wb");
    int W = rview.xdim * 2, H = rview.ydim * 2;
    fprintf(o, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint16_t c = dk_cram[fb[y >> 1][x >> 1] & 255];
            unsigned char rgb[3] = { (unsigned char)((c & 31) << 3), (unsigned char)(((c >> 5) & 31) << 3),
                                     (unsigned char)(((c >> 10) & 31) << 3) };
            fwrite(rgb, 1, 3, o);
        }
    fclose(o);
    return 0;
}
