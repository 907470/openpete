#ifndef OPENPETE_INPUT_LOCK_H
#define OPENPETE_INPUT_LOCK_H

/* The host input lock (--input-lock / OPENPETE_INPUT_LOCK).
 *
 * With the lock on the process takes no input from a physical host device:
 *
 *   - the event pump discards every keyboard, text, mouse, touch, pen,
 *     joystick and gamepad event before any consumer (the mouse channel, the
 *     dev overlay's imgui feed, the Escape and quit-key handling, PsyCross's
 *     event handlers) sees it;
 *   - the published SDL-state snapshot carries no held key, no modifier and
 *     no physical gamepad, so every polled binding (the game pad mapping and
 *     every hotkey) reads released;
 *   - the player-1 pad decode skips the keyboard and physical gamepads;
 *   - the mouse channel behaves as it does with its own switch off.
 *
 * What still flows: --bk2 replay and the other pad override writers, the
 * synthetic pad sources, SDL virtual gamepads, and events the injection
 * script pushes, which carry OPENPETE_INJECT_DEVICE_ID. Quit, window-close
 * and every other window event pass, so closing the window still exits.
 *
 * Host-side only: nothing here reads or writes guest RAM. */

#ifdef __cplusplus
extern "C" {
#endif

union SDL_Event;

/* Device id stamped on the `which` field of every event the injection script
 * pushes. SDL numbers real devices from a small positive allocator and
 * reserves the top of the range for its own synthetic sources (touch, pen),
 * so this value belongs to no device. */
#define OPENPETE_INJECT_DEVICE_ID 0xFFFF0001u

/* Set from the options at host init. Read on the pump thread and, through
 * the accessor, by any consumer. */
void openpete_input_lock_set(int on);
int  openpete_input_lock_active(void);

/* Pump thread, first call on every dequeued event. Returns 1 when the lock is
 * on and the event came from a physical input device: the caller discards it
 * without forwarding it anywhere. Counts what it discards. */
int  openpete_input_lock_drops_event(const union SDL_Event* ev);

/* Number of events discarded since boot. */
unsigned long long openpete_input_lock_dropped(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_INPUT_LOCK_H */
