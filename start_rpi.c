/*
    Copyright © 2019 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#include <stdarg.h>
#include <stdint.h>
#include "config.h"
#include "support_rpi.h"
#include "tlsf.h"
#include "devicetree.h"
#include "M68k.h"
#include "HunkLoader.h"
#include "DuffCopy.h"
#include "EmuLogo.h"

#undef ARM_PERIIOBASE
#define ARM_PERIIOBASE (__arm_periiobase)

void _start();

asm("   .section .startup           \n"
"       .globl _start               \n"
"       .type _start,%function      \n"
"_start:                            \n"
"       mrs     r4, cpsr_all        \n" /* Check if in hypervisor mode */
"       and     r4, r4, #0x1f       \n"
"       mov     r8, #0x1a           \n"
"       cmp     r4, r8              \n"
"       beq     leave_hyper         \n"
"continue_boot:                     \n"
"       cps     #0x13               \n" /* Should be in SVC (supervisor) mode already, but just incase.. */
#if EMU68_HOST_BIG_ENDIAN
"       setend  be                  \n" /* Switch to big endian mode */
#endif
"       ldr     sp, tmp_stack_ptr   \n"
"       mrc     p15,0,r4,c1,c0,2    \n" /* Enable signle and double VFP coprocessors */
"       orr     r4, r4, #0x00f00000 \n" /* This is necessary since gcc might want to use vfp registers  */
"       mcr     p15,0,r4,c1,c0,2    \n" /* Either as cache for general purpose regs or e.g. for division. This is the case with gcc9 */
"       mov     r4,#0x40000000      \n"
"       fmxr    fpexc,r4            \n" /* Enable VFP now */
"       ldr     r4, mmu_table_ptr   \n" /* Load MMU table pointer */
"       mcr     p15,0,r4,c2,c0,0    \n" /* Write page_dir address to ttbr0 */
"       mov     r8, #0              \n"
"       mcr     p15,0,r8,c2,c0,2    \n" /* Write ttbr control N = 0 (use only ttbr0) */
"       mov     r4, #1              \n"
"       mcr     p15,0,r4,c3,c0,0    \n" /* Set domains - Dom0 is usable, rest is disabled */
"       mrc     p15,0,r4,c1,c0,0    \n" /* Load control register */
"       orr     r4,r4,#8388608      \n" /* v6 page tables, subpages disabled */
"       orr     r4,r4,#1            \n" /* Enable MMU */
#if EMU68_HOST_BIG_ENDIAN
"       orr     r4,r4,#1<<25        \n" /* MMU tables in big endian */
#endif
"       mcr     p15,0,r8,c7,c10,4   \n" /* DSB */
"       mcr     p15,0,r4,c1,c0,0    \n" /* Set control register and thus really enable mmu */
"       ldr     r4, boot_address    \n"
"       mcr     p15,0,r8,c7,c5,4    \n" /* ISB */
"       bx      r4                  \n"
"leave_hyper:                       \n"
#if EMU68_HOST_BIG_ENDIAN
"       setend  be                  \n"
#endif
"       adr     r4, continue_boot   \n"
"       .byte   0x04,0xf3,0x2e,0xe1 \n" /* msr     ELR_hyp, r4  */
"       mrs     r4, cpsr_all        \n"
"       and     r4, r4, #0x1f       \n"
"       orr     r4, r4, #0x13       \n"
"       .byte   0x04,0xf3,0x6e,0xe1 \n" /* msr     SPSR_hyp, r4 */
"       .byte   0x6e,0x00,0x60,0xe1 \n" /* eret                 */
"       .section .text              \n"
".byte 0                            \n"
#if EMU68_HOST_BIG_ENDIAN
".string \"$VER: Emu68.img v0.1 (" __DATE__ ") BigEndian\"\n"
#else
".string \"$VER: Emu68.img v0.1 (" __DATE__ ")\"\n"
#endif
".byte 0                            \n"
"\n\t\n\t"
);

#if EMU68_HOST_BIG_ENDIAN
static const char bootstrapName[] = "Emu68 runtime/ARM v7-a BigEndian";
#else
static const char bootstrapName[] = "Emu68 runtime/ARM v7-a LittleEndian";
#endif

static __attribute__((used)) void * tmp_stack_ptr __attribute__((used, section(".startup"))) = (void *)(0xff800000 + 0x1000 - 16);
extern int __bootstrap_end;

int raise(int sig)
{
    kprintf("[BOOT] called raise(%d)\n", sig);
    (void)sig;
    return 0;
}

void bzero(void *ptr, long sz)
{
    char *p = ptr;
    if (p)
        while(sz--)
            *p++ = 0;
}

void memset(void *ptr, uint8_t fill, long sz)
{
    uint8_t *p = ptr;
    if (p)
        while(sz--)
            *p++ = fill;
}

void memcpy(void *dst, const void *src, long sz)
{
    kprintf("[BOOT] called memcpy(%08x, %08x, %08x)\n", dst, src, sz);
}

void mmap()
{
    kprintf("[BOOT] called mmap");
}

void printf(const char * restrict format, ...)
{
    va_list v;
    va_start(v, format);
    vkprintf(format, v);
    va_end(v);
}

/*
    Initial MMU map covers first 8 MB of RAM and shadows this RAM at topmost 8 MB of address space. Peripherals will be mapped
    at 0xf2000000 - 0xf2ffffff.

    After successfull start the topmost 8MB of memory will be used for emulation and the code will be moved there. Once ready,
    the MMU map will be updated accordingly
*/
static __attribute__((used, section(".mmu"))) uint32_t mmu_table[4096] = {
    [0x000] = 0x00001c0e,   /* caches write-through, write allocate, access for all */
    [0x001] = 0x00101c0e,
    [0x002] = 0x00201c0e,
    [0x003] = 0x00301c0e,
    [0x004] = 0x00401c0e,
    [0x005] = 0x00501c0e,
    [0x006] = 0x00601c0e,
    [0x007] = 0x00701c0e,

    [0xff8] = 0x00001c0e,   /* shadow of first 8 MB with the same attributes */
    [0xff9] = 0x00101c0e,
    [0xffa] = 0x00201c0e,
    [0xffb] = 0x00301c0e,
    [0xffc] = 0x00401c0e,
    [0xffd] = 0x00501c0e,
    [0xffe] = 0x00601c0e,
    [0xfff] = 0x00701c0e
};

/* Trivial virtual to physical translator, fetches data from MMU table and assumes 1M pages */
uint32_t virt2phys(uint32_t virt_addr)
{
    uint32_t page = virt_addr >> 20;
    uint32_t offset = virt_addr & 0x000fffff;

    offset |= mmu_table[page] & 0xfff00000;

    kprintf("virt2phys(%08x) -> %08x\n", virt_addr, offset);

    return offset;
}

static __attribute__((used)) void * mmu_table_ptr __attribute__((used, section(".startup"))) = (void *)((uintptr_t)mmu_table - 0xff800000);

void *tlsf;

void start_emu(void *);

int ARM_SUPPORTS_DIV = 0;

void boot(uintptr_t dummy, uintptr_t arch, uintptr_t atags, uintptr_t dummy2)
{
    (void)dummy; (void)arch; (void)atags; (void)dummy2;
    uint32_t tmp, initcr;
    uint32_t isar;
    of_node_t *e;

    /*
     * Enable caches and branch prediction. Also enabled unaligned memory
     * access. Exceptions are set to run in big-endian mode and this is the mode
     * in which page tables are written if the system is set to run in BE.
     */
    asm volatile ("mrc p15, 0, %0, c1, c0, 0" : "=r"(initcr));
    tmp = initcr;
    tmp |= (1 << 2) | (1 << 12) | (1 << 11);    /* I and D caches, branch prediction */
    tmp = (tmp & ~2) | (1 << 22);               /* Unaligned access enable */
#if EMU68_HOST_BIG_ENDIAN
    tmp |= (1 << 25);                           /* EE bit for exceptions set - big endian */
                                                /* This bit sets also endianess of page tables */
#endif
    asm volatile ("mcr p15, 0, %0, c1, c0, 0" : : "r"(tmp));


    asm volatile ("mrc p15, 0, %0, c0, c2, 0" : "=r"(isar));
    if ((isar & 0x0f000000) == 0x02000000)
        ARM_SUPPORTS_DIV = 1;

    /* Create 1:1 map for whole memory in order to access the device tree, which can reside anywhere in ram */
    for (int i=8; i < 4096-8; i++)
    {
        /* Caches write-through, write allocate, access for all */
        mmu_table[i] = (i << 20) | 0x1c0e;
    }

    arm_flush_cache((uint32_t)mmu_table, sizeof(mmu_table));

    tlsf = tlsf_init();
    tlsf_add_memory(tlsf, &__bootstrap_end, 0xffff0000 - (uintptr_t)&__bootstrap_end);
    dt_parse((void*)atags);

    /* Prepare mapping for peripherals. Use the data from device tree here */
    e = dt_find_node("/soc");
    if (e)
    {
        of_property_t *p = dt_find_property(e, "ranges");
        uint32_t *ranges = p->op_value;
        int32_t len = p->op_length;
        uint32_t start_map = 0xf20;

        while(len > 0)
        {
            uint32_t addr_bus, addr_cpu;
            uint32_t addr_len;

            addr_bus = BE32(*ranges++);
            addr_cpu = BE32(*ranges++);
            addr_len = BE32(*ranges++);

            (void)addr_bus;

            if (addr_len < 0x00100000)
                addr_len = 0x00100000;

            /* Prepare mapping - device type */
            for (unsigned i=0; i < (addr_len >> 20); i++)
            {
                /* Strongly-ordered device, uncached, 16MB region */
                mmu_table[start_map + i] = (i << 20) | addr_cpu | 0x0c06;
            }

            ranges[-2] = BE32(start_map << 20);

            start_map += addr_len >> 20;

            len -= 12;
        }
    }

    arm_flush_cache((uint32_t)mmu_table, sizeof(mmu_table));

    setup_serial();

    struct Size sz;

    kprintf("[BOOT] Booting %s\n", bootstrapName);
    kprintf("[BOOT] Boot address is %08x\n", _start);
    kprintf("[BOOT] Bootstrap ends at %08x\n", &__bootstrap_end);
    kprintf("[BOOT] ISAR=%08x\n", isar);
    kprintf("[BOOT] Args=%08x,%08x,%08x,%08x\n", dummy, arch, atags, dummy2);
    kprintf("[BOOT] Local memory pool:\n");
    kprintf("[BOOT]    %08x - %08x (size=%d)\n", &__bootstrap_end, 0xffff0000, 0xffff0000 - (uintptr_t)&__bootstrap_end);

    e = dt_find_node("/memory");
    if (e)
    {
        of_property_t *p = dt_find_property(e, "reg");
        uint32_t *range = p->op_value;

        uint32_t top_of_ram = BE32(range[0]) + BE32(range[1]);
        uint32_t kernel_new_loc = top_of_ram - 0x00800000;

        range[1] = BE32(BE32(range[1])-0x00800000);

        kprintf("[BOOT] System memory: %p-%p\n", BE32(range[0]), BE32(range[0]) + BE32(range[1]) - 1);

        kprintf("[BOOT] Adjusting MMU map\n");

        for (int i=0; i < 8; i++)
        {
            /* Caches write-through, write allocate, access for all */
            mmu_table[0xff8 + i] = ((kernel_new_loc & 0xfff00000) + (i << 20)) | 0x1c0e;
        }
        kprintf("[BOOT] Moving kernel to %p\n", (void*)kernel_new_loc);
        DuffCopy((void*)(kernel_new_loc+4), (void*)4, 0x00800000 / 4 - 1);
        arm_flush_cache(kernel_new_loc, 0x00800000);

        asm volatile("dsb; mcr p15,0,%0,c2,c0,0; dsb"::"r"(((uint32_t)mmu_table_ptr & 0x000fffff) | (kernel_new_loc & 0xfff00000)));
        asm volatile("dsb; mcr p15, 0, %0, c8, c7, 0; dsb"::"r"(0));
    }

    if (get_max_clock_rate(3) != get_clock_rate(3)) {
        kprintf("[BOOT] Changing ARM clock rate from %d MHz to %d MHz\n", get_clock_rate(3)/1000000, get_max_clock_rate(3)/1000000);
        set_clock_rate(3, get_max_clock_rate(3));
    }

    sz = get_display_size();
    kprintf("[BOOT] Display size is %dx%d\n", sz.widht, sz.height);

    e = dt_find_node("/chosen");
    if (e)
    {
        void *image_start, *image_end;
        of_property_t *p = dt_find_property(e, "linux,initrd-start");
        image_start = (void*)BE32(*(uint32_t*)p->op_value);
        p = dt_find_property(e, "linux,initrd-end");
        image_end = (void*)BE32(*(uint32_t*)p->op_value);

        kprintf("[BOOT] Loading executable from %p-%p\n", image_start, image_end);
        void *hunks = LoadHunkFile(image_start);
        start_emu((void *)((uint32_t)hunks + 4));
    }

    while(1);
}

static __attribute__((used)) void * boot_address __attribute__((used, section(".startup"))) = (void *)((intptr_t)boot);


uint8_t m68kcode[] = {
/*
    0x7c, 0x20,
    0x7e, 0xff,
    0x52, 0x80, 0x52, 0x80, 0x52, 0x80,
    0x52, 0x80, 0x52, 0x80, 0x52, 0x80,
    0x52, 0x80, 0x52, 0x80, 0x52, 0x80,
    0x52, 0x80, 0x52, 0x80, 0x52, 0x80,
    0x52, 0x80, 0x52, 0x80, 0x52, 0x80,
    0x52, 0x80, 0x52, 0x80, 0x52, 0x80,
    0x52, 0x80, 0x52, 0x80, 0x52, 0x80,
    0x52, 0x80, 0x52, 0x80, 0x52, 0x80,
    0x52, 0x80, 0x52, 0x80, 0x52, 0x80,
    0x52, 0x80, 0x52, 0x80, 0x52, 0x80,
    0x52, 0x80, 0x52, 0x80, 0x52, 0x80,
    0x52, 0x80, 0x52, 0x80, 0x52, 0x80,
    0x52, 0x80, 0x52, 0x80, 0x52, 0x80,
    0x52, 0x80, 0x52, 0x80, 0x52, 0x80,
    0x52, 0x80, 0x52, 0x80, 0x52, 0x80,
    0x52, 0x80, 0x52, 0x80, 0x52, 0x80,
    0x51, 0xcf, 0xff, 0x9e,
    0x51, 0xce, 0xff, 0x98,
    0x4e, 0x75,
    0xff, 0xff
*/
    /*0x74,0x08,
    0x20,0x3c,0xaa,0x55,0xaa,0x55,
    0x22,0x3c,0x87,0x65,0x43,0x21,
    0xe5,0xb8,
    0xe4,0xb9,
    0x4e,0x75,*/
    /*
    0x20,0x3c,0xde,0xad,0xbe,0xef,
    0x22,0x3c,0x12,0x34,0x56,0x78,
    0x34,0x3c,0xaa,0x55,
    0x16,0x3c,0x00,0xc7,
    0x4e,0x75,
*/
    0x2a,0x49,  // movea.l  a1,a5
    0x2c,0x4a,  // movea.l  a2,a6
    0x22,0x5e,  //           	movea.l (a6)+,a1
    0x24,0x5e,  //           	movea.l (a6)+,a2
    0x26,0x5e,  //           	movea.l (a6)+,a3
    0x28,0x56,  //           	movea.l (a6),a4
    0x2c,0x3c,0x55,0x55 ,0x55,0x55, // 	move.l #1431655765,d6
    0x2e,0x3c ,0x33,0x33,0x33,0x33, // 	move.l #858993459,d7
    0x28,0x3c ,0x0f,0x0f ,0x0f,0x0f,// 	loop: move.l #252645135,d4
    0x2a,0x3c ,0x00,0xff ,0x00,0xff, //	move.l #16711935,d5
    0x20,0x18, // move.l (a0)+, d0
    0xc0,0x84, //           	and.l d4,d0
    0x22,0x18, // 	move.l (a0)+,d1
    0xe9,0x88, //           	lsl.l #4,d0
    0xc2,0x84, //           	and.l d4,d1
    0x80,0x81, //           	or.l d1,d0
    0x22,0x18, // 	move.l (a0)+,d1
    0xc2,0x84, //           	and.l d4,d1
    0x24,0x18, // 	move.l (a0)+,d2
    0xe9,0x89, //           	lsl.l #4,d1
    0xc4,0x84, //           	and.l d4,d2
    0x82,0x82, //           	or.l d2,d1
    0x24,0x00, //           	move.l d0,d2
    0x26,0x01, //           	move.l d1,d3
    0xc0,0x85, //           	and.l d5,d0
    0xc6,0x85, //           	and.l d5,d3
    0xb1,0x82, //           	eor.l d0,d2
    0xb7,0x81, //           	eor.l d3,d1
    0xe1,0x88, //           	lsl.l #8,d0
    0xe0,0x89, //           	lsr.l #8,d1
    0x80,0x83, //           	or.l d3,d0
    0x82,0x82, //           	or.l d2,d1
    0x24,0x00, //           	move.l d0,d2
    0x26,0x01, //           	move.l d1,d3
    0xc0,0x86, //           	and.l d6,d0
    0xc6,0x86, //           	and.l d6,d3
    0xb1,0x82, //           	eor.l d0,d2
    0xb7, 0x81,//           	eor.l d3,d1
    0xd6,0x83, //           	add.l d3,d3
    0xe2,0x8a, //           	lsr.l #1,d2
    0x80,0x83, //           	or.l d3,d0
    0x82,0x82, //           	or.l d2,d1
    0x24,0x18, // 	move.l (a0)+,d2
    0xc4,0x84, //           	and.l d4,d2
    0x26,0x18, // 	move.l (a0)+,d3
    0xe9,0x8a, //           	lsl.l #4,d2
    0xc6,0x84, //           	and.l d4,d3
    0x84,0x83, //           	or.l d3,d2
    0x26,0x18, // 	move.l (a0)+,d3
    0xc6,0x84, //           	and.l d4,d3
    0xc8,0x98, // 	move.l (a0)+,d4
    0xe9,0x8b, //           	lsl.l #4,d3
    0x86,0x84, //           	or.l d4,d3
    0x28,0x02, //           	move.l d2,d4
    0xc4,0x85, //           	and.l d5,d2
    0xca,0x83, //           	and.l d3,d5
    0xb5,0x84, //           	eor.l d2,d4
    0xbb,0x83, //           	eor.l d5,d3
    0xe1,0x8a, //           	lsl.l #8,d2
    0xe0,0x8b, //           	lsr.l #8,d3
    0x84,0x85, //           	or.l d5,d2
    0x86,0x84, //           	or.l d4,d3
    0x28,0x02, //           	move.l d2,d4
    0x2a,0x03, //           	move.l d3,d5
    0xc4,0x86, //           	and.l d6,d2
    0xca,0x86, //           	and.l d6,d5
    0xb5,0x84, //           	eor.l d2,d4
    0xbb,0x83, //           	eor.l d5,d3
    0xda,0x85, //           	add.l d5,d5
    0xe2,0x8c, //           	lsr.l #1,d4
    0x84,0x85, //           	or.l d5,d2
    0x86,0x84, //           	or.l d4,d3
    0x48,0x42, //           	swap d2
    0x48,0x43, //           	swap d3
    0xb1,0x42, //           	eor.w d0,d2
    0xb3,0x43, //           	eor.w d1,d3
    0xb5,0x40, //           	eor.w d2,d0
    0xb7,0x41, //           	eor.w d3,d1
    0xb1,0x42, //           	eor.w d0,d2
    0xb3,0x43, //           	eor.w d1,d3
    0x48,0x42, //           	swap d2
    0x48,0x43, //           	swap d3
    0x28,0x00, //           	move.l d0,d4
    0x2a,0x02, //           	move.l d2,d5
    0xc0,0x87, //           	and.l d7,d0
    0xca,0x87, //           	and.l d7,d5
    0xb1,0x84, //           	eor.l d0,d4
    0xbb,0x82, //           	eor.l d5,d2
    0xe5,0x88, //           	lsl.l #2,d0
    0xe4,0x8a, //           	lsr.l #2,d2
    0x80,0x85, //           	or.l d5,d0
    0x22,0xc0, //           	move.l d0,(a1)+
    0x84,0x84, //           	or.l d4,d2
    0x26,0xc2, //           	move.l d2,(a3)+
    0x28,0x01, //           	move.l d1,d4
    0x2a,0x03, //           	move.l d3,d5
    0xc2,0x87, //           	and.l d7,d1
    0xca,0x87, //           	and.l d7,d5
    0xb3,0x84, //           	eor.l d1,d4
    0xbb,0x83, //           	eor.l d5,d3
    0xe5,0x89, //           	lsl.l #2,d1
    0xe4,0x8b, //           	lsr.l #2,d3
    0x82,0x85, //           	or.l d5,d1
    0x24,0xc1, //           	move.l d1,(a2)+
    0x86,0x84, //           	or.l d4,d3
    0x28,0xc3, //           	move.l d3,(a4)+
    0xb1,0xcd, //       cmpa.l a5,a0
    0x6d,0x00,0xff,0x30, // blt.w loop
    0x4e,0x75,
};

void *m68kcodeptr = m68kcode;

uint32_t data[128];
uint32_t stack[512];

void print_context(struct M68KState *m68k)
{
    printf("\nM68K Context:\n");

    for (int i=0; i < 8; i++) {
        if (i==4)
            printf("\n");
        printf("    D%d = 0x%08x", i, BE32(m68k->D[i].u32));
    }
    printf("\n");

    for (int i=0; i < 8; i++) {
        if (i==4)
            printf("\n");
        printf("    A%d = 0x%08x", i, BE32(m68k->A[i].u32));
    }
    printf("\n");

    printf("    PC = 0x%08x    SR = ", BE32((int)m68k->PC));
    uint16_t sr = BE16(m68k->SR);
    if (sr & SR_X)
        printf("X");
    else
        printf(".");

    if (sr & SR_N)
        printf("N");
    else
        printf(".");

    if (sr & SR_Z)
        printf("Z");
    else
        printf(".");

    if (sr & SR_V)
        printf("V");
    else
        printf(".");

    if (sr & SR_C)
        printf("C");
    else
        printf(".");

    printf("\n    USP= 0x%08x    MSP= 0x%08x    ISP= 0x%08x\n", BE32(m68k->USP.u32), BE32(m68k->MSP.u32), BE32(m68k->ISP.u32));
}

#define DATA_SIZE 25600*4800

uint8_t *chunky; //[DATA_SIZE];
uint8_t *plane0; //[DATA_SIZE ];
uint8_t *plane1; //[DATA_SIZE ];
uint8_t *plane2; //[DATA_SIZE ];
uint8_t *plane3; //[DATA_SIZE ];

uint8_t *bitmap[4];// = {
    //plane0, plane1, plane2, plane3
//};


void start_emu(void *addr)
{
    register struct M68KState * m68k asm("fp");
    M68K_InitializeCache();
    uint64_t t1=0, t2=0;
    uint8_t *ptr = (uint8_t *)0x800000;
    chunky = ptr;
    ptr += DATA_SIZE;
    plane0 = ptr;
    ptr += DATA_SIZE;
    plane1 = ptr;
    ptr += DATA_SIZE;
    plane2 = ptr;
    ptr += DATA_SIZE;
    plane3 = ptr;

    bitmap[0] = plane0;
    bitmap[1] = plane1;
    bitmap[2] = plane2;
    bitmap[3] = plane3;

    void (*arm_code)(); //(struct M68KState *ctx);

    struct M68KTranslationUnit * unit = (void*)0;
    struct M68KState __m68k;

    bzero(&__m68k, sizeof(__m68k));
    asm volatile ("mov %0, %1":"=r"(m68k):"r"(&__m68k));
    //m68k = &__m68k;


for (int i=0; i < 3; i++) {

    bitmap[0] = (uint8_t *)BE32((uintptr_t)bitmap[0]);
    bitmap[1] = (uint8_t *)BE32((uintptr_t)bitmap[1]);
    bitmap[2] = (uint8_t *)BE32((uintptr_t)bitmap[2]);
    bitmap[3] = (uint8_t *)BE32((uintptr_t)bitmap[3]);

    memset(&stack, 0xaa, sizeof(stack));
    m68k->D[0].u32 = BE32(10);
    m68k->A[0].u32 = BE32((uint32_t)chunky);
    m68k->A[1].u32 = BE32((uint32_t)chunky + DATA_SIZE);
    m68k->A[2].u32 = BE32((uint32_t)bitmap);
    m68k->A[7].u32 = BE32((uint32_t)&stack[511]);
    m68k->PC = (uint16_t *)BE32((uint32_t)addr); //m68k.PC = (uint16_t *)BE32((uint32_t)m68kcodeptr);

    data[0] = BE32(3);
    data[1] = BE32(100);
    stack[511] = 0;

    print_context(m68k);

    printf("[JIT] Let it go...\n");
    uint64_t ctx_count = 0;
    uint32_t last_PC = 0xffffffff;
    t1 = *(volatile uint32_t*)0xf2003004 | (uint64_t)(*(volatile uint32_t *)0xf2003008) << 32;

    do {
        if (last_PC != (uint32_t)m68k->PC)
        {
            unit = M68K_GetTranslationUnit((uint16_t *)(BE32((uint32_t)m68k->PC)));
            last_PC = (uint32_t)m68k->PC;
        }

        *(void**)(&arm_code) = unit->mt_ARMEntryPoint;
        arm_code(m68k);
    } while(m68k->PC != (void*)0);

    t2 = *(volatile uint32_t*)0xf2003004 | (uint64_t)(*(volatile uint32_t *)0xf2003008) << 32;

    printf("[JIT] Time spent in m68k mode: %lld us\n", t2-t1);
    printf("[JIT] Number of ARM-M68k switches: %lld\n", ctx_count);

    printf("Back from translated code\n");

    print_context(m68k);
}
    M68K_DumpStats();
}