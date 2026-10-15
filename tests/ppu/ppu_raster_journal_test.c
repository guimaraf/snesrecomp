/* ROM-free test of the raster journal (ppu.c): field cut by the beam, every
 * display register journaled, render restores the completed field and
 * RenderEnd restores the guest registers/latches and OAM saved before rendering.
 * Modelled on Doom's frame:
 * vblank write, a split at line 22, a split at 199, and a CPU half that runs
 * past V=225 into the next field. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snes/ppu.h"
#include "snes/snes.h"

Snes *g_snes;
int snes_frame_counter;
unsigned char g_snesrecomp_last_hdmaen;
uint16_t WsShadowTile(int layer, int x, uint32_t y, uint16_t tile) {
  (void)layer; (void)x; (void)y; return tile;
}
bool WsShadowLayerActive(int layer) { (void)layer; return false; }
uint32_t WsShadowWorldX(int layer) { (void)layer; return 0; }
uint32_t WsShadowPresentWorldY(int layer, int x) { (void)layer; (void)x; return 0; }
uint32_t WsShadowScrollY(int layer) { (void)layer; return 0; }
void WsShadowOnVramWrite(uint16_t address, uint16_t value) { (void)address; (void)value; }

#define CHECK(test) do { if (!(test)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); exit(1); \
} } while (0)

static Snes s_snes;
static Ppu p;

static void write_at(uint16_t line, uint8_t reg, uint8_t val) {
  s_snes.vPos = line;
  ppu_write(&p, reg, val);
}

static void test_oam_field_timeline(void) {
  ppu_rasterReset();
  ppu_reset(&p);
  /* A stationary object at world X=200: old camera=100, new camera=103.
   * The late upload prepares X=97 while this field still displays X=100. */
  write_at(225, 0x0d, 100);
  write_at(225, 0x0d, 0);
  p.oam[0] = 0x2064;
  p.oam[255] = 0x1234;
  p.highOam[0] = 0x10;
  p.highOam[31] = 0x22;
  ppu_rasterBegin(&p);
  write_at(218, 0x02, 0);
  write_at(218, 0x03, 0);
  /* All 512 low and 32 high bytes, including the automatic table wrap. */
  for (unsigned i = 0; i < 256; i++) {
    uint16_t word = i == 0 ? 0x2061 : (uint16_t)(0x4000 | i);
    write_at(218, 0x04, (uint8_t)word);
    write_at(218, 0x04, (uint8_t)(word >> 8));
  }
  for (unsigned i = 0; i < 32; i++)
    write_at(218, 0x04, (uint8_t)(0x80 | i));
  s_snes.vPos = 225;
  ppu_rasterFieldBoundary();
  write_at(251, 0x0d, 103);
  write_at(251, 0x0d, 0);
  /* Additional writes in field B must not leak into A, even after its split. */
  write_at(252, 0x02, 5);
  write_at(252, 0x03, 0);
  write_at(252, 0x04, 0xef);
  write_at(252, 0x04, 0xbe);
  write_at(252, 0x02, 15);
  write_at(252, 0x03, 1);
  write_at(252, 0x04, 0xdd);
  write_at(252, 0x04, 0xee);
  const uint32_t guest_writes = p.oamWriteCount;
  const uint8_t guest_adr = p.oamAdr;
  const bool guest_high = p.oamInHigh, guest_second = p.oamSecondWrite;
  const uint8_t guest_buffer = p.oamBuffer;
  uint16_t guest_oam[256];
  uint8_t guest_high_oam[32];
  memcpy(guest_oam, p.oam, sizeof(guest_oam));
  memcpy(guest_high_oam, p.highOam, sizeof(guest_high_oam));
  {
    char dump[32768];
    ppu_rasterDebugDump(dump, sizeof(dump));
    CHECK(strstr(dump, "\"oam\":true") != NULL);
    CHECK(strstr(dump, "\"r\":\"0x8000\"") == NULL);
    CHECK(strstr(dump, "\"v\":\"0x2061\"") == NULL);
  }
  /* Repeating a render neither overwrites history nor creates new uploads. */
  for (int pass = 0; pass < 2; pass++) {
    ppu_rasterRenderBegin(&p);
    CHECK(p.oam[0] == 0x2064 && p.oam[255] == 0x1234);
    CHECK(p.highOam[0] == 0x10 && p.highOam[31] == 0x22);
    ppu_rasterApplyLine(&p, 0);
    CHECK((p.oam[0] & 255) + p.hScroll[0] == 200);
    ppu_rasterApplyLine(&p, 217);
    CHECK(p.oam[0] == 0x2064 && p.oam[5] == 0);
    ppu_rasterApplyLine(&p, 218);
    CHECK(p.oam[0] == 0x2061 && p.oam[5] == 0x4005);
    CHECK(p.oam[255] == 0x40ff && p.highOam[31] == 0x9f);
    CHECK(p.hScroll[0] == 100 && p.oamWriteCount == guest_writes);
    /* A render-walk OAM write must not leak into guest memory/counters. */
    ppu_write(&p, 0x02, 0);
    ppu_write(&p, 0x03, 0);
    ppu_write(&p, 0x04, 0xaa);
    ppu_write(&p, 0x04, 0xbb);
    ppu_rasterRenderEnd(&p);
    CHECK(p.hScroll[0] == 103 && p.oam[0] == 0x2061);
    CHECK(p.oam[5] == 0xbeef && p.highOam[30] == 0xdd && p.highOam[31] == 0xee);
    CHECK(p.oamWriteCount == guest_writes && p.oamAdr == guest_adr);
    CHECK(p.oamInHigh == guest_high && p.oamSecondWrite == guest_second);
    CHECK(p.oamBuffer == guest_buffer);
    CHECK(memcmp(p.oam, guest_oam, sizeof(guest_oam)) == 0);
    CHECK(memcmp(p.highOam, guest_high_oam, sizeof(guest_high_oam)) == 0);
  }
  /* The following field pairs the new coordinates with the new scroll. */
  s_snes.vPos = 225;
  ppu_rasterFieldBoundary();
  ppu_rasterRenderBegin(&p);
  ppu_rasterApplyLine(&p, 0);
  CHECK((p.oam[0] & 255) + p.hScroll[0] == 200);
  CHECK(p.oam[5] == 0xbeef && p.highOam[31] == 0xee);
  CHECK(p.oamWriteCount == guest_writes);
  ppu_rasterRenderEnd(&p);
}

static void test_oam_commit_and_boundary(void) {
  ppu_rasterReset();
  ppu_reset(&p);
  p.oam[5] = 0x1234;
  ppu_rasterBegin(&p);
  write_at(10, 0x02, 5);
  write_at(10, 0x03, 0);
  write_at(10, 0x04, 0xaa); /* Only the port latch changes. */
  write_at(12, 0x04, 0xbb); /* The word commits here. */
  /* First frame: no completed field yet, so replay the recording field. */
  ppu_rasterRenderBegin(&p);
  ppu_rasterApplyLine(&p, 11);
  CHECK(p.oam[5] == 0x1234);
  ppu_rasterApplyLine(&p, 12);
  CHECK(p.oam[5] == 0xbbaa);
  ppu_rasterRenderEnd(&p);
  write_at(224, 0x04, 0x77);
  write_at(224, 0x04, 0x99); /* Outside the driver's visible replay window. */
  write_at(224, 0x04, 0xcc); /* Half a word crossing the field boundary. */
  s_snes.vPos = 225;
  ppu_rasterFieldBoundary();
  write_at(230, 0x04, 0xdd);
  const uint32_t guest_writes = p.oamWriteCount;
  ppu_rasterRenderBegin(&p);
  ppu_rasterApplyLine(&p, 11);
  CHECK(p.oam[5] == 0x1234);
  ppu_rasterApplyLine(&p, 12);
  CHECK(p.oam[5] == 0xbbaa);
  ppu_rasterApplyLine(&p, 223);
  CHECK(p.oam[6] == 0 && p.oam[7] == 0);
  ppu_rasterRenderEnd(&p);
  CHECK(p.oam[6] == 0x9977 && p.oam[7] == 0xddcc);
  CHECK(p.oamAdr == 8 && !p.oamSecondWrite);
  CHECK(p.oamWriteCount == guest_writes);
  s_snes.vPos = 225;
  ppu_rasterFieldBoundary();
  ppu_rasterRenderBegin(&p);
  ppu_rasterApplyLine(&p, 0);
  CHECK(p.oam[6] == 0x9977 && p.oam[7] == 0xddcc);
  ppu_rasterRenderEnd(&p);
}

int main(void) {
  memset(&s_snes, 0, sizeof(s_snes));
  memset(&p, 0, sizeof(p));
  ppu_reset(&p);
  g_snes = &s_snes;
  s_snes.ppu = &p;

  /* Unarmed: writes are not journaled and a render leaves state alone. */
  write_at(100, 0x26, 0x33);
  ppu_rasterRenderBegin(&p);
  ppu_rasterApplyLine(&p, 223);
  ppu_rasterRenderEnd(&p);
  CHECK(p.window1left == 0x33);

  /* Field A starts here (the host arms at its frame start, beam at V=225). */
  write_at(225, 0x00, 0x80);
  write_at(225, 0x26, 0xFF);
  ppu_rasterBegin(&p);
  write_at(230, 0x00, 0x0F);        /* vblank: before line 0 of field A */
  write_at(22, 0x26, 0x14);         /* top split: window, not on the old list */
  write_at(22, 0x27, 0xEB);
  write_at(199, 0x00, 0x80);        /* bottom split */
  write_at(199, 0x26, 0xFF);
  write_at(150, 0x18, 0x55);        /* VMDATA: an upload, never replayed */
  /* The bottom split's DMA carries the CPU across V=225 ... */
  s_snes.vPos = 225;
  ppu_rasterFieldBoundary();
  /* ... and what it writes after that belongs to field B. */
  write_at(10, 0x26, 0x20);
  write_at(12, 0x02, 0x40);         /* OAMADDL moves the guest's OAM port */
  p.oamAdr = 0x41;                  /* ... which a following $2104 advanced */

  /* Render field A. */
  ppu_rasterRenderBegin(&p);
  CHECK(p.inidisp == 0x80 && p.window1left == 0xFF);   /* field A's start */
  ppu_rasterApplyLine(&p, 0);
  CHECK(p.inidisp == 0x0F && p.window1left == 0xFF);   /* vblank write */
  ppu_rasterApplyLine(&p, 21);
  CHECK(p.window1left == 0xFF);
  ppu_rasterApplyLine(&p, 22);
  CHECK(p.window1left == 0x14 && p.window1right == 0xEB);
  ppu_rasterApplyLine(&p, 198);
  CHECK(p.inidisp == 0x0F);
  ppu_rasterApplyLine(&p, 199);
  CHECK(p.inidisp == 0x80 && p.window1left == 0xFF);
  ppu_rasterApplyLine(&p, 224);
  /* Field B's writes are not part of field A's picture. */
  CHECK(p.window1left == 0xFF);
  ppu_rasterRenderEnd(&p);
  /* The guest resumes with the register file it last wrote ... */
  CHECK(p.window1left == 0x20 && p.oamaddl == 0x40);
  /* ... and its OAM port where its own writes left it. */
  CHECK(p.oamAdr == 0x41);

  /* Writes made during a render (replay, render-walk HDMA) are not new
   * history: field B must hold only its two entries, so rendering it after
   * the next boundary starts from field B's baseline. */
  s_snes.vPos = 225;
  ppu_rasterFieldBoundary();
  ppu_rasterRenderBegin(&p);
  CHECK(p.window1left == 0xFF);      /* field B started at field A's end */
  ppu_rasterApplyLine(&p, 10);
  CHECK(p.window1left == 0x20);
  ppu_rasterRenderEnd(&p);

  /* ER-06: line 224 is beyond the driver's last ApplyLine(223). Those
   * writes still belong to the guest and to the next field's baseline. */
  ppu_rasterReset();
  write_at(225, 0x2c, 0x04);
  write_at(225, 0x2d, 0x01);
  ppu_rasterBegin(&p);
  write_at(224, 0x2c, 0x17);
  write_at(224, 0x2d, 0x00);
  write_at(224, 0x0d, 0x35);
  write_at(224, 0x0d, 0x02);
  write_at(224, 0x1b, 0x43);
  write_at(224, 0x1b, 0x01);
  p.mosaicStartLine = 7;
  p.oamAdr = 0x42;
  p.oamInHigh = true;
  p.oamSecondWrite = true;
  p.oamBuffer = 0x56;
  s_snes.vPos = 225;
  ppu_rasterFieldBoundary();
  {
    uint8_t guest_regs[PPU_SAVESTATE_REGS_SIZE];
    const uint8_t scroll = p.scrollPrev, scroll2 = p.scrollPrev2, m7 = p.m7prev;
    memcpy(guest_regs, &p.inidisp, sizeof(guest_regs));
    ppu_rasterRenderBegin(&p);
    ppu_rasterApplyLine(&p, 223);
    CHECK(p.screenEnabled[0] == 0x04 && p.screenEnabled[1] == 0x01);
    /* Render-walk writes must not leak their latch state to the guest. */
    ppu_write(&p, 0x0d, 0x99);
    ppu_write(&p, 0x1b, 0x88);
    ppu_write(&p, 0x06, 0x11);
    ppu_write(&p, 0x02, 0x11);
    ppu_rasterRenderEnd(&p);
    CHECK(memcmp(guest_regs, &p.inidisp, sizeof(guest_regs)) == 0);
    CHECK(p.scrollPrev == scroll && p.scrollPrev2 == scroll2 && p.m7prev == m7);
    CHECK(p.mosaicStartLine == 7);
    CHECK(p.oamAdr == 0x42 && p.oamInHigh && p.oamSecondWrite && p.oamBuffer == 0x56);
    /* Two more fields with no new mask writes must not alternate baselines. */
    for (int field = 0; field < 2; field++) {
      s_snes.vPos = 225;
      ppu_rasterFieldBoundary();
      ppu_rasterRenderBegin(&p);
      ppu_rasterApplyLine(&p, 223);
      CHECK(p.screenEnabled[0] == 0x17 && p.screenEnabled[1] == 0x00);
      ppu_rasterRenderEnd(&p);
      CHECK(memcmp(guest_regs, &p.inidisp, sizeof(guest_regs)) == 0);
    }
  }

  test_oam_field_timeline();
  test_oam_commit_and_boundary();

  /* A machine reset disarms and does not restore stale OAM. */
  ppu_rasterReset();
  p.oam[0] = 0x7654;
  write_at(50, 0x26, 0x77);
  ppu_rasterRenderBegin(&p);
  CHECK(p.window1left == 0x77);
  CHECK(p.oam[0] == 0x7654);
  ppu_rasterRenderEnd(&p);

  printf("ppu_raster_journal_test: ok\n");
  return 0;
}
