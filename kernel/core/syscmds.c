/* General system commands: info, memory, CPUs, time, power, self-tests. */
#include "arch.h"
#include "block.h"
#include "console.h"
#include "fdt.h"
#include "irq.h"
#include "kernel.h"
#include "mm.h"
#include "shell.h"
#include "smp.h"
#include "string.h"

/* Civil date from days since 1970-01-01 (Howard Hinnant's algorithm). */
void epoch_to_date(u64 epoch, int *y, int *mo, int *d, int *h, int *mi, int *s)
{
    i64 days = (i64)(epoch / 86400);
    i64 rem = (i64)(epoch % 86400);
    *h = (int)(rem / 3600);
    *mi = (int)(rem / 60 % 60);
    *s = (int)(rem % 60);
    days += 719468;
    i64 era = days / 146097;
    i64 doe = days - era * 146097;
    i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    i64 yy = yoe + era * 400;
    i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    i64 mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *mo = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*mo <= 2));
}

void format_uptime(char *buf, size_t size)
{
    u64 s = time_ms() / 1000;
    snprintf(buf, size, "%llud %02lluh %02llum %02llus", (unsigned long long)(s / 86400),
             (unsigned long long)(s / 3600 % 24), (unsigned long long)(s / 60 % 60),
             (unsigned long long)(s % 60));
}

const char *cpu_model_name(void)
{
    u64 midr = read_sysreg(midr_el1);
    u32 impl = (u32)(midr >> 24) & 0xff, part = (u32)(midr >> 4) & 0xfff;
    if (impl == 0x41) {
        switch (part) {
        case 0xd03: return "ARM Cortex-A53";
        case 0xd04: return "ARM Cortex-A35";
        case 0xd05: return "ARM Cortex-A55";
        case 0xd07: return "ARM Cortex-A57";
        case 0xd08: return "ARM Cortex-A72";
        case 0xd09: return "ARM Cortex-A73";
        case 0xd0a: return "ARM Cortex-A75";
        case 0xd0b: return "ARM Cortex-A76";
        case 0xd0c: return "ARM Neoverse-N1";
        case 0xd0d: return "ARM Cortex-A77";
        case 0xd40: return "ARM Neoverse-V1";
        case 0xd41: return "ARM Cortex-A78";
        case 0xd44: return "ARM Cortex-X1";
        case 0xd46: return "ARM Cortex-A510";
        case 0xd47: return "ARM Cortex-A710";
        case 0xd48: return "ARM Cortex-X2";
        case 0xd49: return "ARM Neoverse-N2";
        case 0xd4f: return "ARM Neoverse-V2";
        case 0xd80: return "ARM Cortex-A520";
        case 0xd81: return "ARM Cortex-A720";
        case 0xd82: return "ARM Cortex-X4";
        default: return "ARM Cortex (unknown part)";
        }
    }
    if (impl == 0x61)
        return "Apple silicon";
    if (impl == 0x51)
        return "Qualcomm";
    if (impl == 0x00)
        return "QEMU/emulated";
    return "unknown";
}

static int cmd_info(int argc, char **argv)
{
    char up[48];
    format_uptime(up, sizeof(up));
    const char *model = fdt_prop_str(0, "model");
    u64 isar0 = read_sysreg(id_aa64isar0_el1);
    printk("%s %s for aarch64\n", OS_NAME, OS_VERSION);
    printk("machine : %s\n", model ? model : "unknown");
    printk("cpu     : %s x %d (MIDR %#llx)%s%s\n", cpu_model_name(), smp_num_cpus(),
           (unsigned long long)read_sysreg(midr_el1),
           ((isar0 >> 44) & 0xf) ? " +dotprod" : "", ((isar0 >> 20) & 0xf) >= 2 ? " +lse" : "");
    printk("memory  : %llu MiB total, %llu MiB free\n",
           (unsigned long long)(pmm_total_bytes() / MiB),
           (unsigned long long)(pmm_free_bytes() / MiB));
    printk("irq     : %s\n", gic_name());
    printk("uptime  : %s\n", up);
    printk("booted  : at EL%d, image at %#llx\n", boot_info.boot_el,
           (unsigned long long)boot_info.image_base);
    return 0;
}
SHELL_COMMAND(info, "info", "", "show system information", cmd_info);

static int cmd_mem(int argc, char **argv)
{
    struct mem_range r[16];
    int n = pmm_ram_ranges(r, 16);
    for (int i = 0; i < n; i++)
        printk("ram %d: %#012llx - %#012llx (%llu MiB)\n", i, (unsigned long long)r[i].base,
               (unsigned long long)(r[i].base + r[i].size), (unsigned long long)(r[i].size / MiB));
    printk("total %llu MiB, free %llu MiB, largest free block %llu MiB, heap in use %llu KiB\n",
           (unsigned long long)(pmm_total_bytes() / MiB),
           (unsigned long long)(pmm_free_bytes() / MiB),
           (unsigned long long)(pmm_largest_free() / MiB),
           (unsigned long long)(kmalloc_stats_used() / KiB));
    return 0;
}
SHELL_COMMAND(mem, "mem", "", "show memory usage", cmd_mem);

static int cmd_cpus(int argc, char **argv)
{
    for (int i = 0; i < smp_num_cpus(); i++)
        printk("cpu%d: mpidr %#llx%s\n", i, (unsigned long long)smp_cpu_mpidr(i),
               i == 0 ? " (boot, interrupts)" : " (worker)");
    printk("parallel workers in use: %d\n", smp_get_workers());
    return 0;
}
SHELL_COMMAND(cpus, "cpus", "", "list processors", cmd_cpus);

static int cmd_uptime(int argc, char **argv)
{
    char up[48];
    format_uptime(up, sizeof(up));
    printk("up %s\n", up);
    return 0;
}
SHELL_COMMAND(uptime, "uptime", "", "time since boot", cmd_uptime);

static int cmd_dmesg(int argc, char **argv)
{
    size_t want = 64 * 1024;
    if (argc > 2 || (argc == 2 && !strcmp(argv[1], "-h"))) {
        printk("usage: dmesg [BYTES]\n");
        return 1;
    }
    if (argc == 2) {
        char *end;
        want = strtoul(argv[1], &end, 10);
        if (*end || !want) {
            printk("dmesg: not a byte count: %s\n", argv[1]);
            return 1;
        }
    }
    char *buf = kmalloc(MIN(want, (size_t)64 * 1024));
    if (!buf) {
        printk("dmesg: out of memory\n");
        return 1;
    }
    size_t n = console_log_read(buf, MIN(want, (size_t)64 * 1024));
    console_write(buf, n);
    kfree(buf);
    return 0;
}
SHELL_COMMAND(dmesg, "dmesg", "[BYTES]", "show recent console output, from boot on", cmd_dmesg);

static int cmd_date(int argc, char **argv)
{
    u64 e = rtc_epoch();
    if (!e) {
        printk("date: no real-time clock\n");
        return 1;
    }
    int y, mo, d, h, mi, s;
    epoch_to_date(e, &y, &mo, &d, &h, &mi, &s);
    printk("%04d-%02d-%02d %02d:%02d:%02d UTC\n", y, mo, d, h, mi, s);
    return 0;
}
SHELL_COMMAND(date, "date", "", "show the current date and time (UTC)", cmd_date);

static int cmd_sleep(int argc, char **argv)
{
    if (argc != 2) {
        printk("usage: sleep SECONDS\n");
        return 1;
    }
    double sec = strtod(argv[1], NULL);
    if (sec < 0 || sec > 86400) {
        printk("sleep: bad duration\n");
        return 1;
    }
    u64 until = time_ms() + (u64)(sec * 1000);
    while (time_ms() < until) {
        if (console_interrupted())
            return 130;
        console_run_idle(); /* keep the network serviced */
        wfi();
    }
    return 0;
}
SHELL_COMMAND(sleep, "sleep", "sleep SECONDS", "wait (Ctrl-C stops)", cmd_sleep);

static int cmd_time(int argc, char **argv)
{
    if (argc < 2) {
        printk("usage: time COMMAND...\n");
        return 1;
    }
    char line[1024];
    size_t n = 0;
    line[0] = 0;
    for (int i = 1; i < argc && n < sizeof(line); i++)
        n += (size_t)snprintf(line + n, sizeof(line) - n, i > 1 ? " %s" : "%s", argv[i]);
    u64 t0 = time_ns();
    int rc = shell_exec(line);
    u64 us = (time_ns() - t0) / 1000;
    printk("real %llu.%06llu s\n", (unsigned long long)(us / 1000000),
           (unsigned long long)(us % 1000000));
    return rc;
}
SHELL_COMMAND(time, "time", "time COMMAND...", "run a command and show how long it took",
              cmd_time);

static int cmd_poweroff(int argc, char **argv)
{
    printk("Powering off.\n");
    blk_shutdown_all();
    psci_system_off();
}
SHELL_COMMAND(poweroff, "poweroff", "", "turn the machine off", cmd_poweroff);

static int cmd_reboot(int argc, char **argv)
{
    printk("Rebooting.\n");
    blk_shutdown_all();
    psci_system_reset();
}
SHELL_COMMAND(reboot, "reboot", "", "restart the machine", cmd_reboot);

struct smptest {
    u64 *sums;
    int per;
};

static void smptest_fn(void *ctx, int start, int end)
{
    struct smptest *t = ctx;
    for (int i = start; i < end; i++) {
        u64 s = (u64)i + 1;
        for (int k = 0; k < t->per; k++) { /* dependent hash chain: not reducible */
            s ^= s >> 29;
            s *= 0xbf58476d1ce4e5b9ULL;
            s ^= s >> 32;
            s += (u64)k;
        }
        t->sums[i] = s;
    }
}

static int cmd_smptest(int argc, char **argv)
{
    int n = 4096;
    struct smptest t = { kcalloc((size_t)n, sizeof(u64)), 4000 };
    if (!t.sums)
        return 1;
    u64 t0 = time_ns();
    smp_set_workers(1);
    parallel_for(n, 16, smptest_fn, &t);
    u64 t1 = time_ns();
    u64 check1 = 0;
    for (int i = 0; i < n; i++)
        check1 += t.sums[i];
    memset(t.sums, 0, (size_t)n * sizeof(u64));
    smp_set_workers(MAX_CPUS);
    for (int rep = 0; rep < 200; rep++) /* exercise job hand-off repeatedly */
        parallel_for(64, 1, smptest_fn, &t);
    u64 t2 = time_ns();
    parallel_for(n, 16, smptest_fn, &t);
    u64 t3 = time_ns();
    u64 check2 = 0;
    for (int i = 0; i < n; i++)
        check2 += t.sums[i];
    kfree(t.sums);
    printk("1 cpu: %llu us, %d cpus: %llu us (speedup %.2fx), results %s\n",
           (unsigned long long)((t1 - t0) / 1000), smp_get_workers(),
           (unsigned long long)((t3 - t2) / 1000), (double)(t1 - t0) / (double)(t3 - t2),
           check1 == check2 ? "match" : "MISMATCH");
    return check1 == check2 ? 0 : 1;
}
SHELL_COMMAND(smptest, "smptest", "", "check that all CPUs compute correctly", cmd_smptest);
