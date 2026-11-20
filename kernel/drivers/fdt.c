/*
 * Flattened Device Tree reader (devicetree specification v0.4).
 * Read-only, allocation-free, and safe to use with the MMU off.
 */
#include "fdt.h"
#include "kernel.h"
#include "string.h"

#define FDT_MAGIC 0xd00dfeed
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE 2
#define FDT_PROP 3
#define FDT_NOP 4
#define FDT_END 9

static const u8 *blob;
static const u8 *structs;
static const char *strings;
static u32 struct_size;
static u32 strings_size;

u32 fdt_total_size(const void *b) { return be32((const u8 *)b + 4); }
const void *fdt_blob(void) { return blob; }

bool fdt_init(const void *b)
{
    const u8 *p = b;
    if (!p || ((uintptr_t)p & 3) || be32(p) != FDT_MAGIC)
        return false;
    u32 version = be32(p + 20);
    u32 last_comp = be32(p + 24);
    if (version < 16 || last_comp > 17)
        return false;
    blob = p;
    structs = p + be32(p + 8);
    strings = (const char *)p + be32(p + 12);
    strings_size = be32(p + 32);
    struct_size = be32(p + 36);
    return true;
}

static u32 tok_at(int off) { return be32(structs + off); }

/* Skip one token at off; returns offset of the next token. */
static int skip_token(int off)
{
    u32 tok = tok_at(off);
    off += 4;
    switch (tok) {
    case FDT_BEGIN_NODE: {
        const char *name = (const char *)structs + off;
        off += (int)ALIGN_UP(strlen(name) + 1, 4);
        return off;
    }
    case FDT_PROP: {
        u32 len = be32(structs + off);
        off += 8 + (int)ALIGN_UP(len, 4);
        return off;
    }
    case FDT_END_NODE:
    case FDT_NOP:
        return off;
    default:
        return -1;
    }
}

int fdt_root(void) { return blob ? 0 : -1; }

int fdt_next_node(int node, int *depth)
{
    if (node < 0)
        return -1;
    int off = node;
    /* Step past the current BEGIN_NODE first. */
    if (tok_at(off) == FDT_BEGIN_NODE) {
        off = skip_token(off);
    }
    while (off >= 0 && (u32)off < struct_size) {
        u32 tok = tok_at(off);
        if (tok == FDT_BEGIN_NODE) {
            if (depth)
                (*depth)++;
            return off;
        }
        if (tok == FDT_END_NODE) {
            if (depth)
                (*depth)--;
        } else if (tok == FDT_END) {
            return -1;
        }
        off = skip_token(off);
    }
    return -1;
}

int fdt_first_child(int node)
{
    int depth = 0;
    int n = fdt_next_node(node, &depth);
    return (n >= 0 && depth == 1) ? n : -1;
}

int fdt_next_sibling(int node)
{
    int depth = 0;
    int n = node;
    for (;;) {
        n = fdt_next_node(n, &depth);
        if (n < 0 || depth < 0)
            return -1;
        if (depth == 0)
            return n;
    }
}

int fdt_parent(int node)
{
    int stack[64];
    int depth = 0;
    int n = 0;
    stack[0] = 0;
    if (node == 0)
        return -1;
    int d = 0;
    for (;;) {
        n = fdt_next_node(n, &d);
        if (n < 0 || d <= 0 || d >= 64)
            return -1;
        stack[d] = n;
        depth = d;
        if (n == node)
            return stack[depth - 1];
    }
}

const char *fdt_name(int node)
{
    if (node < 0 || tok_at(node) != FDT_BEGIN_NODE)
        return "";
    return (const char *)structs + node + 4;
}

const void *fdt_prop(int node, const char *name, int *len)
{
    if (node < 0 || tok_at(node) != FDT_BEGIN_NODE)
        return NULL;
    int off = skip_token(node);
    while (off >= 0) {
        u32 tok = tok_at(off);
        if (tok == FDT_PROP) {
            u32 plen = be32(structs + off + 4);
            u32 nameoff = be32(structs + off + 8);
            if (nameoff < strings_size && strcmp(strings + nameoff, name) == 0) {
                if (len)
                    *len = (int)plen;
                return structs + off + 12;
            }
        } else if (tok != FDT_NOP) {
            break; /* properties precede subnodes */
        }
        off = skip_token(off);
    }
    return NULL;
}

bool fdt_prop_u32(int node, const char *name, u32 *out)
{
    int len;
    const void *p = fdt_prop(node, name, &len);
    if (!p || len < 4)
        return false;
    *out = be32(p);
    return true;
}

const char *fdt_prop_str(int node, const char *name)
{
    int len;
    const char *p = fdt_prop(node, name, &len);
    if (!p || len <= 0 || p[len - 1] != 0)
        return NULL;
    return p;
}

bool fdt_is_compatible(int node, const char *compat)
{
    int len;
    const char *p = fdt_prop(node, "compatible", &len);
    if (!p)
        return false;
    const char *end = p + len;
    while (p < end) {
        if (strcmp(p, compat) == 0)
            return true;
        p += strlen(p) + 1;
    }
    return false;
}

bool fdt_is_enabled(int node)
{
    const char *s = fdt_prop_str(node, "status");
    return !s || strcmp(s, "okay") == 0 || strcmp(s, "ok") == 0;
}

int fdt_find_compatible(int after, const char *compat)
{
    int depth = 0;
    int n = after < 0 ? 0 : after;
    if (after < 0 && fdt_is_compatible(0, compat))
        return 0;
    for (;;) {
        n = fdt_next_node(n, &depth);
        if (n < 0)
            return -1;
        if (fdt_is_compatible(n, compat))
            return n;
    }
}

int fdt_find_phandle(u32 phandle)
{
    int depth = 0;
    int n = 0;
    while (n >= 0) {
        u32 v;
        if ((fdt_prop_u32(n, "phandle", &v) || fdt_prop_u32(n, "linux,phandle", &v)) &&
            v == phandle)
            return n;
        n = fdt_next_node(n, &depth);
    }
    return -1;
}

int fdt_path(const char *path)
{
    if (!blob || path[0] != '/')
        return -1;
    int node = 0;
    const char *p = path + 1;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t seg = slash ? (size_t)(slash - p) : strlen(p);
        if (seg == 0) {
            p++;
            continue;
        }
        int child = fdt_first_child(node);
        int found = -1;
        while (child >= 0) {
            const char *name = fdt_name(child);
            /* Match "name" exactly, or "name@unit" when no unit given. */
            if (strncmp(name, p, seg) == 0 &&
                (name[seg] == 0 || (name[seg] == '@' && !memchr(p, '@', seg)))) {
                found = child;
                break;
            }
            child = fdt_next_sibling(child);
        }
        if (found < 0)
            return -1;
        node = found;
        p += seg;
    }
    return node;
}

u32 fdt_address_cells(int node)
{
    int parent = fdt_parent(node);
    u32 v = 2;
    if (parent >= 0)
        fdt_prop_u32(parent, "#address-cells", &v);
    else
        fdt_prop_u32(0, "#address-cells", &v);
    return v;
}

u32 fdt_size_cells(int node)
{
    int parent = fdt_parent(node);
    u32 v = 1;
    if (parent >= 0)
        fdt_prop_u32(parent, "#size-cells", &v);
    else
        fdt_prop_u32(0, "#size-cells", &v);
    return v;
}

/* Translate a bus address through the "ranges" of node's ancestors. */
static u64 translate(int node, u64 addr)
{
    int bus = fdt_parent(node);
    while (bus > 0) {
        int len;
        const u8 *r = fdt_prop(bus, "ranges", &len);
        if (!r)
            break; /* no ranges: no translation possible, assume identity */
        if (len == 0) {
            bus = fdt_parent(bus);
            continue; /* empty ranges: identity */
        }
        u32 child_ac = 2, parent_ac = 2, sc = 1;
        fdt_prop_u32(bus, "#address-cells", &child_ac);
        fdt_prop_u32(bus, "#size-cells", &sc);
        int grand = fdt_parent(bus);
        if (grand >= 0)
            fdt_prop_u32(grand, "#address-cells", &parent_ac);
        u32 entry = (child_ac + parent_ac + sc) * 4;
        bool done = false;
        for (int off = 0; entry && off + (int)entry <= len; off += (int)entry) {
            u64 cbase = fdt_read_cells(r + off, child_ac);
            u64 pbase = fdt_read_cells(r + off + child_ac * 4, parent_ac);
            u64 size = fdt_read_cells(r + off + (child_ac + parent_ac) * 4, sc);
            if (addr >= cbase && addr - cbase < size) {
                addr = addr - cbase + pbase;
                done = true;
                break;
            }
        }
        if (!done)
            break;
        bus = grand;
    }
    return addr;
}

bool fdt_reg(int node, int index, u64 *addr, u64 *size)
{
    int len;
    const u8 *p = fdt_prop(node, "reg", &len);
    if (!p)
        return false;
    u32 ac = fdt_address_cells(node), sc = fdt_size_cells(node);
    u32 entry = (ac + sc) * 4;
    if (!entry || (u32)len < entry * (u32)(index + 1))
        return false;
    p += entry * (u32)index;
    u64 a = fdt_read_cells(p, ac);
    if (addr)
        *addr = translate(node, a);
    if (size)
        *size = sc ? fdt_read_cells(p + ac * 4, sc) : 0;
    return true;
}

int fdt_irq(int node, int index)
{
    int len;
    const u8 *p = fdt_prop(node, "interrupts", &len);
    if (!p)
        return -1;
    /* Find the interrupt controller to learn #interrupt-cells. */
    u32 cells = 3;
    int n = node;
    while (n >= 0) {
        u32 ph;
        if (fdt_prop_u32(n, "interrupt-parent", &ph)) {
            int ic = fdt_find_phandle(ph);
            if (ic >= 0)
                fdt_prop_u32(ic, "#interrupt-cells", &cells);
            break;
        }
        n = fdt_parent(n);
    }
    if (cells < 2 || (u32)len < cells * 4 * (u32)(index + 1))
        return -1;
    p += cells * 4 * (u32)index;
    if (cells == 2) /* simple controllers: raw number */
        return (int)be32(p);
    u32 type = be32(p), num = be32(p + 4);
    if (type == 0)
        return (int)num + 32; /* SPI */
    if (type == 1)
        return (int)num + 16; /* PPI */
    return -1;
}

int fdt_num_mem_rsv(void)
{
    const u8 *r = blob + be32(blob + 16);
    int n = 0;
    while (be64(r + 16 * n) || be64(r + 16 * n + 8))
        n++;
    return n;
}

bool fdt_get_mem_rsv(int i, u64 *addr, u64 *size)
{
    const u8 *r = blob + be32(blob + 16);
    *addr = be64(r + 16 * i);
    *size = be64(r + 16 * i + 8);
    return *addr || *size;
}
