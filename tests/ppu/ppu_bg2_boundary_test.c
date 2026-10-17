/* ER-07 diagnostic: synthetic Mode 1 BG2 at captured scroll boundaries.
 * No ROM, guest code, tiles or memory dumps are used. This isolates pixel
 * addressing; it deliberately makes no claim about hardware HDMA phase. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "snes/ppu.h"
#include "snes/snes.h"

Snes *g_snes;
int snes_frame_counter;
unsigned char g_snesrecomp_last_hdmaen;
uint16_t WsShadowTile(int layer, int x, uint32_t y, uint16_t scroll,
                      uint16_t map, uint16_t tile) {
    (void)layer; (void)x; (void)y; (void)scroll; (void)map; return tile;
}
bool WsShadowLayerActive(int layer) { (void)layer; return false; }
uint32_t WsShadowWorldX(int layer) { (void)layer; return 0; }
uint32_t WsShadowPresentWorldY(int layer, int x) { (void)layer; (void)x; return 0; }
uint32_t WsShadowScrollY(int layer) { (void)layer; return 0; }
void WsShadowOnVramWrite(uint16_t address, uint16_t value) {
    (void)address; (void)value;
}

static uint32_t pixels[2][256 * 224];

static void fixture(Ppu *ppu) {
    ppu_reset(ppu);
    memset(ppu->vram, 0, sizeof(ppu->vram));
    memset(ppu->cgram, 0, sizeof(ppu->cgram));
    for (unsigned i = 0; i < 128; i++) ppu->oam[i * 2] = 0xf000;
    ppu->inidisp = 15;
    ppu->bgmode = 0x69; /* Mode 1, 16x16 BG2, as in the captured boss. */
    ppu->bgXsc[1] = 0x3a; /* one map wide, two maps high */
    ppu->bgTileAdr = 0x0277; /* BG2 character base $7000 (word address) */
    ppu->screenEnabled[0] = 2;
    ppu->screenEnabled[1] = 0;
    ppu->cgram[1] = 0x001f;
    ppu->cgram[2] = 0x03e0;
    /* A 16x16 map entry references four characters. Keep all four blank
     * characters separate from the colored test tiles. */
    for (unsigned i = 0; i < 2048; i++) ppu->vram[0x3800 + i] = 128;
    /* Last row of the first 256-pixel band is green; last row of the
     * first 512-pixel page is red. All other source rows are transparent. */
    for (unsigned x = 0; x < 32; x++) {
        ppu->vram[0x3800 + 15 * 32 + x] = 3;
        ppu->vram[0x3800 + 31 * 32 + x] = 1;
    }
    for (unsigned right = 0; right < 2; right++) {
        ppu->vram[0x7000 + (17 + right) * 16 + 7] = 0x00ff;
        ppu->vram[0x7000 + (19 + right) * 16 + 7] = 0xff00;
    }
}

typedef struct { unsigned output_y, vofs, source_y; } Sample;
int main(void) {
    static const Sample samples[] = {
        {43,979,1023}, {44,978,1023}, {45,465,511}, {46,468,515},
        {44,466,511}, {45,488,534}, {170,84,255}, {171,851,1023},
        {76,434,511}, {48,462,511}, {24,486,511},
        /* Counterfactual previous-line value: changes the selected row,
         * but is not evidence that delaying HDMA is correct in the game. */
        {45,978,0}, {44,979,0}, {170,108,279},
        /* Following lines with the preceding HDMA sample. The chosen
         * coordinate becomes 512 instead of the colored edge row 511. */
        {46,465,512}, {77,434,512}, {49,462,512}, {25,486,512},
        {0,254,255}, {0,255,256}, {0,510,511}, {0,511,512}
    };
    static const unsigned scrolls[] = {0,36,971,988,1023};
    Ppu *ppu = ppu_init();
    unsigned cases = 0;
    if (!ppu) return 2;
    for (unsigned i = 0; i < sizeof(samples)/sizeof(samples[0]); i++) {
        const Sample *s = &samples[i];
        uint32_t expected = s->source_y == 511 ? 0xff0000u :
                            s->source_y == 255 ? 0x00ff00u : 0;
        if (((s->output_y + 1 + s->vofs) & 1023) != s->source_y) return 3;
        for (unsigned hi = 0; hi < sizeof(scrolls)/sizeof(scrolls[0]); hi++) {
            for (unsigned renderer = 0; renderer < 2; renderer++) {
                fixture(ppu);
                ppu->hScroll[1] = (uint16_t)scrolls[hi];
                ppu->vScroll[1] = (uint16_t)s->vofs;
                memset(pixels[renderer], 0xa5, sizeof(pixels[renderer]));
                PpuBeginDrawing(ppu, (uint8_t *)pixels[renderer], 256 * 4,
                                renderer | kPpuRenderFlags_NoSpriteLimits);
                ppu_runLine(ppu, 0);
                ppu_runLine(ppu, (int)s->output_y + 1);
                for (unsigned x = 0; x < 256; x++) {
                    uint32_t got = pixels[renderer][s->output_y * 256 + x] & 0xffffff;
                    if (got != expected) {
                        fprintf(stderr,"FAIL renderer=%u y=%u VOFS=%u H=%u x=%u got=%06X expected=%06X\n",
                                renderer,s->output_y,s->vofs,scrolls[hi],x,got,expected);
                        ppu_free(ppu); return 1;
                    }
                }
            }
            cases++;
        }
    }
    ppu_free(ppu);
    printf("PASS: %u synthetic BG2 cases, both renderers, 256 pixels per case.\n",cases);
    puts("Source rows 255/511 are rendered by A and B when selected; hardware HDMA phase and the game's intended graphics are not established by this test.");
    return 0;
}
