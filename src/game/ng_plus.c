#include "game/ng_plus.h"
#include "game/game_state.h"
#include "game/inventory.h"
#include <stddef.h>
#include <string.h>

#define NG_MAGIC 0x3150474Eu     /* "NGP1" */
#define NG_FLAG_FIRST 1016       /* event flags 1016-1023 hold the cycle count */
#define NG_COPIES 2
#define CRAZY_FLAG_FIRST 1008    /* event flag 1008: Crazy Mode */

PACKED_STRUCT
typedef struct {
    uint32_t magic;
    uint8_t  cycle;              /* the cycle a New Game+ from this clear would be */
    uint8_t  crazy;              /* the cleared game was a Crazy Mode game */
    uint8_t  reserved[2];
    CharStruct chars[4];
    uint16_t checksum;           /* sum of the bytes above */
} NgBlock;
END_PACKED_STRUCT

static NgBlock pending;
static bool pending_valid;
static bool pending_crazy;

int ng_plus_cycle(void) {
    return event_flags[NG_FLAG_FIRST / 8];
}

bool ng_plus_crazy(void) {
    return event_flags[CRAZY_FLAG_FIRST / 8] & 1;
}

static void set_crazy(bool crazy) {
    event_flags[CRAZY_FLAG_FIRST / 8] = crazy ? 1 : 0;
}

static void set_cycle(int cycle) {
    event_flags[NG_FLAG_FIRST / 8] = (uint8_t)(cycle > 255 ? 255 : cycle);
}

static uint16_t block_sum(const NgBlock *b) {
    const uint8_t *p = (const uint8_t *)b;
    uint16_t sum = 0;
    for (size_t i = 0; i < offsetof(NgBlock, checksum); i++) sum += p[i];
    return sum;
}

static bool read_block(int slot, NgBlock *out) {
    for (int copy = 0; copy < NG_COPIES; copy++) {
        if (save_aux_read(slot, (size_t)copy * sizeof(NgBlock), out, sizeof(*out)) != sizeof(*out)) continue;
        if (out->magic == NG_MAGIC && out->checksum == block_sum(out)) return true;
    }
    return false;
}

static bool write_block(int slot, NgBlock *b) {
    b->magic = NG_MAGIC;
    b->checksum = block_sum(b);
    bool ok = true;
    for (int copy = 0; copy < NG_COPIES; copy++)
        ok &= save_aux_write(slot, (size_t)copy * sizeof(NgBlock), b, sizeof(*b));
    return ok;
}

void ng_plus_record_clear(void) {
    int slot = (int)current_save_slot - 1;
    if (slot < 0 || slot >= SAVE_COUNT) return;
    NgBlock b;
    memset(&b, 0, sizeof(b));
    b.cycle = (uint8_t)(ng_plus_cycle() + 1);
    b.crazy = ng_plus_crazy();
    memcpy(b.chars, party_characters, sizeof(b.chars));
    write_block(slot, &b);
}

bool ng_plus_slot_cleared(int slot) {
    NgBlock b;
    return read_block(slot, &b);
}

int ng_plus_next_cycle(int slot) {
    NgBlock b;
    return read_block(slot, &b) ? b.cycle : 0;
}

bool ng_plus_crazy_unlocked(int slot) {
    NgBlock b;
    return read_block(slot, &b) && b.cycle >= 2;
}

void ng_plus_begin_crazy(void) {
    pending_valid = false;
    pending_crazy = true;
}

bool ng_plus_begin(int slot) {
    pending_crazy = false;
    pending_valid = read_block(slot, &pending);
    return pending_valid;
}

bool ng_plus_pending(void) { return pending_valid; }
void ng_plus_cancel(void) { pending_valid = false; pending_crazy = false; }

static bool is_key_item(uint8_t item) {
    return item != 0 && (get_item_type(item) & ITEM_TYPE_MASK) == (ITEM_TYPE_KEY_ITEM & ITEM_TYPE_MASK);
}

void ng_plus_apply_new_game(void) {
    if (!pending_valid) {
        set_cycle(0);            /* an ordinary or Crazy Mode new game */
        set_crazy(pending_crazy);
        pending_crazy = false;
        return;
    }
    pending_valid = false;
    for (int c = 0; c < 4; c++) {
        CharStruct *dst = &party_characters[c];
        const CharStruct *src = &pending.chars[c];
        dst->level = src->level;
        dst->exp = src->exp;
        dst->max_hp = src->max_hp;
        dst->max_pp = src->max_pp;
        dst->base_offense = src->base_offense;
        dst->base_defense = src->base_defense;
        dst->base_speed = src->base_speed;
        dst->base_guts = src->base_guts;
        dst->base_luck = src->base_luck;
        dst->base_vitality = src->base_vitality;
        dst->base_iq = src->base_iq;
        /* Every item but the key items, in the slots they were in so the equipment slots still
         * point at the right things; an equipped key item is simply unequipped. */
        memcpy(dst->items, src->items, sizeof(dst->items));
        memcpy(dst->equipment, src->equipment, sizeof(dst->equipment));
        for (int i = 0; i < 14; i++) {
            if (!is_key_item(dst->items[i])) continue;
            dst->items[i] = 0;
            for (int e = 0; e < 4; e++)
                if (dst->equipment[e] == i + 1) dst->equipment[e] = 0;
        }
        uint16_t id = (uint16_t)(c + 1);
        recalc_character_postmath_offense(id);
        recalc_character_postmath_defense(id);
        recalc_character_postmath_speed(id);
        recalc_character_postmath_guts(id);
        recalc_character_postmath_luck(id);
        recalc_character_postmath_vitality(id);
        recalc_character_postmath_iq(id);
        recalc_character_miss_rate(id);
        calc_resistances(id);
    }
    set_cycle(pending.cycle);
    set_crazy(pending.crazy);    /* a New Game+ of a Crazy Mode clear is crazy too */
}

void ng_plus_clear_slot(int slot) {
    static const uint8_t zero[sizeof(NgBlock)];
    for (int copy = 0; copy < NG_COPIES; copy++)
        save_aux_write(slot, (size_t)copy * sizeof(NgBlock), zero, sizeof(zero));
}

uint32_t ng_plus_scale_percent(void) {
    return 100u + 50u * (uint32_t)ng_plus_cycle();
}
