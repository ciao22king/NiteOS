#ifndef NEO_MB2_PARSE_H
#define NEO_MB2_PARSE_H

#include <multiboot2.h>

/* Iterate over the tags in a Multiboot2 info structure. Usage:

     struct mb2_tag *tag;
     for_each_mb2_tag(info, tag) { ... }

   `tag` must be declared as struct mb2_tag *. */
#define for_each_mb2_tag(info, tag)                                        \
    for (struct mb2_tag *tag = (struct mb2_tag *)((u8 *)(info) + 8);       \
         tag->type != MB2_TAG_END;                                         \
         tag = (struct mb2_tag *)ALIGN_UP((u64)tag + tag->size, 8))

/* First tag of a given type, or NULL. */
struct mb2_tag *mb2_find(struct mb2_info *info, u32 type);

/* Global access to the boot info structure, set once in kmain. */
void mb2_set_info(struct mb2_info *info);
struct mb2_info *neo_mb_info(void);

/* Upper bound (exclusive) of the info structure, used to reserve its pages. */
u64 mb2_end_addr(struct mb2_info *info);

#endif
