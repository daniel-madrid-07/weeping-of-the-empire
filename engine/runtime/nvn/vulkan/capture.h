// Render-thread capture: LARECOMP_CAPTURE=<file> records LARECOMP_CAPTURE_FRAMES (30) presented frames from present LARECOMP_CAPTURE_AT (600; "<n>s": n seconds
// after the first execute): every command list the backend executes, the host objects the lists name, and the guest memory the CPU side of the backend reads.
// tools/engine/replay replays them without the game, to time the translation. A capture turns async execute off (exec.c backend_async_on) unless
// LARECOMP_ASYNC_EXEC=1 is set.
//
// File: the 8 bytes "NXRCAP01", then records {u32 tag, u32 id, u64 len} + len bytes (8-byte aligned).
// Host pointers inside records are replaced by ids (1-based per kind, 0 = NULL); CALL handle arrays name list ids.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "../nvn.h"

#define CAP_MAGIC "NXRCAP01"
enum {
    CAP_HEADER = 1,  // CapHeader
    CAP_MEMPOOL,     // id: NvnPool
    CAP_TEX,         // id: NvnTexture, .pool = mempool id, .backend = 0
    CAP_PROG,        // id: NvnProgram, backends 0
    CAP_TPOOL,       // id: CapPoolHdr + count NvnTexEntry (.tex = texture id); again when its entries change
    CAP_SPOOL,       // id: CapPoolHdr + count NvnSamplerDesc (.backend = 0); again when they change
    CAP_MEM,         // u64 guest address + bytes (again when the bytes change)
    CAP_LIST,        // id: the list's records, linear (JUMPs followed and dropped), ending with CMD_END
    CAP_EXEC,        // id: backend_execute of that list
    CAP_PRESENT,     // id: backend_present of that texture (a frame ends)
    CAP_STATE,       // exec.c's binding state before the first execute (exec_state_save)
    CAP_END,
};
enum { CAPK_TEX = 1, CAPK_PROG, CAPK_TPOOL, CAPK_SPOOL, CAPK_MEMPOOL, CAPK_N };
typedef struct { uint32_t tag, id; uint64_t len; } CapRec;
typedef struct { uint64_t heap_base, heap_size, frames, start_present, alias_base, alias_size; } CapHeader;   // (alias_*: the
                                                                                                    // alias window, hle_os_mem.cpp; absent in the first captures)
typedef struct { uint64_t addr; int32_t count, pad; } CapPoolHdr;
// cmdbuf.c's record payloads that hold host texture pointers (kept in step with cmdbuf.c)
typedef struct { uint64_t tex[8]; NvnTextureView views[8]; uint8_t has_view[8]; uint64_t depth; NvnTextureView dview; uint8_t has_dview; } CapRtRec;
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } CapTexRef;

// exec.c: its binding state with host pointers turned into ids and back (same layout as exec.c's `st`)
size_t exec_state_size(void);
void exec_state_save(void *out, uint64_t (*id_of)(int kind, const void *p));
void exec_state_load(const void *in, void *(*ptr_of)(int kind, uint64_t id));

// capture.c
bool capture_on(void);
void capture_exec(uint64_t handle);    // exec.c, under the exec lock, before the list runs
void capture_present(NvnTexture *t);   // objects.c, nvnQueuePresentTexture

// cmdbuf.c: a command-memory shadow for [base, base + size) (the replay places its lists there)
const CmdChunk *cmd_chunk_replay(uint64_t base, uint64_t size);
