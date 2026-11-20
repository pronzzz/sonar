/* Flattened Device Tree (DTB) reader. Node handles are byte offsets into
 * the structure block; negative values mean "not found". */
#pragma once

#include "types.h"

bool fdt_init(const void *blob);
const void *fdt_blob(void);
u32 fdt_total_size(const void *blob);

int fdt_root(void);
int fdt_next_node(int node, int *depth);
int fdt_first_child(int node);
int fdt_next_sibling(int node);
int fdt_parent(int node);
int fdt_path(const char *path);
int fdt_find_compatible(int after, const char *compat);
int fdt_find_phandle(u32 phandle);
const char *fdt_name(int node);

const void *fdt_prop(int node, const char *name, int *len);
bool fdt_prop_u32(int node, const char *name, u32 *out);
const char *fdt_prop_str(int node, const char *name);
bool fdt_is_compatible(int node, const char *compat);
bool fdt_is_enabled(int node);

u32 fdt_address_cells(int node); /* #address-cells governing node's reg */
u32 fdt_size_cells(int node);
bool fdt_reg(int node, int index, u64 *addr, u64 *size);
int fdt_irq(int node, int index); /* GIC INTID or -1 */

/* Memory reservation block iteration. */
int fdt_num_mem_rsv(void);
bool fdt_get_mem_rsv(int i, u64 *addr, u64 *size);

static inline u32 be32(const void *p)
{
    const u8 *b = p;
    return (u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3];
}

static inline u64 be64(const void *p) { return (u64)be32(p) << 32 | be32((const u8 *)p + 4); }

static inline u64 fdt_read_cells(const void *p, u32 cells)
{
    u64 v = 0;
    for (u32 i = 0; i < cells; i++)
        v = (v << 32) | be32((const u8 *)p + 4 * i);
    return v;
}
