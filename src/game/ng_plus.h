#ifndef GAME_NG_PLUS_H
#define GAME_NG_PLUS_H

#include <stdbool.h>
#include <stdint.h>

/*
 * New Game+ (port addition).
 *
 * Beating the game (the staff credits) marks the slot being played as cleared and stores a
 * snapshot of the four PCs behind its save blocks (save_aux_*). A cleared slot can then start a
 * New Game+ in a free slot: levels, experience, stats and every item that is not a key item
 * carry over, and each cycle makes the monsters tougher. The number of cycles a playthrough has
 * is kept in the last event flags (1016-1023, unused by the game), so it travels with the save.
 */

/* Cycles of this playthrough: 0 for a normal game, 1 for the first New Game+, ... */
int ng_plus_cycle(void);

/* Staff credits reached: mark the slot being played as cleared and snapshot the party. */
void ng_plus_record_clear(void);

/* Is save slot `slot` (0-based) a cleared game? */
bool ng_plus_slot_cleared(int slot);

/* How many cycles a New Game+ started from cleared slot `slot` would have (its cycle + 1). */
int ng_plus_next_cycle(int slot);

/* Choose the cleared slot the next new game continues from; false if it is not cleared. */
bool ng_plus_begin(int slot);
bool ng_plus_pending(void);
void ng_plus_cancel(void);

/* New game setup, after the PCs are built from the initial stats: carry the snapshot over and
 * set the cycle. Does nothing when no New Game+ is pending. */
void ng_plus_apply_new_game(void);

/* A new game starting in `slot` must not inherit a stale cleared mark. */
void ng_plus_clear_slot(int slot);

/*
 * Crazy Mode: a blank-slate game with the monsters of a pirated copy. The original's anti-piracy
 * check sets PIRACY_FLAG, which makes the overworld spawner skip its spawn-chance roll, so every
 * attempt puts a monster on the map; only that is kept, none of the crashes or save erasure.
 * Unlocked by finishing a New Game+ once. The flag lives in the event flags beside the cycle, so
 * a Crazy Mode save is crazy for as long as it is played, and a New Game+ started from a cleared
 * Crazy Mode save is both at once.
 */
bool ng_plus_crazy(void);
bool ng_plus_crazy_unlocked(int slot);   /* is cleared slot `slot` a finished New Game+ (which unlocks Crazy Mode)? */
void ng_plus_begin_crazy(void);          /* the next new game is a Crazy Mode game */

/* Monster scaling for the current cycle: percent of the normal value (100 at cycle 0). */
uint32_t ng_plus_scale_percent(void);

#endif /* GAME_NG_PLUS_H */
