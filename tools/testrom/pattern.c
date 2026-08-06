/* Test-pattern generator for a GBA in mode 3 (240x160, 16-bit BGR555).
   Arithmetic only -- no lookup tables, so nothing lands in .rodata and the
   whole program is one .text section that needs no linker.

   ## Why it builds rows and blits them

   The obvious version computes a colour per pixel straight into VRAM. At
   16.78 MHz that took about twelve frames to fill the screen once, so the
   animation ran at a twelfth speed: 38400 volatile halfword stores, each
   fighting the PPU for the VRAM bus during active display, with the pattern
   arithmetic redone for every one.

   Every pattern here is row-coherent -- bars and the ramp strip repeat down
   the screen, the crosshatch has only a line row and a gap row, the
   checkerboard alternates every eight. So each frame computes at most two
   rows into IWRAM and copies them out 32 bits at a time. The per-pixel maths
   drops from 38400 evaluations to 480, and the stores halve. */
#define VRAM    ((unsigned int *)0x06000000)   /* not volatile: it is memory */
#define DISPCNT (*(volatile unsigned int   *)0x04000000)
#define VCOUNT  (*(volatile unsigned short *)0x04000006)

#define W   240
#define H   160
#define WW  (W / 2)                            /* words per row, two pixels each */

static inline unsigned rgb(unsigned r, unsigned g, unsigned b)
{
    return (r & 31) | ((g & 31) << 5) | ((b & 31) << 10);
}

static inline unsigned pair(unsigned lo, unsigned hi)
{
    return lo | (hi << 16);
}

static void blit(unsigned *dst, const unsigned *row, unsigned rows)
{
    unsigned y, x;
    for (y = 0; y < rows; y++)
        for (x = 0; x < WW; x++)
            *dst++ = row[x];
}

void gbamain(void)
{
    unsigned rowA[WW];
    unsigned rowB[WW];
    unsigned frame = 0;

    DISPCNT = 0x0403;                          /* mode 3, BG2 on */

    for (;;) {
        unsigned phase = (frame >> 5) & 3;
        unsigned x, y;

        if (phase == 0) {
            /* Eight bars in SMPTE order, scrolling, over a grey ramp strip.
               A bar's colour is the bit pattern of its index, so no table. */
            for (x = 0; x < WW; x++) {
                unsigned x0 = x * 2, x1 = x0 + 1;
                unsigned i0 = ((x0 + frame) >> 5) & 7;
                unsigned i1 = ((x1 + frame) >> 5) & 7;
                rowA[x] = pair(rgb((i0 & 4) ? 0 : 31, (i0 & 2) ? 0 : 31, (i0 & 1) ? 0 : 31),
                               rgb((i1 & 4) ? 0 : 31, (i1 & 2) ? 0 : 31, (i1 & 1) ? 0 : 31));
                rowB[x] = pair(rgb(x0 >> 3, x0 >> 3, x0 >> 3),
                               rgb(x1 >> 3, x1 >> 3, x1 >> 3));
            }
            blit(VRAM, rowA, 128);
            blit(VRAM + 128 * WW, rowB, H - 128);
        } else if (phase == 1) {
            /* Crosshatch, drifting: geometry and convergence. Two rows only --
               one that is a horizontal line, one that is not. */
            unsigned fy = frame >> 1;
            for (x = 0; x < WW; x++) {
                unsigned x0 = x * 2, x1 = x0 + 1;
                unsigned g0 = ((x0 + frame) & 31) < 2, g1 = ((x1 + frame) & 31) < 2;
                rowA[x] = pair(g0 ? rgb(31, 31, 31) : rgb(0, 0, 6),
                               g1 ? rgb(31, 31, 31) : rgb(0, 0, 6));
                rowB[x] = pair(rgb(31, 31, 31), rgb(31, 31, 31));
            }
            for (y = 0; y < H; y++)
                blit(VRAM + y * WW, (((y + fy) & 31) < 2) ? rowB : rowA, 1);
        } else if (phase == 2) {
            /* A hard vertical edge sweeping across a grey ramp -- the pair a
               composite path smears differently from each other. */
            unsigned edge = frame & 255;
            for (x = 0; x < WW; x++) {
                unsigned x0 = x * 2, x1 = x0 + 1;
                unsigned v0 = x0 >> 3, v1 = x1 >> 3;
                rowA[x] = pair((x0 > edge) ? rgb(v0, v0, v0) : rgb(31 - v0, 0, v0),
                               (x1 > edge) ? rgb(v1, v1, v1) : rgb(31 - v1, 0, v1));
            }
            blit(VRAM, rowA, H);
        } else {
            /* Checkerboard, inverting: the highest frequency the raster has. */
            unsigned f4 = frame >> 4;
            for (x = 0; x < WW; x++) {
                unsigned x0 = x * 2, x1 = x0 + 1;
                unsigned k0 = (x0 >> 3) & 1, k1 = (x1 >> 3) & 1;
                rowA[x] = pair(k0 ? rgb(31, 31, 31) : 0, k1 ? rgb(31, 31, 31) : 0);
                rowB[x] = pair(k0 ? 0 : rgb(31, 31, 31), k1 ? 0 : rgb(31, 31, 31));
            }
            for (y = 0; y < H; y++)
                blit(VRAM + y * WW, (((y >> 3) + f4) & 1) ? rowB : rowA, 1);
        }

        while (VCOUNT >= H) { }                /* wait out this vblank */
        while (VCOUNT <  H) { }                /* then wait for the next */
        frame++;
    }
}
