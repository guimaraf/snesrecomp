#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "snes/dma.h"
#include "snes/ppu.h"
#include "snes/snes.h"

Ppu *g_ppu;
bool g_fail;

static uint8_t bus[0x1000000];
static uint8_t bbus[0x100];
static unsigned bbus_writes;
static struct {
    unsigned register_writes, init_before, init_after;
    uint16_t last_address;
    uint8_t last_value;
    DmaChannel before, after;
} trace_probe;

static void trace_dma(DmaTraceKind kind, const Dma *dma,
                      uint16_t address, uint8_t value) {
    if (kind == kDmaTraceRegisterWrite) {
        trace_probe.register_writes++;
        trace_probe.last_address = address;
        trace_probe.last_value = value;
    } else if (kind == kDmaTraceHdmaInitBefore) {
        trace_probe.init_before++;
        trace_probe.before = dma->channel[0];
    } else if (kind == kDmaTraceHdmaInitAfter) {
        trace_probe.init_after++;
        trace_probe.after = dma->channel[0];
    }
}

static int check(bool condition, const char *message) {
    if (!condition) fprintf(stderr, "FAIL: %s\n", message);
    return condition ? 0 : 1;
}

/* The PPU write journal (ppu.c) is not linked into this harness; dma.c only
 * tags who is writing. */
uint8_t g_ppu_wlog_src;

uint8_t snes_read(Snes *snes, uint32_t adr) {
    (void)snes;
    return bus[adr & 0xffffffu];
}

void snes_write(Snes *snes, uint32_t adr, uint8_t val) {
    (void)snes;
    bus[adr & 0xffffffu] = val;
}

uint8_t snes_readBBus(Snes *snes, uint8_t adr) {
    (void)snes;
    return bbus[adr];
}

void snes_writeBBus(Snes *snes, uint8_t adr, uint8_t val) {
    (void)snes;
    bbus[adr] = val;
    bbus_writes++;
}

static void clear_bus(void) {
    memset(bus, 0, sizeof bus);
    memset(bbus, 0, sizeof bbus);
    bbus_writes = 0;
}

static int test_direct_hdma(void) {
    Snes snes = {0};
    Dma *dma = dma_init(&snes);
    int failures = 0;
    if (!dma) return 1;
    snes.dma = dma;
    dma_reset(dma);
    clear_bus();

    bus[0x808000] = 0x82; /* two lines, transfer on each line */
    bus[0x808001] = 0x11;
    bus[0x808002] = 0x22;
    bus[0x808003] = 0x00;

    dma_write(dma, 0x4300, 0x00);
    dma_write(dma, 0x4301, 0x26);
    dma_write(dma, 0x4302, 0x00);
    dma_write(dma, 0x4303, 0x80);
    dma_write(dma, 0x4304, 0x80);
    dma_startDma(dma, 0x01, true);
    dma_initHdma(dma);

    failures += check(dma->channel[0].hdmaActive, "frame init must not clear hdmaActive");
    failures += check(!dma->channel[0].terminated, "enabled channel should start active");

    dma_doHdma(dma);
    failures += check(bbus[0x26] == 0x11, "direct line 1 wrote first table byte");
    failures += check(dma->channel[0].tableAdr == 0x8002, "direct line 1 advanced table");
    failures += check(dma->channel[0].repCount == 0x81, "direct line 1 decremented repeat count");
    failures += check(dma->channel[0].doTransfer, "direct line 1 reports transfer");

    dma_doHdma(dma);
    failures += check(bbus[0x26] == 0x22, "direct line 2 wrote second table byte");
    failures += check(dma->channel[0].tableAdr == 0x8004, "direct line 2 consumed terminator");
    failures += check(dma->channel[0].repCount == 0x00, "direct line 2 loaded zero line count");
    failures += check(dma->channel[0].terminated, "direct line 2 terminates channel");
    failures += check(!dma->channel[0].doTransfer, "direct terminator reports no transfer");

    dma_doHdma(dma);
    failures += check(dma->channel[0].terminated, "zero line count terminates channel");
    failures += check(!dma->channel[0].doTransfer, "terminator reports no transfer");
    failures += check(bbus_writes == 2, "terminator does not write B-bus");

    dma_free(dma);
    return failures;
}

static int test_indirect_hdma(void) {
    Snes snes = {0};
    Dma *dma = dma_init(&snes);
    int failures = 0;
    if (!dma) return 1;
    snes.dma = dma;
    dma_reset(dma);
    clear_bus();

    bus[0x818100] = 0x81; /* one line, mode 1 transfers two bytes */
    bus[0x818101] = 0x34;
    bus[0x818102] = 0x12;
    bus[0x7e1234] = 0xaa;
    bus[0x7e1235] = 0xbb;

    dma_write(dma, 0x4310, 0x41);
    dma_write(dma, 0x4311, 0x0d);
    dma_write(dma, 0x4312, 0x00);
    dma_write(dma, 0x4313, 0x81);
    dma_write(dma, 0x4314, 0x81);
    dma_write(dma, 0x4317, 0x7e);
    dma_startDma(dma, 0x02, true);
    dma_initHdma(dma);
    dma_doHdma(dma);

    failures += check(bbus[0x0d] == 0xaa, "indirect mode wrote first byte");
    failures += check(bbus[0x0e] == 0xbb, "indirect mode wrote offset byte");
    failures += check(dma->channel[1].terminated, "indirect line consumed terminator");
    failures += check(!dma->channel[1].doTransfer, "indirect terminator reports no transfer");
    failures += check(dma->channel[1].size == 0x1236, "indirect pointer advanced by transfer length");
    failures += check(dma->channel[1].tableAdr == 0x8104, "indirect table consumed terminator");

    dma_free(dma);
    return failures;
}

static int test_repeat_skip_hdma(void) {
    Snes snes = {0};
    Dma *dma = dma_init(&snes);
    int failures = 0;
    if (!dma) return 1;
    snes.dma = dma;
    dma_reset(dma);
    clear_bus();

    bus[0x828200] = 0x02; /* two lines, transfer only on first line */
    bus[0x828201] = 0x77;
    bus[0x828202] = 0x00;
    bus[0x828203] = 0x00;

    dma_write(dma, 0x4320, 0x00);
    dma_write(dma, 0x4321, 0x2c);
    dma_write(dma, 0x4322, 0x00);
    dma_write(dma, 0x4323, 0x82);
    dma_write(dma, 0x4324, 0x82);
    dma_startDma(dma, 0x04, true);
    dma_initHdma(dma);

    dma_doHdma(dma);
    failures += check(bbus[0x2c] == 0x77, "non-repeat descriptor transfers first line");
    failures += check(!dma->channel[2].doTransfer, "non-repeat first line arms skip");
    failures += check(dma->channel[2].repCount == 0x01, "non-repeat first line decremented count");

    dma_doHdma(dma);
    failures += check(bbus[0x2c] == 0x77, "non-repeat descriptor skips second line");
    failures += check(dma->channel[2].terminated, "non-repeat skipped line consumed terminator");
    failures += check(!dma->channel[2].doTransfer, "non-repeat terminator reports no transfer");
    failures += check(dma->channel[2].tableAdr == 0x8203, "skipped line consumed terminator only");
    failures += check(dma->channel[2].repCount == 0x00, "non-repeat second line loaded zero line count");

    dma_doHdma(dma);
    failures += check(dma->channel[2].terminated, "non-repeat terminator reached after skipped line");
    failures += check(bbus_writes == 1, "non-repeat descriptor wrote once");

    dma_free(dma);
    return failures;
}

static int test_mid_frame_enable_disable(void) {
    Snes snes = {0};
    Dma *dma = dma_init(&snes);
    int failures = 0;
    if (!dma) return 1;
    snes.dma = dma;
    dma_reset(dma);
    clear_bus();

    bus[0x838300] = 0x81;
    bus[0x838301] = 0x55;
    bus[0x838302] = 0x00;

    dma_write(dma, 0x4330, 0x00);
    dma_write(dma, 0x4331, 0x2d);
    dma_write(dma, 0x4332, 0x00);
    dma_write(dma, 0x4333, 0x83);
    dma_write(dma, 0x4334, 0x83);

    dma_startDma(dma, 0x00, true);
    dma_initHdma(dma);
    dma_startDma(dma, 0x08, true);
    dma_doHdma(dma);
    failures += check(bbus_writes == 0, "mid-frame enable spends first slot on init");
    failures += check(!dma->channel[3].terminated, "mid-frame enabled channel initializes live");
    failures += check(dma->channel[3].doTransfer, "mid-frame enabled channel arms transfer");
    failures += check(dma->channel[3].tableAdr == 0x8301, "mid-frame init loaded table header");

    dma_initHdma(dma);
    dma_doHdma(dma);
    failures += check(bbus[0x2d] == 0x55, "next frame init arms newly enabled channel");
    failures += check(bbus_writes == 1, "newly enabled channel writes after frame init");

    dma_startDma(dma, 0x00, true);
    dma_doHdma(dma);
    failures += check(bbus_writes == 1, "mid-frame disable suppresses further HDMA");
    failures += check(!dma->channel[3].hdmaActive, "mid-frame disable clears enable bit");

    dma_free(dma);
    return failures;
}

static int test_trace_observer(void) {
    Snes snes = {0};
    Dma *dma = dma_init(&snes);
    int failures = 0;
    if (!dma) return 1;
    snes.dma = dma;
    dma_reset(dma);
    clear_bus();
    memset(&trace_probe, 0, sizeof trace_probe);
    bus[0x808000] = 0x81;

    dma_set_trace_observer(trace_dma);
    dma_write(dma, 0x4300, 0x00);
    dma_write(dma, 0x4301, 0x26);
    dma_write(dma, 0x4302, 0x00);
    dma_write(dma, 0x4303, 0x80);
    dma_write(dma, 0x4304, 0x80);
    dma_startDma(dma, 0x01, true);
    dma_trace_register_write(dma, 0x420c, 0x01);
    dma_initHdma(dma);
    dma_set_trace_observer(NULL);

    failures += check(trace_probe.register_writes == 6,
                      "trace reports five channel writes and the HDMA mask write");
    failures += check(trace_probe.last_address == 0x420c &&
                      trace_probe.last_value == 0x01,
                      "trace records the HDMA enable register and value");
    failures += check(trace_probe.init_before == 1 && trace_probe.init_after == 1,
                      "trace brackets one HDMA frame initialization");
    failures += check(trace_probe.before.aAdr == 0x8000 &&
                      trace_probe.before.tableAdr == 0x8000,
                      "pre-init snapshot retains the programmed table pointer");
    failures += check(trace_probe.after.tableAdr == 0x8001 &&
                      trace_probe.after.repCount == 0x81 &&
                      !trace_probe.after.terminated,
                      "post-init snapshot contains the loaded descriptor state");

    dma_free(dma);
    return failures;
}

int main(void) {
    int failures = 0;
    failures += test_direct_hdma();
    failures += test_indirect_hdma();
    failures += test_repeat_skip_hdma();
    failures += test_mid_frame_enable_disable();
    failures += test_trace_observer();
    if (failures) return 1;
    puts("hdma_test: ok");
    return 0;
}
