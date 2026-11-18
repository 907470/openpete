// SPDX-FileCopyrightText: 2026 Tyson Cleary
// SPDX-License-Identifier: MIT
//
// probe_capture — PC-keyed probes of the reference run.
//
// PcHooks fans one Execute breakpoint per guest address out to any number
// of callbacks, so several probes can key on the same address (the BIOS
// call vectors 0xA0, 0xB0 and 0xC0 among them) without the breakpoint
// list refusing the second one.
//
// RamDump (-ram-dump-at SPEC) writes main RAM and the CPU registers when
// an instruction address is executed with optional register conditions,
// at the Nth such hit or at the return of that call.

#pragma once

#include "common/types.h"

#include <functional>
#include <string>

namespace PcHooks {

// Called before the instruction at the hooked address executes, with that
// address. Returning false removes this callback.
using Fn = std::function<bool(u32 pc)>;

// Adds a callback at a guest address (compared on the low 28 bits, as the
// breakpoint list compares). Returns false when the breakpoint cannot be
// installed (another breakpoint already holds the address).
bool Add(u32 pc, Fn fn);

} // namespace PcHooks

namespace RamDump {

// SPEC: PC[,t1=HEX][,a0=HEX][,hit=N][,ret]=PATH. PC and the register
// values are hexadecimal. The Nth (default 1st) execution of PC with every
// condition true dumps; with `ret` the dump waits for the instruction at
// that hit's $ra. PATH receives the 2 MiB of main RAM, PATH.txt the frame
// number, exact tick, hit index, PC and the 32 general registers at the
// dump (and at the call, for `ret`). Repeatable.
bool AddSpec(const std::string& spec, std::string* error_out);

// Installs every parsed spec's hook; called once the system has booted.
bool Install(std::string* error_out);

// Logs every spec that never dumped.
void Shutdown();

} // namespace RamDump

namespace CardWindow {

// -card-window PATH: the card driver's timing and the main thread's view
// of card-handler state. PATH receives one row per event, columns
// space-separated, frame = VBLANK count, tick = exact CPU tick:
//   E frame tick class spec mode func     OpenEvent (B0h 08h) call
//   D frame tick class spec ra            the kernel's DeliverEvent entry
//   C frame tick port cmd sector          card command issued on SIO0
//                                          (0x81 then cmd 'R','W','S';
//                                          sector -1 for a command without
//                                          one; cmd 00 sector -2 for a select
//                                          no card acknowledged)
//   H frame tick func                     entry of a card event handler
//                                          (a func opened on class
//                                          0xF0000011, 0xF4000001 or
//                                          0xF4000002) by a jal/jalr; the
//                                          address reached any other way
//                                          (code an overlay loaded there)
//                                          is counted, not logged
//   W frame written sio active max uncalled
//                                         at the VBLANK of every 2000th
//                                          frame: handler-written and
//                                          IRQ7-written byte counts, active
//                                          handler/delivery calls and their
//                                          maximum, non-call entries so far
//   R addr width frame pc count cat       main-path load of a byte a card
//                                          handler or a card-class delivery
//                                          stored (cat bit 0) or the service of
//                                          an IRQ7-only interrupt stored (cat
//                                          bit 1), made between a
//                                          VBLANK and the first card handler
//                                          entry after a card command issued
//                                          since that VBLANK (first such load,
//                                          and the count over the run)
// and a closing `# summary` line. Main path = no exception being serviced.
// A handler or card-class delivery call is active from its entry until
// execution reaches its return address with the entry stack pointer.
// Installs chained instruction and exception hooks; call after every other
// recorder that sets them.
bool Initialize(const std::string& path, std::string* error_out);
void Shutdown();

} // namespace CardWindow
