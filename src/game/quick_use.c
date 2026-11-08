#include "game/quick_use.h"
#include "game/game_state.h"
#include "game/inventory.h"
#include "core/mode_stack.h"
#include "include/pad.h"
#include <string.h>

#define QR_MAGIC 0x31525551u      /* "QUR1" */
#define QR_AUX_OFFSET 1024        /* behind the New Game+ snapshot in the slot's aux area */
#define ITEM_CELL_PHONE 0xB5      /* Apple Kid's phone: the Receiver phone, renamed */

QuickRegistration quick_reg;

PACKED_STRUCT
typedef struct {
    uint32_t magic;
    QuickRegistration reg;
    uint8_t checksum;
} QrBlock;
END_PACKED_STRUCT

void quick_use_reset(void) {
    memset(&quick_reg, 0, sizeof(quick_reg));
    quick_reg.rt_item = ITEM_CELL_PHONE;
}

void quick_use_register_psi(uint8_t char_id, uint8_t psi) {
    quick_reg.lt_char = char_id;
    quick_reg.lt_psi = psi;
}

void quick_use_register_item(uint8_t char_id, uint8_t item) {
    quick_reg.rt_char = char_id;
    quick_reg.rt_item = item;
}

static uint8_t sum(const QrBlock *b) {
    const uint8_t *p = (const uint8_t *)&b->reg;
    uint8_t s = 0;
    for (size_t i = 0; i < sizeof(b->reg); i++) s = (uint8_t)(s + p[i]);
    return s;
}

void quick_use_save(int slot) {
    QrBlock b = { .magic = QR_MAGIC, .reg = quick_reg };
    b.checksum = sum(&b);
    save_aux_write(slot, QR_AUX_OFFSET, &b, sizeof(b));
}

void quick_use_load(int slot) {
    QrBlock b;
    if (save_aux_read(slot, QR_AUX_OFFSET, &b, sizeof(b)) == sizeof(b) &&
        b.magic == QR_MAGIC && b.checksum == sum(&b))
        quick_reg = b.reg;
    else
        quick_use_reset();       /* a save from before the shortcuts: the Cell Phone on RT */
}

static bool in_party(uint8_t char_id) {
    for (int i = 0; i < 6; i++)
        if (game_state.party_members[i] == char_id) return true;
    return false;
}

static bool able(uint8_t char_id) {
    uint8_t aff = party_characters[char_id - 1].afflictions[0];
    return aff != 1 && aff != 2;           /* unconscious, diamondized */
}

/* 1-based inventory slot of `item` for `char_id`, or 0. */
static uint8_t find_slot(uint8_t char_id, uint8_t item) {
    for (int i = 0; i < ITEM_INVENTORY_SIZE; i++)
        if (party_characters[char_id - 1].items[i] == item) return (uint8_t)(i + 1);
    return 0;
}

bool quick_use_prepare(uint16_t button, void *init_ptr) {
    ModeState *init = init_ptr;
    uint8_t kind = 0, who = 0, arg = 0;

    if (button == PAD_LT) {
        if (quick_reg.lt_char == 0 || quick_reg.lt_char > 4 || !in_party(quick_reg.lt_char) ||
            !able(quick_reg.lt_char))
            return false;
        kind = 1; who = quick_reg.lt_char; arg = quick_reg.lt_psi;
    } else if (button == PAD_RT) {
        if (quick_reg.rt_item == 0) return false;
        if (quick_reg.rt_char != 0) {
            if (quick_reg.rt_char > 4 || !in_party(quick_reg.rt_char) || !able(quick_reg.rt_char))
                return false;
            who = quick_reg.rt_char;
            arg = find_slot(who, quick_reg.rt_item);
        } else {
            for (int i = 0; i < 6 && arg == 0; i++) {        /* whoever carries it */
                uint8_t c = game_state.party_members[i];
                if (c >= 1 && c <= 4 && able(c) && (arg = find_slot(c, quick_reg.rt_item)) != 0)
                    who = c;
            }
        }
        if (who == 0 || arg == 0) return false;
        kind = 2;
    } else {
        return false;
    }

    memset(init, 0, sizeof(*init));
    init->pause_menu.phase = PM_ENTER;
    init->pause_menu.quick_kind = kind;
    init->pause_menu.quick_char = who;
    init->pause_menu.quick_arg = arg;
    return true;
}
