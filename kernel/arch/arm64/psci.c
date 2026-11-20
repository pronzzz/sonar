/* PSCI (Power State Coordination Interface) calls via HVC or SMC, as
 * selected by the device tree's /psci "method" property. */
#include "arch.h"
#include "fdt.h"
#include "kernel.h"
#include "string.h"

#define PSCI_VERSION 0x84000000U
#define PSCI_CPU_ON_64 0xc4000003U
#define PSCI_SYSTEM_OFF 0x84000008U
#define PSCI_SYSTEM_RESET 0x84000009U

static int method; /* 0 = none, 1 = hvc, 2 = smc */

static u64 psci_call(u64 fn, u64 a1, u64 a2, u64 a3)
{
    register u64 x0 __asm__("x0") = fn;
    register u64 x1 __asm__("x1") = a1;
    register u64 x2 __asm__("x2") = a2;
    register u64 x3 __asm__("x3") = a3;
    if (method == 1)
        __asm__ volatile("hvc #0"
                         : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                         :
                         : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
                           "x14", "x15", "x16", "x17", "memory");
    else if (method == 2)
        __asm__ volatile("smc #0"
                         : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                         :
                         : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
                           "x14", "x15", "x16", "x17", "memory");
    else
        return (u64)-1;
    return x0;
}

int psci_init(void)
{
    int node = fdt_path("/psci");
    if (node < 0)
        node = fdt_find_compatible(-1, "arm,psci-1.0");
    if (node < 0)
        node = fdt_find_compatible(-1, "arm,psci-0.2");
    if (node < 0)
        return -1;
    const char *m = fdt_prop_str(node, "method");
    if (m && strcmp(m, "hvc") == 0)
        method = 1;
    else if (m && strcmp(m, "smc") == 0)
        method = 2;
    else
        return -1;
    u64 v = psci_call(PSCI_VERSION, 0, 0, 0);
    return (int)v;
}

int psci_cpu_on(u64 mpidr, paddr_t entry, u64 context)
{
    return (int)(i64)psci_call(PSCI_CPU_ON_64, mpidr, entry, context);
}

void psci_system_off(void)
{
    psci_call(PSCI_SYSTEM_OFF, 0, 0, 0);
    for (;;)
        wfi();
}

void psci_system_reset(void)
{
    psci_call(PSCI_SYSTEM_RESET, 0, 0, 0);
    for (;;)
        wfi();
}
