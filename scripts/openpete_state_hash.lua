-- openpete_state_hash.lua - Lua mirror of core/runtime/run.c's
-- gameplay_hash_compute_ex(). Hashes each state region the openpete
-- runtime hashes, with the same FNV-1a-64 polynomial and the same
-- indirect-pointer dereference logic.
--
-- Used by scripts/pcsx-redux-trace.lua to emit per-frame hashes that the
-- openpete oracle can value-equality-compare against. Keep byte-for-byte
-- aligned with core/include/openpete_fnv1a64.h + the dispatcher in
-- gameplay_hash_compute_ex(); the C-side unit test
-- tests/unit/fnv1a64_test pins the constants we mirror here.
--
-- Returns:
--   {
--     compute = function(mem_ptr, regions, ram_size) -> total_hex,
--                                                       per_region_hex_array,
--     fnv1a64 = function(buf_ptr, len) -> uint64_cdata,
--     OFFSET  = uint64 0xCBF29CE484222325,
--     PRIME   = uint64 0x100000001B3,
--   }

local ffi = require 'ffi'
local bit = require 'bit'

local U64    = ffi.typeof('uint64_t')
local OFFSET = U64(0xCBF29CE484222325ULL)
local PRIME  = U64(0x100000001B3ULL)
local RAM_MASK = 0x1FFFFF
local KIND_DIRECT                  = 0
local KIND_INDIRECT_ARRAY          = 1
local KIND_INDIRECT_ARRAY_END_PTR  = 2
local KIND_INDIRECT_ARRAY_COUNT    = 3

-- FNV-1a-64 over `len` bytes starting at `buf` (uint8_t* cdata).
-- LuaJIT's `bit.bxor` operates correctly on uint64_t cdata, and `*` on
-- two uint64_t cdata wraps mod 2^64. Returns a uint64_t cdata.
local function fnv1a64(buf, len)
    local h = OFFSET
    for i = 0, len - 1 do
        h = bit.bxor(h, U64(buf[i]))
        h = h * PRIME
    end
    return h
end

-- Read a little-endian uint32 from a uint8_t* at offset `off`.
local function read_u32(mem, off)
    return tonumber(mem[off])
         + tonumber(mem[off + 1]) * 0x100
         + tonumber(mem[off + 2]) * 0x10000
         + tonumber(mem[off + 3]) * 0x1000000
end

-- Compute total + per-region hashes. Mirrors gameplay_hash_compute_ex in
-- core/runtime/run.c. Skipped regions get hash 0 (matches the C path).
--
-- mem      : uint8_t* into 2 MB main RAM (PCSX.getMemPtr()).
-- regions  : table from openpete_state_regions.gen.lua (require'd).
-- ram_size : 2*1024*1024 (constant - passed in for symmetry with C API).
local function compute(mem, regions, ram_size)
    local per = {}
    local total = OFFSET
    for r_idx = 1, #regions do
        local reg = regions[r_idx]
        per[r_idx] = U64(0)
        local kind = reg.kind
        local is_indirect = (kind == KIND_INDIRECT_ARRAY
                          or kind == KIND_INDIRECT_ARRAY_END_PTR
                          or kind == KIND_INDIRECT_ARRAY_COUNT)
        local base_off
        local base_vaddr = 0
        local live_len = reg.length
        if is_indirect then
            local ptr_off = bit.band(reg.start, RAM_MASK)
            if ptr_off + 4 > ram_size then
                goto continue
            end
            base_vaddr = read_u32(mem, ptr_off)
            if base_vaddr == 0 then
                goto continue
            end
            base_off = bit.band(base_vaddr, RAM_MASK)
        else
            base_off = bit.band(reg.start, RAM_MASK)
        end
        if kind == KIND_INDIRECT_ARRAY_END_PTR then
            local aux_off = bit.band(reg.aux, RAM_MASK)
            if aux_off + 4 <= ram_size then
                local end_vaddr = read_u32(mem, aux_off)
                if end_vaddr >= base_vaddr then
                    local live = end_vaddr - base_vaddr
                    if live < live_len then live_len = live end
                end
            end
        elseif kind == KIND_INDIRECT_ARRAY_COUNT then
            local aux_off = bit.band(reg.aux, RAM_MASK)
            if aux_off + 4 <= ram_size and reg.elem_size > 0 then
                local cnt  = read_u32(mem, aux_off)
                local live = cnt * reg.elem_size
                if live < live_len then live_len = live end
            end
        end
        local end_off = base_off + live_len
        if end_off > ram_size then end_off = ram_size end
        if end_off <= base_off then goto continue end
        local len = end_off - base_off
        local h = fnv1a64(mem + base_off, len)
        per[r_idx] = h
        -- Update the running total by replaying the same bytes (mirrors C).
        for i = 0, len - 1 do
            total = bit.bxor(total, U64((mem + base_off)[i]))
            total = total * PRIME
        end
        ::continue::
    end
    return total, per
end

return {
    compute = compute,
    fnv1a64 = fnv1a64,
    OFFSET  = OFFSET,
    PRIME   = PRIME,
    KIND_DIRECT                  = KIND_DIRECT,
    KIND_INDIRECT_ARRAY          = KIND_INDIRECT_ARRAY,
    KIND_INDIRECT_ARRAY_END_PTR  = KIND_INDIRECT_ARRAY_END_PTR,
    KIND_INDIRECT_ARRAY_COUNT    = KIND_INDIRECT_ARRAY_COUNT,
}
