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

/** @brief Switch the input lock on or off.
 *
 * Set once from the options at host init; the knob is restart-only. Read on
 * the pump thread and, through openpete_input_lock_active, by any consumer.
 * @param on nonzero to lock. */
void openpete_input_lock_set(int on);
int  openpete_input_lock_active(void);

/** @brief Return whether the lock discards an event; pump thread, first call on every dequeued event.
 *
 * An event is discarded when the lock is on and the event is input from a
 * physical device: a keyboard or mouse event whose `which` is not
 * OPENPETE_INJECT_DEVICE_ID, a joystick or gamepad input event from a
 * non-virtual joystick, and every text, touch, pen and drop event. Device
 * arrival and removal, quit and window events pass. Each discard increments
 * the openpete_input_lock_dropped count.
 * @param ev the dequeued event; NULL is never discarded.
 * @return 1 when the caller must discard the event without forwarding it,
 *         0 otherwise. */
int  openpete_input_lock_drops_event(const union SDL_Event* ev);

/** @brief Return the number of events openpete_input_lock_drops_event discarded since boot.
 *
 * @return the discard count. */
unsigned long long openpete_input_lock_dropped(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_INPUT_LOCK_H */
