#ifndef GAME_QUICK_USE_H
#define GAME_QUICK_USE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * LT / RT shortcuts (port addition).
 *
 * LT casts the registered PSI and RT uses the registered Goods item (the Cell Phone until another is
 * registered). They are registered from the menus: the PSI list and the Goods action menu get a
 * "Register" entry. A registration names a character, so the button does nothing while that
 * character is out of the party, and works again when they return. The registrations are kept in the
 * save (the slot's aux area).
 */

typedef struct {
    uint8_t lt_char;     /* 1-based PSI user; 0 = nothing registered */
    uint8_t lt_psi;      /* battle PSI table index */
    uint8_t rt_char;     /* 1-based item holder; 0 = whoever in the party carries rt_item */
    uint8_t rt_item;     /* item id; 0 = nothing registered */
} QuickRegistration;

extern QuickRegistration quick_reg;

void quick_use_reset(void);                          /* new game: LT empty, RT = Cell Phone */
void quick_use_register_psi(uint8_t char_id, uint8_t psi);
void quick_use_register_item(uint8_t char_id, uint8_t item);

/* Persist with the save slot (called by save_game / load_game). */
void quick_use_save(int slot);
void quick_use_load(int slot);

/* Is the button's registration usable right now (character in the party, able, item still carried)?
 * If so, fills `init` with the pause-menu push that runs it and returns true. */
bool quick_use_prepare(uint16_t button, void *init);

#endif /* GAME_QUICK_USE_H */
