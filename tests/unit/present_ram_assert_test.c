/* tests/unit/present_ram_assert_test.c - the present-thread write refusal
 * of core/psx/cpu.c (OPENPETE_PRESENT_RAM_ASSERT).
 *
 * A thread that binds `present` resolves guest RAM through the resolvers and
 * reads the bytes the sim side wrote; a store through such a pointer must
 * abort the process with the named message (the negative control, run in a
 * forked child); the main thread, which holds no `present` bit, keeps
 * writing through the same resolvers.
 *
 * Compiles cpu.c, psx_region.c and thread_roles.c with stubs for the CD-ROM
 * register model, the BIOS ROM window and the logger. */

#include "openpete_psx.h"
#include "openpete_log.h"
#include "openpete_thread_roles.h"
#include "psx_runtime.h"

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/** @brief Stub CD-ROM register read: every register is unmapped here. */
int openpete_cdrom_mmio_read8(uint32_t phys, uint8_t* out) { (void)phys; (void)out; return 0; }
/** @brief Stub CD-ROM register write: every register is unmapped here. */
int openpete_cdrom_mmio_write8(uint32_t phys, uint8_t value) { (void)phys; (void)value; return 0; }
/** @brief Stub BIOS ROM window: no byte of it is mapped here. */
int openpete_psx_bios_rom_read8(uint32_t phys, uint8_t* out) { (void)phys; (void)out; return 0; }
/** @brief Stub logger sink. */
void openpete_logf(openpete_log_level_t level, const char* subsystem, const char* fmt, ...) { (void)level; (void)subsystem; (void)fmt; }

#define WORD_ADDR 0x80010000u
#define WORD_VALUE 0x5EED1234u

/* What the present-role thread saw, for the main thread to check. */
typedef struct {
    int       write;      /* 1: store through the pointer (the negative control) */
    uint32_t  read;       /* the word read through the resolved pointer */
    uintptr_t ptr;        /* the pointer the resolver returned */
    uintptr_t ram;        /* openpete_psx_get_ram() on that thread */
} probe_t;

/** @brief The present-thread stand-in: binds `present`, resolves the guest word, reads it, and stores to it when asked.
 *
 * @param arg The probe_t to fill.
 * @return NULL. */
static void* present_stub_main(void* arg) {
    OP_ROLE_BIND(present);
    probe_t* pr = (probe_t*)arg;
    volatile uint32_t* w = (volatile uint32_t*)openpete_psx_to_host_span(WORD_ADDR, 4u);
    pr->ptr = (uintptr_t)w;
    pr->ram = (uintptr_t)openpete_psx_get_ram();
    pr->read = w ? *w : 0u;
    if (pr->write && w) *w = pr->read;   /* write-and-restore: changes no byte */
    return NULL;
}

/** @brief Run the stand-in on a new thread and join it.
 *
 * @param pr The probe to fill. */
static void run_probe(probe_t* pr) {
    pthread_t t;
    if (pthread_create(&t, NULL, present_stub_main, pr) != 0) { perror("pthread_create"); exit(1); }
    pthread_join(t, NULL);
}

/** @brief Fork a child whose present-role thread stores through its pointer; the child must die of SIGABRT naming the address.
 *
 * @return 0 when the refusal fired as specified, else 1. */
static int negative_control(void) {
    int fds[2];
    if (pipe(fds) != 0) { perror("pipe"); return 1; }
    const pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        dup2(fds[1], 2);
        close(fds[0]);
        probe_t pr = { 1, 0, 0, 0 };
        run_probe(&pr);
        _exit(0);   /* reached only when the store was not refused */
    }
    close(fds[1]);
    char buf[512];
    size_t n = 0;
    ssize_t r;
    while (n < sizeof buf - 1 && (r = read(fds[0], buf + n, sizeof buf - 1 - n)) > 0) n += (size_t)r;
    buf[n] = '\0';
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    const int aborted = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
    const int named = strstr(buf, "present thread wrote guest RAM [80010000]") != NULL;
    printf("negative control: child %s, message %s\n",
           aborted ? "aborted (SIGABRT)" : "was NOT aborted",
           named ? "names [80010000]" : "missing");
    if (!named && n) fprintf(stderr, "child stderr: %s\n", buf);
    return (aborted && named) ? 0 : 1;
}

/** @brief Runs the positive checks, then the negative control.
 *
 * @return 0 on success. */
int main(void) {
    setenv("OPENPETE_PRESENT_RAM_ASSERT", "1", 1);
    static CPUState cpu;
    openpete_psx_runtime_init(&cpu);
    int fail = 0;

    /* The sim side (no `present` bit) writes through the resolver and the bus. */
    uint8_t* ram = openpete_psx_get_ram();
    volatile uint32_t* w = (volatile uint32_t*)openpete_psx_to_host_span(WORD_ADDR, 4u);
    if (!w || (uint8_t*)w != ram + 0x10000u) { fprintf(stderr, "FAIL: sim-side pointer is not g_ram\n"); fail++; }
    else *w = WORD_VALUE;
    cpu.write_word(WORD_ADDR + 4u, WORD_VALUE + 1u);

    /* A present-role thread reads the same bytes through a different pointer. */
    probe_t pr = { 0, 0, 0, 0 };
    run_probe(&pr);
    if (pr.read != WORD_VALUE) { fprintf(stderr, "FAIL: present-side read %08x, want %08x\n", pr.read, WORD_VALUE); fail++; }
    if (pr.ptr == (uintptr_t)w || pr.ram == (uintptr_t)ram) { fprintf(stderr, "FAIL: present-side pointer is the writable array\n"); fail++; }
    if (pr.ptr != pr.ram + 0x10000u) { fprintf(stderr, "FAIL: present-side resolver and get_ram disagree\n"); fail++; }
    if (cpu.read_word(WORD_ADDR + 4u) != WORD_VALUE + 1u) { fprintf(stderr, "FAIL: bus write lost\n"); fail++; }

    fail += negative_control();
    printf("%s\n", fail ? "FAIL" : "PASS");
    return fail ? 1 : 0;
}
