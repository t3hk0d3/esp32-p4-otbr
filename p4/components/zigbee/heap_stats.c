#include "zigbee_heap_stats.h"

static void grow(zigbee_heap_stats_t *st, size_t size)
{
    st->current += size;
    if (st->current > st->peak) {
        st->peak = st->current;
    }
}

static void shrink(zigbee_heap_stats_t *st, size_t size)
{
    st->current = size < st->current ? st->current - size : 0;
}

void zigbee_heap_stats_alloc(zigbee_heap_stats_t *st, size_t size)
{
    grow(st, size);
}

void zigbee_heap_stats_free(zigbee_heap_stats_t *st, size_t size)
{
    shrink(st, size);
}

void zigbee_heap_stats_realloc(zigbee_heap_stats_t *st, size_t old_size, size_t new_size)
{
    shrink(st, old_size);
    grow(st, new_size);
}

void zigbee_heap_stats_failed(zigbee_heap_stats_t *st)
{
    st->failures++;
}
