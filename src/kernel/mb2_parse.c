#include <mb2_parse.h>

static struct mb2_info *g_info;

void mb2_set_info(struct mb2_info *info) { g_info = info; }
struct mb2_info *neo_mb_info(void) { return g_info; }

struct mb2_tag *mb2_find(struct mb2_info *info, u32 type)
{
    for_each_mb2_tag(info, tag) {
        if (tag->type == type) return tag;
    }
    return NULL;
}

u64 mb2_end_addr(struct mb2_info *info)
{
    return (u64)info + info->total_size;
}
