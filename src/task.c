// This file is a part of Julia. License is MIT: https://julialang.org/license

/*
  task.c
  lightweight processes (symmetric coroutines)
*/

// need this to get the real definition of ucontext_t,
// if we're going to use the ucontext_t implementation there
//#if defined(__APPLE__) && defined(JL_HAVE_UCONTEXT)
//#pragma push_macro("_XOPEN_SOURCE")
//#define _XOPEN_SOURCE
//#include <ucontext.h>
//#pragma pop_macro("_XOPEN_SOURCE")
//#endif

// this is needed for !COPY_STACKS to work on linux
#ifdef _FORTIFY_SOURCE
// disable __longjmp_chk validation so that we can jump between stacks
// (which would normally be invalid to do with setjmp / longjmp)
#pragma push_macro("_FORTIFY_SOURCE")
#undef _FORTIFY_SOURCE
#include <setjmp.h>
#pragma pop_macro("_FORTIFY_SOURCE")
#endif

#include "platform.h"

#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include "julia.h"
#include "julia_internal.h"
#include "threading.h"
#include "julia_assert.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_COMPILER_ASAN_ENABLED_)
#if __GLIBC__
#include <dlfcn.h>
// Bypass the ASAN longjmp wrapper - we are unpoisoning the stack ourselves,
// since ASAN normally unpoisons far too much.
// c.f. interceptor in jl_dlopen as well
void (*real_siglongjmp)(jmp_buf _Buf, int _Value) = NULL;
#endif
static inline void sanitizer_start_switch_fiber(jl_ptls_t ptls, jl_task_t *from, jl_task_t *to) {
    if (to->copy_stack)
        __sanitizer_start_switch_fiber(&from->ctx.asan_fake_stack, (char*)ptls->stackbase-ptls->stacksize, ptls->stacksize);
    else
        __sanitizer_start_switch_fiber(&from->ctx.asan_fake_stack, to->stkbuf, to->bufsz);
}
static inline void sanitizer_start_switch_fiber_killed(jl_ptls_t ptls, jl_task_t *to) {
    if (to->copy_stack)
        __sanitizer_start_switch_fiber(NULL, (char*)ptls->stackbase-ptls->stacksize, ptls->stacksize);
    else
        __sanitizer_start_switch_fiber(NULL, to->stkbuf, to->bufsz);
}
static inline void sanitizer_finish_switch_fiber(jl_task_t *last, jl_task_t *current) {
    __sanitizer_finish_switch_fiber(current->ctx.asan_fake_stack, NULL, NULL);
        //(const void**)&last->stkbuf,
        //&last->bufsz);
}
#else
static inline void sanitizer_start_switch_fiber(jl_ptls_t ptls, jl_task_t *from, jl_task_t *to) JL_NOTSAFEPOINT {}
static inline void sanitizer_start_switch_fiber_killed(jl_ptls_t ptls, jl_task_t *to) JL_NOTSAFEPOINT {}
static inline void sanitizer_finish_switch_fiber(jl_task_t *last, jl_task_t *current) JL_NOTSAFEPOINT {}
#endif

#if defined(_COMPILER_TSAN_ENABLED_)
// must defined as macros, since the function containing them must not return before the longjmp
#define tsan_destroy_ctx(_ptls, _ctx) do { \
        jl_ucontext_t *_tsan_macro_ctx = (_ctx); \
        if (_tsan_macro_ctx != &(_ptls)->root_task->ctx) { \
            __tsan_destroy_fiber(_tsan_macro_ctx->tsan_state); \
        } \
        _tsan_macro_ctx->tsan_state = NULL; \
    } while (0)
#define tsan_switch_to_ctx(_ctx) do { \
        jl_ucontext_t *_tsan_macro_ctx = (_ctx); \
        __tsan_switch_to_fiber(_tsan_macro_ctx->tsan_state, 0); \
    } while (0)
#ifdef COPY_STACKS
#define tsan_destroy_copyctx(_ptls, _ctx) do { \
        jl_ucontext_t *_tsan_macro_ctx = (_ctx); \
        if (_tsan_macro_ctx != &(_ptls)->root_task->ctx) { \
            __tsan_destroy_fiber(_tsan_macro_ctx->tsan_state); \
        } \
        _tsan_macro_ctx->tsan_state = NULL; \
    } while (0)
#define tsan_switch_to_copyctx(_ctx) do { \
        struct jl_stack_context_t *_tsan_macro_ctx = (_ctx); \
        __tsan_switch_to_fiber(_tsan_macro_ctx->tsan_state, 0); \
    } while (0)
#endif
#else
// just do minimal type-checking on the arguments
#define tsan_destroy_ctx(_ptls, _ctx) do { \
        jl_ucontext_t *_tsan_macro_ctx = (_ctx); \
        (void)_tsan_macro_ctx; \
    } while (0)
#define tsan_switch_to_ctx(_ctx) do { \
        jl_ucontext_t *_tsan_macro_ctx = (_ctx); \
        (void)_tsan_macro_ctx; \
    } while (0)
#ifdef COPY_STACKS
#define tsan_destroy_copyctx(_ptls, _ctx) do { \
        jl_ucontext_t *_tsan_macro_ctx = (_ctx); \
        (void)_tsan_macro_ctx; \
    } while (0)
#define tsan_switch_to_copyctx(_ctx) do { \
        jl_ucontext_t *_tsan_macro_ctx = (_ctx); \
        (void)_tsan_macro_ctx; \
    } while (0)
#endif
#endif

// empirically, jl_finish_task needs about 64k stack space to infer/run
// and additionally, gc-stack reserves 64k for the guard pages
#if defined(MINSIGSTKSZ)
#define MINSTKSZ (MINSIGSTKSZ > 131072 ? MINSIGSTKSZ : 131072)
#else
#define MINSTKSZ 131072
#endif

#ifdef _COMPILER_ASAN_ENABLED_
#define ROOT_TASK_STACK_ADJUSTMENT 0
#else
#define ROOT_TASK_STACK_ADJUSTMENT 3000000
#endif

static char *jl_alloc_fiber(_jl_ucontext_t *t, size_t *ssize, jl_task_t *owner) JL_NOTSAFEPOINT;
static void jl_set_fiber(jl_ucontext_t *t);
static void jl_swap_fiber(jl_ucontext_t *lastt, jl_ucontext_t *t);
static void jl_start_fiber_swap(jl_ucontext_t *savet, jl_ucontext_t *t);
static void jl_start_fiber_set(jl_ucontext_t *t);

#ifdef ALWAYS_COPY_STACKS
# ifndef COPY_STACKS
# error "ALWAYS_COPY_STACKS requires COPY_STACKS"
# endif
static int always_copy_stacks = 1;
#else
static int always_copy_stacks = 0;
#endif

#if defined(_COMPILER_ASAN_ENABLED_)
extern void __asan_get_shadow_mapping(size_t *shadow_scale, size_t *shadow_offset);

JL_NO_ASAN void *memcpy_noasan(void *dest, const void *src, size_t n) {
  char *d = (char*)dest;
  const char *s = (const char *)src;
  for (size_t i = 0; i < n; ++i)
    d[i] = s[i];
  return dest;
}

JL_NO_ASAN void *memcpy_a16_noasan(uint64_t *dest, const uint64_t *src, size_t nb) {
  uint64_t *end = (uint64_t*)((char*)src + nb);
  while (src < end)
    *(dest++) = *(src++);
  return dest;
}

/* Copy stack are allocated as regular bigval objects and do no go through free_stack,
   which would otherwise unpoison it before returning to the GC pool */
static void asan_free_copy_stack(void *stkbuf, size_t bufsz) {
    __asan_unpoison_stack_memory((uintptr_t)stkbuf, bufsz);
}
#else
static void asan_free_copy_stack(void *stkbuf, size_t bufsz) {}
#endif

#ifdef COPY_STACKS
static void JL_NO_ASAN JL_NO_MSAN memcpy_stack_a16(uint64_t *to, uint64_t *from, size_t nb)
{
#if defined(_COMPILER_ASAN_ENABLED_)
    /* Asan keeps shadow memory for everything on the stack. However, in general,
       this function may touch invalid portions of the stack, since it just moves
       the stack around. To keep ASAN's stack tracking capability intact, we need
       to move the shadow memory along with the stack memory itself. */
    size_t shadow_offset;
    size_t shadow_scale;
    __asan_get_shadow_mapping(&shadow_scale, &shadow_offset);
    uintptr_t from_addr = (((uintptr_t)from) >> shadow_scale) + shadow_offset;
    uintptr_t to_addr = (((uintptr_t)to) >> shadow_scale) + shadow_offset;
    // Make sure that the shadow scale is compatible with the alignment, so
    // we can copy whole bytes.
    assert(shadow_scale <= 4);
    size_t shadow_nb = nb >> shadow_scale;
    // Copy over the shadow memory
    memcpy_noasan((char*)to_addr, (char*)from_addr, shadow_nb);
    memcpy_a16_noasan(jl_assume_aligned(to, 16), jl_assume_aligned(from, 16), nb);
#elif defined(_COMPILER_MSAN_ENABLED_)
# warning This function is imcompletely implemented for MSAN (TODO).
    memcpy((char*)jl_assume_aligned(to, 16), (char*)jl_assume_aligned(from, 16), nb);
#else
    memcpy((char*)jl_assume_aligned(to, 16), (char*)jl_assume_aligned(from, 16), nb);
    //uint64_t *end = (uint64_t*)((char*)from + nb);
    //while (from < end)
    //    *(to++) = *(from++);
#endif
}

static void NOINLINE save_stack(jl_ptls_t ptls, jl_task_t *lastt, jl_task_t **pt)
{
    char *frame_addr = (char*)((uintptr_t)jl_get_frame_addr() & ~15);
    char *stackbase = (char*)ptls->stackbase;
    assert(stackbase > frame_addr);
    size_t nb = stackbase - frame_addr;
    void *buf;
    if (lastt->bufsz < nb) {
        asan_free_copy_stack(lastt->stkbuf, lastt->bufsz);
        buf = (void*)jl_gc_alloc_buf(ptls, nb);
        lastt->stkbuf = buf;
        lastt->bufsz = nb;
    }
    else {
        buf = lastt->stkbuf;
    }
    *pt = NULL; // clear the gc-root for the target task before copying the stack for saving
    lastt->copy_stack = nb;
    lastt->sticky = 1;
    memcpy_stack_a16((uint64_t*)buf, (uint64_t*)frame_addr, nb);
    // this task's stack could have been modified after
    // it was marked by an incremental collection
    // move the barrier back instead of walking it again here
    jl_gc_wb_back(lastt);
}

JL_NO_ASAN static void NOINLINE JL_NORETURN restore_stack(jl_task_t *t, jl_ptls_t ptls, char *p)
{
    size_t nb = t->copy_stack;
    char *_x = (char*)ptls->stackbase - nb;
    if (!p) {
        // switch to a stackframe that's beyond the bounds of the last switch
        p = _x;
        if ((char*)&_x > _x) {
            p = (char*)alloca((char*)&_x - _x);
        }
        restore_stack(t, ptls, p); // pass p to ensure the compiler can't tailcall this or avoid the alloca
    }
    void *_y = t->stkbuf;
    assert(_x != NULL && _y != NULL);
    memcpy_stack_a16((uint64_t*)_x, (uint64_t*)_y, nb); // destroys all but the current stackframe

#if defined(_OS_WINDOWS_)
    jl_setcontext(&t->ctx.copy_ctx);
#else
    jl_longjmp(t->ctx.copy_ctx.uc_mcontext, 1);
#endif
    abort(); // unreachable
}

JL_NO_ASAN static void restore_stack2(jl_task_t *t, jl_ptls_t ptls, jl_task_t *lastt)
{
    assert(t->copy_stack && !lastt->copy_stack);
    size_t nb = t->copy_stack;
    char *_x = (char*)ptls->stackbase - nb;
    void *_y = t->stkbuf;
    assert(_x != NULL && _y != NULL);
    memcpy_stack_a16((uint64_t*)_x, (uint64_t*)_y, nb); // destroys all but the current stackframe
#if defined(JL_HAVE_UNW_CONTEXT)
    volatile int returns = 0;
    int r = unw_getcontext(&lastt->ctx.ctx);
    if (++returns == 2) // r is garbage after the first return
        return;
    if (r != 0 || returns != 1)
        abort();
#elif defined(JL_HAVE_ASM) || defined(JL_HAVE_SIGALTSTACK) || defined(_OS_WINDOWS_)
    if (jl_setjmp(lastt->ctx.copy_ctx.uc_mcontext, 0))
        return;
#else
#error COPY_STACKS is incompatible with this platform
#endif
    tsan_switch_to_copyctx(&t->ctx);
#if defined(_OS_WINDOWS_)
    jl_setcontext(&t->ctx.copy_ctx);
#else
    jl_longjmp(t->ctx.copy_ctx.uc_mcontext, 1);
#endif
}
#endif

/* Rooted by the base module */
static _Atomic(jl_function_t*) task_done_hook_func JL_GLOBALLY_ROOTED = NULL;

void JL_NORETURN jl_finish_task(jl_task_t *ct)
{
    JL_PROBE_RT_FINISH_TASK(ct);
    JL_SIGATOMIC_BEGIN();
    if (jl_atomic_load_relaxed(&ct->_isexception))
        jl_atomic_store_release(&ct->_state, JL_TASK_STATE_FAILED);
    else
        jl_atomic_store_release(&ct->_state, JL_TASK_STATE_DONE);
    if (ct->copy_stack) { // early free of stkbuf
        asan_free_copy_stack(ct->stkbuf, ct->bufsz);
        ct->stkbuf = NULL;
    }
    // ensure that state is cleared
    ct->ptls->in_finalizer = 0;
    ct->ptls->in_pure_callback = 0;
    ct->world_age = jl_atomic_load_acquire(&jl_world_counter);
    // let the runtime know this task is dead and find a new task to run
    jl_function_t *done = jl_atomic_load_relaxed(&task_done_hook_func);
    if (done == NULL) {
        done = (jl_function_t*)jl_get_global(jl_base_module, jl_symbol("task_done_hook"));
        if (done != NULL)
            jl_atomic_store_release(&task_done_hook_func, done);
    }
    if (done != NULL) {
        jl_value_t *args[2] = {done, (jl_value_t*)ct};
        JL_TRY {
            jl_apply(args, 2);
        }
        JL_CATCH {
            jl_no_exc_handler(jl_current_exception(ct), ct);
        }
    }
    jl_gc_debug_critical_error();
    abort();
}

JL_DLLEXPORT void *jl_task_stack_buffer(jl_task_t *task, size_t *size, int *ptid)
{
    size_t off = 0;
#ifndef _OS_WINDOWS_
    jl_ptls_t ptls0 = jl_atomic_load_relaxed(&jl_all_tls_states)[0];
    if (ptls0->root_task == task) {
        // See jl_init_root_task(). The root task of the main thread
        // has its buffer enlarged by an artificial 3000000 bytes, but
        // that means that the start of the buffer usually points to
        // inaccessible memory. We need to correct for this.
        off = ROOT_TASK_STACK_ADJUSTMENT;
    }
#endif
    jl_ptls_t ptls2 = task->ptls;
    *ptid = -1;
    if (ptls2) {
        *ptid = jl_atomic_load_relaxed(&task->tid);
#ifdef COPY_STACKS
        if (task->copy_stack) {
            *size = ptls2->stacksize;
            return (char *)ptls2->stackbase - *size;
        }
#endif
    }
    *size = task->bufsz - off;
    return (void *)((char *)task->stkbuf + off);
}

JL_DLLEXPORT void jl_active_task_stack(jl_task_t *task,
                                       char **active_start, char **active_end,
                                       char **total_start, char **total_end)
{
    if (!task->started) {
        *total_start = *active_start = 0;
        *total_end = *active_end = 0;
        return;
    }

    jl_ptls_t ptls2 = task->ptls;
    if (task->copy_stack && ptls2) {
        *total_start = *active_start = (char*)ptls2->stackbase - ptls2->stacksize;
        *total_end = *active_end = (char*)ptls2->stackbase;
    }
    else if (task->stkbuf) {
        *total_start = *active_start = (char*)task->stkbuf;
#ifndef _OS_WINDOWS_
        jl_ptls_t ptls0 = jl_atomic_load_relaxed(&jl_all_tls_states)[0];
        if (ptls0->root_task == task) {
            // See jl_init_root_task(). The root task of the main thread
            // has its buffer enlarged by an artificial 3000000 bytes, but
            // that means that the start of the buffer usually points to
            // inaccessible memory. We need to correct for this.
            *active_start += ROOT_TASK_STACK_ADJUSTMENT;
            *total_start += ROOT_TASK_STACK_ADJUSTMENT;
        }
#endif

        *total_end = *active_end = (char*)task->stkbuf + task->bufsz;
#ifdef COPY_STACKS
        // save_stack stores the stack of an inactive task in stkbuf, and the
        // actual number of used bytes in copy_stack.
        if (task->copy_stack > 1)
            *active_end = (char*)task->stkbuf + task->copy_stack;
#endif
    }
    else {
        // no stack allocated yet
        *total_start = *active_start = 0;
        *total_end = *active_end = 0;
        return;
    }

    if (task == jl_current_task) {
        // scan up to current `sp` for current thread and task
        *active_start = (char*)jl_get_frame_addr();
    }
}

// Marked noinline so we can consistently skip the associated frame.
// `skip` is number of additional frames to skip.
NOINLINE static void record_backtrace(jl_ptls_t ptls, int skip) JL_NOTSAFEPOINT
{
    // storing bt_size in ptls ensures roots in bt_data will be found
    ptls->bt_size = rec_backtrace(ptls->bt_data, JL_MAX_BT_SIZE, skip + 1);
}

JL_DLLEXPORT void jl_set_next_task(jl_task_t *task) JL_NOTSAFEPOINT
{
    jl_current_task->ptls->next_task = task;
}

JL_DLLEXPORT jl_task_t *jl_get_next_task(void) JL_NOTSAFEPOINT
{
    jl_task_t *ct = jl_current_task;
    if (ct->ptls->next_task)
        return ct->ptls->next_task;
    return ct;
}

#ifdef _COMPILER_TSAN_ENABLED_
const char tsan_state_corruption[] = "TSAN state corrupted. Exiting HARD!\n";
#endif

JL_NO_ASAN static void ctx_switch(jl_task_t *lastt)
{
    jl_ptls_t ptls = lastt->ptls;
    jl_task_t **pt = &ptls->next_task;
    jl_task_t *t = *pt;
    assert(t != lastt);
    // none of these locks should be held across a task switch
    assert(ptls->locks.len == 0);

#ifdef _COMPILER_TSAN_ENABLED_
    if (lastt->ctx.tsan_state != __tsan_get_current_fiber()) {
        // Something went really wrong - don't even assume that we can
        // use assert/abort which involve lots of signal handling that
        // looks at the tsan state.
        write(STDERR_FILENO, tsan_state_corruption, sizeof(tsan_state_corruption) - 1);
        _exit(1);
    }
#endif

    int killed = jl_atomic_load_relaxed(&lastt->_state) != JL_TASK_STATE_RUNNABLE;
    if (!t->started && !t->copy_stack) {
        // may need to allocate the stack
        if (t->stkbuf == NULL) {
            t->stkbuf = jl_alloc_fiber(&t->ctx.ctx, &t->bufsz, t);
            if (t->stkbuf == NULL) {
#ifdef COPY_STACKS
                // fall back to stack copying if mmap fails
                t->copy_stack = 1;
                t->sticky = 1;
                t->bufsz = 0;
                if (always_copy_stacks)
                    memcpy(&t->ctx.copy_ctx, &ptls->copy_stack_ctx, sizeof(t->ctx.copy_ctx));
                else
                    memcpy(&t->ctx.ctx, &ptls->base_ctx, sizeof(t->ctx.ctx));
#else
                jl_throw(jl_memory_exception);
#endif
            }
        }
    }

    if (killed) {
        *pt = NULL; // can't fail after here: clear the gc-root for the target task now
        lastt->gcstack = NULL;
        lastt->eh = NULL;
        if (!lastt->copy_stack && lastt->stkbuf) {
            // early free of stkbuf back to the pool
            jl_release_task_stack(ptls, lastt);
        }
    }
    else {
#ifdef COPY_STACKS
        if (lastt->copy_stack) { // save the old copy-stack
            save_stack(ptls, lastt, pt); // allocates (gc-safepoint, and can also fail)
            if (jl_setjmp(lastt->ctx.copy_ctx.uc_mcontext, 0)) {
                sanitizer_finish_switch_fiber(ptls->previous_task, jl_atomic_load_relaxed(&ptls->current_task));
                // TODO: mutex unlock the thread we just switched from
                return;
            }
        }
        else
#endif
        *pt = NULL; // can't fail after here: clear the gc-root for the target task now
    }

    // set up global state for new task and clear global state for old task
    t->ptls = ptls;
    jl_atomic_store_relaxed(&ptls->current_task, t);
    JL_GC_PROMISE_ROOTED(t);
    jl_signal_fence();
    jl_set_pgcstack(&t->gcstack);
    jl_signal_fence();
    lastt->ptls = NULL;
#ifdef MIGRATE_TASKS
    ptls->previous_task = lastt;
#endif

    if (t->started) {
#ifdef COPY_STACKS
        if (t->copy_stack) {
            if (lastt->copy_stack) {
                // Switching from copystack to copystack. Clear any shadow stack
                // memory above the saved shadow stack.
                uintptr_t stacktop = (uintptr_t)ptls->stackbase - t->copy_stack;
                uintptr_t stackbottom = ((uintptr_t)jl_get_frame_addr() & ~15);
                if (stackbottom < stacktop)
                    asan_unpoison_stack_memory(stackbottom, stacktop-stackbottom);
            }
            if (!killed && !lastt->copy_stack) {
                sanitizer_start_switch_fiber(ptls, lastt, t);
                restore_stack2(t, ptls, lastt);
            } else {
                tsan_switch_to_copyctx(&t->ctx);
                if (killed) {
                    sanitizer_start_switch_fiber_killed(ptls, t);
                    tsan_destroy_copyctx(ptls, &lastt->ctx);
                } else {
                    sanitizer_start_switch_fiber(ptls, lastt, t);
                }

                if (lastt->copy_stack) {
                    restore_stack(t, ptls, NULL); // (doesn't return)
                }
                else {
                    restore_stack(t, ptls, (char*)1); // (doesn't return)
                }
            }
        }
        else
#endif
        {
            if (lastt->copy_stack) {
                // Switching away from a copystack to a non-copystack. Clear
                // the whole shadow stack now, because otherwise we won't know
                // how much stack memory to clear the next time we switch to
                // a copystack.
                uintptr_t stacktop = (uintptr_t)ptls->stackbase;
                uintptr_t stackbottom = ((uintptr_t)jl_get_frame_addr() & ~15);
                // We're not restoring the stack, but we still need to unpoison the
                // stack, so it starts with a pristine stack.
                asan_unpoison_stack_memory(stackbottom, stacktop-stackbottom);
            }
            if (killed) {
                sanitizer_start_switch_fiber_killed(ptls, t);
                tsan_switch_to_ctx(&t->ctx);
                tsan_destroy_ctx(ptls, &lastt->ctx);
                jl_set_fiber(&t->ctx); // (doesn't return)
                abort(); // unreachable
            }
            else {
                sanitizer_start_switch_fiber(ptls, lastt, t);
                if (lastt->copy_stack) {
                    // Resume at the jl_setjmp earlier in this function,
                    // don't do a full task swap
                    tsan_switch_to_ctx(&t->ctx);
                    jl_set_fiber(&t->ctx); // (doesn't return)
                }
                else {
                    jl_swap_fiber(&lastt->ctx, &t->ctx);
                }
            }
        }
    }
    else {
        if (lastt->copy_stack) {
            uintptr_t stacktop = (uintptr_t)ptls->stackbase;
            uintptr_t stackbottom = ((uintptr_t)jl_get_frame_addr() & ~15);
            // We're not restoring the stack, but we still need to unpoison the
            // stack, so it starts with a pristine stack.
            asan_unpoison_stack_memory(stackbottom, stacktop-stackbottom);
        }
        if (t->copy_stack && always_copy_stacks) {
            tsan_switch_to_ctx(&t->ctx);
            if (killed) {
                sanitizer_start_switch_fiber_killed(ptls, t);
                tsan_destroy_ctx(ptls, &lastt->ctx);
            } else {
                sanitizer_start_switch_fiber(ptls, lastt, t);
            }
#ifdef COPY_STACKS
#if defined(_OS_WINDOWS_)
            jl_setcontext(&t->ctx.copy_ctx);
#else
            jl_longjmp(t->ctx.copy_ctx.uc_mcontext, 1);
#endif
#endif
            abort(); // unreachable
        }
        else {
            if (killed) {
                sanitizer_start_switch_fiber_killed(ptls, t);
                tsan_switch_to_ctx(&t->ctx);
                tsan_destroy_ctx(ptls, &lastt->ctx);
                jl_start_fiber_set(&t->ctx); // (doesn't return)
                abort();
            }
            sanitizer_start_switch_fiber(ptls, lastt, t);
            if (lastt->copy_stack) {
                // Resume at the jl_setjmp earlier in this function
                tsan_switch_to_ctx(&t->ctx);
                jl_start_fiber_set(&t->ctx); // (doesn't return)
                abort();
            }
            else {
                jl_start_fiber_swap(&lastt->ctx, &t->ctx);
            }
        }
    }
    sanitizer_finish_switch_fiber(ptls->previous_task, jl_atomic_load_relaxed(&ptls->current_task));
}

JL_DLLEXPORT void jl_switch(void) JL_NOTSAFEPOINT_LEAVE JL_NOTSAFEPOINT_ENTER
{
    jl_task_t *ct = jl_current_task;
    jl_ptls_t ptls = ct->ptls;
    jl_task_t *t = ptls->next_task;
    if (t == ct) {
        return;
    }
    int8_t gc_state = jl_gc_unsafe_enter(ptls);
    if (t->started && t->stkbuf == NULL)
        jl_error("attempt to switch to exited task");
    if (ptls->in_finalizer)
        jl_error("task switch not allowed from inside gc finalizer");
    if (ptls->in_pure_callback)
        jl_error("task switch not allowed from inside staged nor pure functions");
    if (!jl_set_task_tid(t, jl_atomic_load_relaxed(&ct->tid))) // manually yielding to a task
        jl_error("cannot switch to task running on another thread");

    JL_PROBE_RT_PAUSE_TASK(ct);

    // Store old values on the stack and reset
    sig_atomic_t defer_signal = ptls->defer_signal;
    int finalizers_inhibited = ptls->finalizers_inhibited;
    ptls->finalizers_inhibited = 0;

    jl_timing_block_t *blk = jl_timing_block_task_exit(ct, ptls);
    ctx_switch(ct);

#ifdef MIGRATE_TASKS
    ptls = ct->ptls;
    t = ptls->previous_task;
    ptls->previous_task = NULL;
    assert(t != ct);
    assert(jl_atomic_load_relaxed(&t->tid) == ptls->tid);
    if (!t->sticky && !t->copy_stack)
        jl_atomic_store_release(&t->tid, -1);
#else
    assert(ptls == ct->ptls);
#endif

    // Pop old values back off the stack
    assert(ct == jl_current_task &&
           0 != ct->ptls &&
           0 == ptls->finalizers_inhibited);
    ptls->finalizers_inhibited = finalizers_inhibited;
    jl_timing_block_task_enter(ct, ptls, blk); (void)blk;

    sig_atomic_t other_defer_signal = ptls->defer_signal;
    ptls->defer_signal = defer_signal;
    if (other_defer_signal && !defer_signal)
        jl_sigint_safepoint(ptls);

    JL_PROBE_RT_RUN_TASK(ct);
    jl_gc_unsafe_leave(ptls, gc_state);
}

JL_DLLEXPORT void jl_switchto(jl_task_t **pt) JL_NOTSAFEPOINT_ENTER // n.b. this does not actually enter a safepoint
{
    jl_set_next_task(*pt);
    jl_switch();
}

JL_DLLEXPORT JL_NORETURN void jl_no_exc_handler(jl_value_t *e, jl_task_t *ct)
{
    // NULL exception objects are used when rethrowing. we don't have a handler to process
    // the exception stack, so at least report the exception at the top of the stack.
    if (!e)
        e = jl_current_exception(ct);

    jl_printf((JL_STREAM*)STDERR_FILENO, "fatal: error thrown and no exception handler available.\n");
    jl_static_show((JL_STREAM*)STDERR_FILENO, e);
    jl_printf((JL_STREAM*)STDERR_FILENO, "\n");
    jlbacktrace(); // written to STDERR_FILENO
    if (ct == NULL)
        jl_raise(6);
    jl_exit(1);
}

/* throw_internal - yield to exception handler */

#ifdef ENABLE_TIMINGS
#define pop_timings_stack()                                                    \
        jl_timing_block_t *cur_block = ptls->timing_stack;                     \
        while (cur_block && eh->timing_stack != cur_block) {                   \
            cur_block = jl_timing_block_pop(cur_block);                        \
        }                                                                      \
        assert(cur_block == eh->timing_stack);
#else
#define pop_timings_stack() /* Nothing */
#endif

#define throw_internal_body(altstack)                                          \
    assert(!jl_get_safe_restore());                                            \
    jl_ptls_t ptls = ct->ptls;                                                 \
    ptls->io_wait = 0;                                                         \
    jl_gc_unsafe_enter(ptls);                                                  \
    if (exception) {                                                           \
        /* The temporary ptls->bt_data is rooted by special purpose code in the\
           GC. This exists only for the purpose of preserving bt_data until we \
           set ptls->bt_size=0 below. */                                       \
        jl_push_excstack(ct, &ct->excstack, exception,                         \
                         ptls->bt_data, ptls->bt_size);                        \
        ptls->bt_size = 0;                                                     \
    }                                                                          \
    assert(ct->excstack && ct->excstack->top);                                 \
    jl_handler_t *eh = ct->eh;                                                 \
    if (eh != NULL) {                                                          \
        if (altstack) ptls->sig_exception = NULL;                              \
        pop_timings_stack()                                                    \
        asan_unpoison_task_stack(ct, &eh->eh_ctx);                             \
        jl_longjmp(eh->eh_ctx, 1);                                             \
    }                                                                          \
    else {                                                                     \
        jl_no_exc_handler(exception, ct);                                      \
    }                                                                          \
    assert(0);

static void JL_NORETURN throw_internal(jl_task_t *ct, jl_value_t *exception JL_MAYBE_UNROOTED)
{
CFI_NORETURN
    JL_GC_PUSH1(&exception);
    throw_internal_body(0);
    jl_unreachable();
}

/* On the signal stack, we don't want to create any asan frames, but we do on the
   normal, stack, so we split this function in two, depending on which context
   we're calling it in. This also lets us avoid making a GC frame on the altstack,
   which might end up getting corrupted if we recur here through another signal. */
JL_NO_ASAN static void JL_NORETURN throw_internal_altstack(jl_task_t *ct, jl_value_t *exception)
{
CFI_NORETURN
    throw_internal_body(1);
    jl_unreachable();
}

// record backtrace and raise an error
JL_DLLEXPORT void jl_throw(jl_value_t *e JL_MAYBE_UNROOTED)
{
    assert(e != NULL);
    jl_jmp_buf *safe_restore = jl_get_safe_restore();
    jl_task_t *ct = jl_get_current_task();
    if (safe_restore) {
        asan_unpoison_task_stack(ct, safe_restore);
        jl_longjmp(*safe_restore, 1);
    }
    if (ct == NULL) // During startup, or on other threads
        jl_no_exc_handler(e, ct);
    record_backtrace(ct->ptls, 1);
    throw_internal(ct, e);
}

// rethrow with current excstack state
JL_DLLEXPORT void jl_rethrow(void)
{
    jl_task_t *ct = jl_current_task;
    jl_excstack_t *excstack = ct->excstack;
    if (!excstack || excstack->top == 0)
        jl_error("rethrow() not allowed outside a catch block");
    throw_internal(ct, NULL);
}

// Special case throw for errors detected inside signal handlers.  This is not
// (cannot be) called directly in the signal handler itself, but is returned to
// after the signal handler exits.
JL_DLLEXPORT JL_NO_ASAN void JL_NORETURN jl_sig_throw(void)
{
CFI_NORETURN
    jl_jmp_buf *safe_restore = jl_get_safe_restore();
    jl_task_t *ct = jl_current_task;
    if (safe_restore) {
        asan_unpoison_task_stack(ct, safe_restore);
        jl_longjmp(*safe_restore, 1);
    }
    jl_ptls_t ptls = ct->ptls;
    jl_value_t *e = ptls->sig_exception;
    JL_GC_PROMISE_ROOTED(e);
    throw_internal_altstack(ct, e);
}

JL_DLLEXPORT void jl_rethrow_other(jl_value_t *e JL_MAYBE_UNROOTED)
{
    // TODO: Should uses of `rethrow(exc)` be replaced with a normal throw, now
    // that exception stacks allow root cause analysis?
    jl_task_t *ct = jl_current_task;
    jl_excstack_t *excstack = ct->excstack;
    if (!excstack || excstack->top == 0)
        jl_error("rethrow(exc) not allowed outside a catch block");
    // overwrite exception on top of stack. see jl_excstack_exception
    jl_excstack_raw(excstack)[excstack->top-1].jlvalue = e;
    JL_GC_PROMISE_ROOTED(e);
    throw_internal(ct, NULL);
}

/* This is xoshiro256++ 1.0, used for tasklocal random number generation in Julia.
   This implementation is intended for embedders and internal use by the runtime, and is
   based on the reference implementation at https://prng.di.unimi.it

   Credits go to David Blackman and Sebastiano Vigna for coming up with this PRNG.
   They described xoshiro256++ in "Scrambled Linear Pseudorandom Number Generators",
   ACM Trans. Math. Softw., 2021.

   There is a pure Julia implementation in stdlib that tends to be faster when used from
   within Julia, due to inlining and more aggressive architecture-specific optimizations.
*/
uint64_t jl_genrandom(uint64_t rngState[4]) JL_NOTSAFEPOINT
{
    uint64_t s0 = rngState[0];
    uint64_t s1 = rngState[1];
    uint64_t s2 = rngState[2];
    uint64_t s3 = rngState[3];

    uint64_t t = s1 << 17;
    uint64_t tmp = s0 + s3;
    uint64_t res = ((tmp << 23) | (tmp >> 41)) + s0;
    s2 ^= s0;
    s3 ^= s1;
    s1 ^= s2;
    s0 ^= s3;
    s2 ^= t;
    s3 = (s3 << 45) | (s3 >> 19);

    rngState[0] = s0;
    rngState[1] = s1;
    rngState[2] = s2;
    rngState[3] = s3;
    return res;
}

/*
The jl_rng_split function forks a task's RNG state in a way that is essentially
guaranteed to avoid collisions between the RNG streams of all tasks. The main
RNG is the xoshiro256++ RNG whose state is stored in rngState[0..3]. There is
also a small internal RNG used for task forking stored in rngState[4]. This
state is used to iterate a linear congruential generator (LCG), which is then
combined with xoshiro256's state and put through four different variations of
the strongest PCG output function, referred to as PCG-RXS-M-XS-64 [1].

The goal of jl_rng_split is to perturb the state of each child task's RNG in
such a way that for an entire tree of tasks spawned starting with a given root
task state, no two tasks have the same RNG state. Moreover, we want to do this
in a way that is deterministic and repeatable based on (1) the root task's seed,
(2) how many random numbers are generated, and (3) the task tree structure. The
RNG state of a parent task is allowed to affect the initial RNG state of a child
task, but the mere fact that a child was spawned should not alter the RNG output
of the parent. This second requirement rules out using the main RNG to seed
children: if we use the main RNG, we either advance it, which affects the
parent's RNG stream or, if we don't advance it, then every child would have an
identical RNG stream. Therefore some separate state must be maintained and
changed upon forking a child task while leaving the main RNG state unchanged.

The basic approach is a generalization and simplification of that used in the
DotMix [2] and SplitMix [3] RNG systems: each task is uniquely identified by a
sequence of "pedigree" numbers, indicating where in the task tree it was
spawned. This vector of pedigree coordinates is then reduced to a single value
by computing a "dot product" with a shared vector of random weights. I write
"dot product" in quotes because what we use is not an actual dot product. The
linear dot product construction used in both DotMix and SplitMix was found by
@foobar_iv2 [4] to allow easy construction of linear relationships between the
main RNG states of tasks, which was in turn reflected in observable linear
relationships between the outputs of their RNGs. This relationship was between a
minimum of four tasks, so doesn't constitute a collision, per se, but is clearly
undesirable and highlights a hazard of the plain dot product construction.

As in DotMix and SplitMix, each task is assigned unique task "pedigree"
coordinates. Our pedigree construction is a bit different and uses only binary
coordinates rather than arbitrary integers. Each pedigree is an infinite
sequence of ones and zeros with only finitely many ones. Each task has a "fork
index": the root task has index 0; the fork index of a task the jth child task
of a parent task with fork index i is i+j. The root task's coordinates are all
zeros; each child task's coordinates are the same as its parents except at its
fork index, where the parent has a zero while the child has a one. A task's
coordinates after its fork index are all zeros. The coordinates of a tasks
ancestors are all prefixes of its own coordinates, padded with zeros.

Also as in DotMix and SplitMix, we generate a sequence of pseudorandom weights
to combine with the coordinates of each task. This sequence is common across all
tasks, and different mix values for each task derive from their coordinates
being different. In DotMix and SplitMix, this is a literal dot product: the
pseudorandom weights are multiplied by corresponding task coordinate and added
up. While this does provably make collisions as unlikely as randomly assigned
task seeds, this linear construction can be used to create linearly correlated
states between tasks. However, it turns out that the compression construction
need not be linear, commutative, associative, etc. which allows us to avoid any
linear or other obvious correlations between related sets of tasks.

To generalize SplitMix's optimized dot product construction, we similarly
compute each task's compression function value incrementally by combining the
parent's compression value with pseudorandom weight corresponding with the
child's fork index. Formally, if the parent's compression value is c then we can
compute the child's compression value as c′ = f(c, wᵢ) where w is the vector of
pseudorandom weights. What is f? It can be any function that is bijective in
each argument for all values of the other argument:

    * For all c: w ↦ f(c, w) is bijective
    * For all w: c ↦ f(c, w) is bijective

The proof that these requirements are sufficient to ensure collision resistance
is in the linked discussion [4]. DotMix/SplitMix are a special case where f is
just addition. Instead we use a much less simple mixing function:

    1. We use (2c+1)(2w+1)÷2 % 2^64 to mix the bits of c and w
    2. We then apply the PCG-RXS-M-XS-64 output function

The first step thoroughly mixes the bits of the previous compression value and
the pseudorandom weight value using multiplication, which is non-commutative
with xoshiro's operations (xor, shift, rotate). This mixing function is a
bijection on each argument witnessed by these inverses:

    * c′ ↦ (2c′+1)(2w+1)⁻¹÷2 % 2^64
    * w′ ↦ (2c+1)⁻¹(2w′+1)÷2 % 2^64

The second PCG output step is a bijection and designed to be significantly
non-linear -- non-linear enough to mask the linearity of the LCG that drives the
PCG-RXS-M-XS-64 RNG and allows it to pass statistical RNG test suites despite
having the same size state and output. In particular, since this mixing function
is highly non-associative and non-linear, we (hopefully) don't have any
discernible relationship between these values:

    * c₀₀ = c
    * c₁₀ = f(c, wᵢ)
    * c₀₁ = f(c, wⱼ)
    * c₁₁ = f(f(c, wᵢ), wⱼ)

When f is simply `+` then these have a very obvious relationship:

    c₀₀ + c₁₁ == c₁₀ + c₀₁

This relationship holds regardless of what wᵢ and wⱼ are and is precisely what
allows easy creation of correlated tasks with the DotMix/SplitMix construction
that we previously used. Expressing any relationship between these values with
our mixing function would require inverting the PCG output function (doable but
non-trivial), knowing the weights wᵢ and wⱼ, and then applying the inversion
functions for those weights appropriately. Since the weights are pseudo-randomly
generated and not directly observable, this is infeasible.

We maintain an LCG in rngState[4] to generate pseudorandom weights. An LCG by
itself is a very bad RNG, but we combine this one with xoshiro256 state
registers in a non-trivial way and then apply the PCG-RXS-M-XS-64 output
function to that. Even if the xoshiro256 states are all zeros, which they should
never be, the output would be the same as PCG-RXS-M-XS-64, which is a solid
statistical RNG.

Each time a child is forked, we update the LCG in both parent and child tasks,
corresponding to increasing the fork index. In the parent, that's all we have to
do -- the main RNG state remains unchanged. Recall that spawning a child should
*not* affect subsequent RNG draws in the parent. The next time the parent forks
a child, the mixing weight used will be different. In the child, we use the LCG
state to perturb the child's main RNG state registers, rngState[0..3].

Since we want these registers to behave independently, we use four different
variations on f to mix the LCG state with each of the four main RNG registers.
Each variation first xors the LCG state with a different random constant before
combining that value above with the old register state via multiplication; the
PCG-RXS-M-XS-64 output function is then applied to that mixed state, with a
different multiplier constant for each variation / register index. Xor is used
in the first step since we multiply the result with the state immediately after
and multiplication distributes over `+` and commutes with `*`, which makes both
options suspect; multiplication doesn't distribute over or commute with xor. We
also use a different odd multiplier in PCG-RXS-M-XS-64 for each RNG register.
These three sources of variation (different xor constants, different xoshiro256
state, different PCG multipliers) are sufficient for each of the four outputs to
behave statistically independently.

[1]: https://www.pcg-random.org/pdf/hmc-cs-2014-0905.pdf

[2]: http://supertech.csail.mit.edu/papers/dprng.pdf

[3]: https://gee.cs.oswego.edu/dl/papers/oopsla14.pdf

[4]:
https://discourse.julialang.org/t/linear-relationship-between-xoshiro-tasks/110454
*/
void jl_rng_split(uint64_t dst[JL_RNG_SIZE], uint64_t src[JL_RNG_SIZE]) JL_NOTSAFEPOINT
{
    // load and advance the internal LCG state
    uint64_t x = src[4];
    src[4] = dst[4] = x * 0xd1342543de82ef95 + 1;
    // high spectrum multiplier from https://arxiv.org/abs/2001.05304

    // random xor constants
    static const uint64_t a[4] = {
        0x214c146c88e47cb7,
        0xa66d8cc21285aafa,
        0x68c7ef2d7b1a54d4,
        0xb053a7d7aa238c61
    };
    // random odd multipliers
    static const uint64_t m[4] = {
        0xaef17502108ef2d9, // standard PCG multiplier
        0xf34026eeb86766af,
        0x38fd70ad58dd9fbb,
        0x6677f9b93ab0c04d
    };

    // PCG-RXS-M-XS-64 output with four variants
    for (int i = 0; i < 4; i++) {
        uint64_t c = src[i];
        uint64_t w = x ^ a[i];
        c += w*(2*c + 1); // c = (2c+1)(2w+1)÷2 % 2^64 (double bijection)
        c ^= c >> ((c >> 59) + 5);
        c *= m[i];
        c ^= c >> 43;
        dst[i] = c;
    }
}

JL_DLLEXPORT jl_task_t *jl_new_task(jl_function_t *start, jl_value_t *completion_future, size_t ssize)
{
    jl_task_t *ct = jl_current_task;
    jl_task_t *t = (jl_task_t*)jl_gc_alloc(ct->ptls, sizeof(jl_task_t), jl_task_type);
    jl_set_typetagof(t, jl_task_tag, 0);
    JL_PROBE_RT_NEW_TASK(ct, t);
    t->copy_stack = 0;
    if (ssize == 0) {
        // stack size unspecified; use default
        if (always_copy_stacks) {
            t->copy_stack = 1;
            t->bufsz = 0;
        }
        else {
            t->bufsz = JL_STACK_SIZE;
        }
        t->stkbuf = NULL;
    }
    else {
        // user requested dedicated stack of a certain size
        if (ssize < MINSTKSZ)
            ssize = MINSTKSZ;
        t->bufsz = ssize;
        t->stkbuf = jl_alloc_fiber(&t->ctx.ctx, &t->bufsz, t);
        if (t->stkbuf == NULL)
            jl_throw(jl_memory_exception);
    }
    t->next = jl_nothing;
    t->queue = jl_nothing;
    t->tls = jl_nothing;
    jl_atomic_store_relaxed(&t->_state, JL_TASK_STATE_RUNNABLE);
    t->start = start;
    t->result = jl_nothing;
    t->donenotify = completion_future;
    jl_atomic_store_relaxed(&t->_isexception, 0);
    // Inherit scope from parent task
    t->scope = ct->scope;
    // Fork task-local random state from parent
    jl_rng_split(t->rngState, ct->rngState);
    // there is no active exception handler available on this stack yet
    t->eh = NULL;
    t->sticky = 1;
    t->gcstack = NULL;
    t->excstack = NULL;
    t->started = 0;
    t->priority = 0;
    jl_atomic_store_relaxed(&t->tid, t->copy_stack ? jl_atomic_load_relaxed(&ct->tid) : -1); // copy_stacks are always pinned since they can't be moved
    t->threadpoolid = ct->threadpoolid;
    t->ptls = NULL;
    t->world_age = ct->world_age;
    t->reentrant_timing = 0;
    jl_timing_task_init(t);

#ifdef COPY_STACKS
    if (!t->copy_stack) {
#if defined(JL_DEBUG_BUILD)
        memset(&t->ctx, 0, sizeof(t->ctx));
#endif
    }
    else {
        if (always_copy_stacks)
            memcpy(&t->ctx.copy_ctx, &ct->ptls->copy_stack_ctx, sizeof(t->ctx.copy_ctx));
        else
            memcpy(&t->ctx.ctx, &ct->ptls->base_ctx, sizeof(t->ctx.ctx));
    }
#endif
#ifdef _COMPILER_TSAN_ENABLED_
    t->ctx.tsan_state = __tsan_create_fiber(0);
#endif
#ifdef _COMPILER_ASAN_ENABLED_
    t->ctx.asan_fake_stack = NULL;
#endif
    return t;
}

// a version of jl_current_task safe for unmanaged threads
JL_DLLEXPORT jl_task_t *jl_get_current_task(void)
{
    jl_gcframe_t **pgcstack = jl_get_pgcstack();
    return pgcstack == NULL ? NULL : container_of(pgcstack, jl_task_t, gcstack);
}

// Do one-time initializations for task system
void jl_init_tasks(void) JL_GC_DISABLED
{
    char *acs = getenv("JULIA_COPY_STACKS");
    if (acs) {
        if (!strcmp(acs, "1") || !strcmp(acs, "yes"))
            always_copy_stacks = 1;
        else if (!strcmp(acs, "0") || !strcmp(acs, "no"))
            always_copy_stacks = 0;
        else {
            jl_safe_printf("invalid JULIA_COPY_STACKS value: %s\n", acs);
            exit(1);
        }
    }
#ifndef COPY_STACKS
    if (always_copy_stacks) {
        jl_safe_printf("Julia built without COPY_STACKS support");
        exit(1);
    }
#endif
#if defined(_COMPILER_ASAN_ENABLED_) && __GLIBC__
    void *libc_handle = dlopen("libc.so.6", RTLD_NOW | RTLD_NOLOAD);
    if (libc_handle) {
        *(void**)&real_siglongjmp = dlsym(libc_handle, "siglongjmp");
        dlclose(libc_handle);
    }
    if (real_siglongjmp == NULL) {
        jl_safe_printf("failed to get real siglongjmp\n");
        exit(1);
    }
#endif
}

#if defined(_COMPILER_ASAN_ENABLED_)
static void NOINLINE JL_NORETURN _start_task(void);
#endif

static void NOINLINE JL_NORETURN JL_NO_ASAN start_task(void)
{
CFI_NORETURN
#if defined(_COMPILER_ASAN_ENABLED_)
    // First complete the fiber switch, otherwise ASAN will be confused
    // when it unpoisons the stack in _start_task
#ifdef __clang_gcanalyzer__
    jl_task_t *ct = jl_get_current_task();
#else
    jl_task_t *ct = jl_current_task;
#endif
    jl_ptls_t ptls = ct->ptls;
    sanitizer_finish_switch_fiber(ptls->previous_task, ct);
    _start_task();
}

static void NOINLINE JL_NORETURN _start_task(void)
{
CFI_NORETURN
#endif
    // this runs the first time we switch to a task
#ifdef __clang_gcanalyzer__
    jl_task_t *ct = jl_get_current_task();
#else
    jl_task_t *ct = jl_current_task;
#endif
    jl_ptls_t ptls = ct->ptls;
    jl_value_t *res;
    assert(ptls->finalizers_inhibited == 0);

#ifdef MIGRATE_TASKS
    jl_task_t *pt = ptls->previous_task;
    ptls->previous_task = NULL;
    if (!pt->sticky && !pt->copy_stack)
        jl_atomic_store_release(&pt->tid, -1);
#endif

    ct->started = 1;
    JL_PROBE_RT_START_TASK(ct);
    jl_timing_block_task_enter(ct, ptls, NULL);
    if (jl_atomic_load_relaxed(&ct->_isexception)) {
        record_backtrace(ptls, 0);
        jl_push_excstack(ct, &ct->excstack, ct->result,
                         ptls->bt_data, ptls->bt_size);
        res = ct->result;
    }
    else {
        JL_TRY {
            if (ptls->defer_signal) {
                ptls->defer_signal = 0;
                jl_sigint_safepoint(ptls);
            }
            JL_TIMING(ROOT, ROOT);
            res = jl_apply(&ct->start, 1);
        }
        JL_CATCH {
            res = jl_current_exception(ct);
            jl_atomic_store_relaxed(&ct->_isexception, 1);
            goto skip_pop_exception;
        }
skip_pop_exception:;
    }
    ct->result = res;
    jl_gc_wb(ct, ct->result);
    jl_finish_task(ct);
    jl_gc_debug_critical_error();
    abort();
}


#if defined(JL_HAVE_UCONTEXT)
#ifdef _OS_WINDOWS_
#define setcontext jl_setcontext
#define swapcontext jl_swapcontext
#define makecontext jl_makecontext
#endif
static char *jl_alloc_fiber(_jl_ucontext_t *t, size_t *ssize, jl_task_t *owner) JL_NOTSAFEPOINT
{
#ifndef _OS_WINDOWS_
    int r = getcontext(t);
    if (r != 0)
        jl_error("getcontext failed");
#endif
    void *stk = jl_malloc_stack(ssize, owner);
    if (stk == NULL)
        return NULL;
    t->uc_stack.ss_sp = stk;
    t->uc_stack.ss_size = *ssize;
#ifdef _OS_WINDOWS_
    makecontext(t, &start_task);
#else
    t->uc_link = NULL;
    makecontext(t, &start_task, 0);
#endif
    return (char*)stk;
}
static void jl_start_fiber_set(jl_ucontext_t *t)
{
    setcontext(&t->ctx);
}
static void jl_start_fiber_swap(jl_ucontext_t *lastt, jl_ucontext_t *t)
{
    assert(lastt);
    tsan_switch_to_ctx(t);
    swapcontext(&lastt->ctx, &t->ctx);
}
static void jl_swap_fiber(jl_ucontext_t *lastt, jl_ucontext_t *t)
{
    tsan_switch_to_ctx(t);
    swapcontext(&lastt->ctx, &t->ctx);
}
static void jl_set_fiber(jl_ucontext_t *t)
{
    setcontext(&t->ctx);
}
#endif

#if defined(JL_HAVE_UNW_CONTEXT) || defined(JL_HAVE_ASM)
static char *jl_alloc_fiber(_jl_ucontext_t *t, size_t *ssize, jl_task_t *owner)
{
    char *stkbuf = (char*)jl_malloc_stack(ssize, owner);
    if (stkbuf == NULL)
        return NULL;
#ifndef __clang_gcanalyzer__
    ((char**)t)[0] = stkbuf; // stash the stack pointer somewhere for start_fiber
    ((size_t*)t)[1] = *ssize; // stash the stack size somewhere for start_fiber
#endif
    return stkbuf;
}
#endif

#if defined(JL_HAVE_UNW_CONTEXT)
static inline void jl_unw_swapcontext(unw_context_t *old, unw_cursor_t *c)
{
    volatile int returns = 0;
    int r = unw_getcontext(old);
    if (++returns == 2) // r is garbage after the first return
        return;
    if (r != 0 || returns != 1)
        abort();
    unw_resume(c);
}
static void jl_swap_fiber(jl_ucontext_t *lastt, jl_ucontext_t *t)
{
    unw_cursor_t c;
    int r = unw_init_local(&c, &t->ctx);
    if (r < 0)
        abort();
    jl_unw_swapcontext(&lastt->ctx, &c);
}
static void jl_set_fiber(jl_ucontext_t *t)
{
    unw_cursor_t c;
    int r = unw_init_local(&c, &t->ctx);
    if (r < 0)
        abort();
    unw_resume(&c);
}
#elif defined(JL_HAVE_ASM)
static void jl_swap_fiber(jl_ucontext_t *lastt, jl_ucontext_t *t)
{
    if (jl_setjmp(lastt->ctx.uc_mcontext, 0))
        return;
    tsan_switch_to_ctx(t);
    jl_set_fiber(t); // doesn't return
}
static void jl_set_fiber(jl_ucontext_t *t)
{
    jl_longjmp(t->ctx.uc_mcontext, 1);
}
#endif

#if defined(JL_HAVE_UNW_CONTEXT) && !defined(JL_HAVE_ASM)
#if defined(_CPU_X86_) || defined(_CPU_X86_64_)
#define PUSH_RET(ctx, stk) \
    do { \
        stk -= sizeof(uintptr_t); \
        *(uintptr_t*)stk = 0; /* push null RIP/EIP onto the stack */ \
    } while (0)
#elif defined(_CPU_ARM_)
#define PUSH_RET(ctx, stk) \
    if (unw_set_reg(ctx, UNW_ARM_R14, 0)) /* put NULL into the LR */ \
        abort();
#else
#error please define how to simulate a CALL on this platform
#endif
static void jl_start_fiber_set(jl_ucontext_t *t)
{
    unw_cursor_t c;
    char *stk = ((char**)&t->ctx)[0];
    size_t ssize = ((size_t*)&t->ctx)[1];
    uintptr_t fn = (uintptr_t)&start_task;
    stk += ssize;
    int r = unw_getcontext(&t->ctx);
    if (r)
        abort();
    if (unw_init_local(&c, &t->ctx))
        abort();
    PUSH_RET(&c, stk);
#if defined __linux__
#error savannah nongnu libunwind is incapable of setting UNW_REG_SP, as required
#endif
    if (unw_set_reg(&c, UNW_REG_SP, (uintptr_t)stk))
        abort();
    if (unw_set_reg(&c, UNW_REG_IP, fn))
        abort();
    unw_resume(&c); // (doesn't return)
}
static void jl_start_fiber_swap(jl_ucontext_t *lastt, jl_ucontext_t *t)
{
    assert(lastt);
    unw_cursor_t c;
    char *stk = ((char**)&t->ctx)[0];
    size_t ssize = ((size_t*)&t->ctx)[1];
    uintptr_t fn = (uintptr_t)&start_task;
    stk += ssize;
    volatile int returns = 0;
    int r = unw_getcontext(&lastt->ctx);
    if (++returns == 2) // r is garbage after the first return
        return;
    if (r != 0 || returns != 1)
        abort();
    r = unw_getcontext(&t->ctx);
    if (r != 0)
        abort();
    if (unw_init_local(&c, &t->ctx))
        abort();
    PUSH_RET(&c, stk);
    if (unw_set_reg(&c, UNW_REG_SP, (uintptr_t)stk))
        abort();
    if (unw_set_reg(&c, UNW_REG_IP, fn))
        abort();
    jl_unw_swapcontext(&lastt->ctx, &c);
}
#endif

#if defined(JL_HAVE_ASM)
JL_NO_ASAN static void jl_start_fiber_swap(jl_ucontext_t *lastt, jl_ucontext_t *t)
{
    assert(lastt);
#ifdef JL_HAVE_UNW_CONTEXT
    volatile int returns = 0;
    int r = unw_getcontext(&lastt->ctx);
    if (++returns == 2) // r is garbage after the first return
        return;
    if (r != 0 || returns != 1)
        abort();
#else
    if (jl_setjmp(lastt->ctx.uc_mcontext, 0))
        return;
#endif
    tsan_switch_to_ctx(t);
    jl_start_fiber_set(t); // doesn't return
}
JL_NO_ASAN static void jl_start_fiber_set(jl_ucontext_t *t)
{
    char *stk = ((char**)&t->ctx)[0];
    size_t ssize = ((size_t*)&t->ctx)[1];
    uintptr_t fn = (uintptr_t)&start_task;
    stk += ssize;
#ifdef _CPU_X86_64_
    asm volatile (
        " movq %0, %%rsp;\n"
        " movq %1, %%rax;\n"
        " xorq %%rbp, %%rbp;\n"
        " push %%rbp;\n" // instead of RSP
        " jmpq *%%rax;\n" // call `fn` with fake stack frame
        " ud2"
        : : "r"(stk), "r"(fn) : "memory" );
#elif defined(_CPU_X86_)
    asm volatile (
        " movl %0, %%esp;\n"
        " movl %1, %%eax;\n"
        " xorl %%ebp, %%ebp;\n"
        " push %%ebp;\n" // instead of ESP
        " jmpl *%%eax;\n" // call `fn` with fake stack frame
        " ud2"
        : : "r"(stk), "r"(fn) : "memory" );
#elif defined(_CPU_AARCH64_)
    asm volatile(
        " mov sp, %0;\n"
        " mov x29, xzr;\n" // Clear link register (x29) and frame pointer
        " mov x30, xzr;\n" // (x30) to terminate unwinder.
        " br %1;\n" // call `fn` with fake stack frame
        " brk #0x1" // abort
        : : "r" (stk), "r"(fn) : "memory" );
#elif defined(_CPU_ARM_)
    // A "i" constraint on `&start_task` works only on clang and not on GCC.
    asm(" mov sp, %0;\n"
        " mov lr, #0;\n" // Clear link register (lr) and frame pointer
        " mov fp, #0;\n" // (fp) to terminate unwinder.
        " bx %1;\n" // call `fn` with fake stack frame.  While `bx` can change
                    // the processor mode to thumb, this will never happen
                    // because all our addresses are word-aligned.
        " udf #0" // abort
        : : "r" (stk), "r"(fn) : "memory" );
#elif defined(_CPU_PPC64_)
    // N.B.: There is two iterations of the PPC64 ABI.
    // v2 is current and used here. Make sure you have the
    // correct version of the ABI reference when working on this code.
    asm volatile(
        // Move stack (-0x30 for initial stack frame) to stack pointer
        " addi 1, %0, -0x30;\n"
        // Build stack frame
        // Skip local variable save area
        " std 2, 0x28(1);\n" // Save TOC
        // Clear link editor/compiler words
        " std 0, 0x20(1);\n"
        " std 0, 0x18(1);\n"
        // Clear LR/CR save area
        " std 0, 0x10(1);\n"
        " std 0, 0x8(1);\n"
        " std 0, 0x0(1); \n" // Clear back link to terminate unwinder
        " mtlr 0; \n"        // Clear link register
        " mr 12, %1; \n"     // Set up target global entry point
        " mtctr 12; \n"      // Move jump target to counter register
        " bctr; \n"          // branch to counter (lr update disabled)
        " trap; \n"
        : : "r"(stk), "r"(fn) : "memory");
#else
#error JL_HAVE_ASM defined but not implemented for this CPU type
#endif
    __builtin_unreachable();
}
#endif

#if defined(JL_HAVE_SIGALTSTACK)
#if defined(_COMPILER_TSAN_ENABLED_)
#error TSAN support not currently implemented for this tasking model
#endif

static void start_basefiber(int sig)
{
    jl_ptls_t ptls = jl_current_task->ptls;
    if (jl_setjmp(ptls->base_ctx.uc_mcontext, 0))
        start_task(); // sanitizer_finish_switch_fiber is part of start_task
}
static char *jl_alloc_fiber(_jl_ucontext_t *t, size_t *ssize, jl_task_t *owner)
{
    stack_t uc_stack, osigstk;
    struct sigaction sa, osa;
    sigset_t set, oset;
    void *stk = jl_malloc_stack(ssize, owner);
    if (stk == NULL)
        return NULL;
    // setup
    jl_ptls_t ptls = jl_current_task->ptls;
    _jl_ucontext_t base_ctx;
    memcpy(&base_ctx, &ptls->base_ctx, sizeof(base_ctx));
    sigfillset(&set);
    if (pthread_sigmask(SIG_BLOCK, &set, &oset) != 0) {
       jl_free_stack(stk, *ssize);
       jl_error("pthread_sigmask failed");
    }
    uc_stack.ss_sp = stk;
    uc_stack.ss_size = *ssize;
    uc_stack.ss_flags = 0;
    if (sigaltstack(&uc_stack, &osigstk) != 0) {
       jl_free_stack(stk, *ssize);
       jl_error("sigaltstack failed");
    }
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = start_basefiber;
    sa.sa_flags = SA_ONSTACK;
    if (sigaction(SIGUSR2, &sa, &osa) != 0) {
       jl_free_stack(stk, *ssize);
       jl_error("sigaction failed");
    }
    // emit signal
    pthread_kill(pthread_self(), SIGUSR2); // initializes jl_basectx
    sigdelset(&set, SIGUSR2);
    sigsuspend(&set);
    // cleanup
    if (sigaction(SIGUSR2, &osa, NULL) != 0) {
       jl_free_stack(stk, *ssize);
       jl_error("sigaction failed");
    }
    if (osigstk.ss_size < MINSTKSZ && (osigstk.ss_flags | SS_DISABLE))
       osigstk.ss_size = MINSTKSZ;
    if (sigaltstack(&osigstk, NULL) != 0) {
       jl_free_stack(stk, *ssize);
       jl_error("sigaltstack failed");
    }
    if (pthread_sigmask(SIG_SETMASK, &oset, NULL) != 0) {
       jl_free_stack(stk, *ssize);
       jl_error("pthread_sigmask failed");
    }
    if (&ptls->base_ctx != t) {
        memcpy(&t, &ptls->base_ctx, sizeof(base_ctx));
        memcpy(&ptls->base_ctx, &base_ctx, sizeof(base_ctx)); // restore COPY_STACKS context
    }
    return (char*)stk;
}
static void jl_start_fiber_set(jl_ucontext_t *t) {
    jl_longjmp(t->ctx.uc_mcontext, 1); // (doesn't return)
}
static void jl_start_fiber_swap(jl_ucontext_t *lastt, jl_ucontext_t *t)
{
    assert(lastt);
    if (lastt && jl_setjmp(lastt->ctx.uc_mcontext, 0))
        return;
    tsan_switch_to_ctx(t);
    jl_start_fiber_set(t);
}
static void jl_swap_fiber(jl_ucontext_t *lastt, jl_ucontext_t *t)
{
    if (jl_setjmp(lastt->ctx.uc_mcontext, 0))
        return;
    tsan_switch_to_ctx(t);
    jl_start_fiber_set(t); // doesn't return
}
static void jl_set_fiber(jl_ucontext_t *t)
{
    jl_longjmp(t->ctx.uc_mcontext, 1);
}
#endif

#if defined(JL_HAVE_ASYNCIFY)
#if defined(_COMPILER_TSAN_ENABLED_)
#error TSAN support not currently implemented for this tasking model
#endif

static char *jl_alloc_fiber(_jl_ucontext_t *t, size_t *ssize, jl_task_t *owner) JL_NOTSAFEPOINT
{
    void *stk = jl_malloc_stack(ssize, owner);
    if (stk == NULL)
        return NULL;
    t->stackbottom = stk;
    t->stacktop = ((char*)stk) + *ssize;
    return (char*)stk;
}
// jl_*_fiber implemented in js
#endif

// Initialize a root task using the given stack.
jl_task_t *jl_init_root_task(jl_ptls_t ptls, void *stack_lo, void *stack_hi)
{
    assert(ptls->root_task == NULL);
    // We need `gcstack` in `Task` to allocate Julia objects; *including* the `Task` type.
    // However, to allocate a `Task` via `jl_gc_alloc` as done in `jl_init_root_task`,
    // we need the `Task` type itself. We use stack-allocated "raw" `jl_task_t` struct to
    // workaround this chicken-and-egg problem. Note that this relies on GC to be turned
    // off as GC fails because we don't/can't allocate the type tag.
    struct {
        jl_value_t *type;
        jl_task_t value;
    } bootstrap_task = {0};
    jl_set_pgcstack(&bootstrap_task.value.gcstack);
    bootstrap_task.value.ptls = ptls;
    if (jl_nothing == NULL) // make a placeholder
        jl_nothing = jl_gc_permobj(0, jl_nothing_type);
    jl_task_t *ct = (jl_task_t*)jl_gc_alloc(ptls, sizeof(jl_task_t), jl_task_type);
    jl_set_typetagof(ct, jl_task_tag, 0);
    memset(ct, 0, sizeof(jl_task_t));
    void *stack = stack_lo;
    size_t ssize = (char*)stack_hi - (char*)stack_lo;
#ifndef _OS_WINDOWS_
    if (ptls->tid == 0) {
        stack = (void*)((char*)stack - ROOT_TASK_STACK_ADJUSTMENT); // offset our guess of the address of the bottom of stack to cover the guard pages too
        ssize += ROOT_TASK_STACK_ADJUSTMENT; // sizeof stack is known exactly, but not where we are in that stack
    }
#endif
    if (always_copy_stacks) {
        ct->copy_stack = 1;
        ct->stkbuf = NULL;
        ct->bufsz = 0;
    }
    else {
        ct->copy_stack = 0;
        ct->stkbuf = stack;
        ct->bufsz = ssize;
    }

#ifdef USE_TRACY
    char *unique_string = (char *)malloc(strlen("Root") + 1);
    strcpy(unique_string, "Root");
    ct->name = unique_string;
#endif
    ct->started = 1;
    ct->next = jl_nothing;
    ct->queue = jl_nothing;
    ct->tls = jl_nothing;
    jl_atomic_store_relaxed(&ct->_state, JL_TASK_STATE_RUNNABLE);
    ct->start = NULL;
    ct->result = jl_nothing;
    ct->donenotify = jl_nothing;
    jl_atomic_store_relaxed(&ct->_isexception, 0);
    ct->scope = jl_nothing;
    ct->eh = NULL;
    ct->gcstack = NULL;
    ct->excstack = NULL;
    jl_atomic_store_relaxed(&ct->tid, ptls->tid);
    ct->threadpoolid = jl_threadpoolid(ptls->tid);
    ct->sticky = 1;
    ct->ptls = ptls;
    ct->world_age = 1; // OK to run Julia code on this task
    ct->reentrant_timing = 0;
    ptls->root_task = ct;
    jl_atomic_store_relaxed(&ptls->current_task, ct);
    JL_GC_PROMISE_ROOTED(ct);
    jl_set_pgcstack(&ct->gcstack);
    assert(jl_current_task == ct);
    assert(jl_current_task->ptls == ptls);

#ifdef _COMPILER_TSAN_ENABLED_
    ct->ctx.tsan_state = __tsan_get_current_fiber();
#endif
#ifdef _COMPILER_ASAN_ENABLED_
    ct->ctx.asan_fake_stack = NULL;
#endif

    jl_timing_block_task_enter(ct, ptls, NULL);

#ifdef COPY_STACKS
    // initialize the base_ctx from which all future copy_stacks will be copies
    if (always_copy_stacks) {
        // when this is set, we will attempt to corrupt the process stack to switch tasks,
        // although this is unreliable, and thus not recommended
        ptls->stackbase = stack_hi;
        ptls->stacksize = ssize;
#ifdef _OS_WINDOWS_
        ptls->copy_stack_ctx.uc_stack.ss_sp = stack_hi;
        ptls->copy_stack_ctx.uc_stack.ss_size = ssize;
#endif
        if (jl_setjmp(ptls->copy_stack_ctx.uc_mcontext, 0))
            start_task(); // sanitizer_finish_switch_fiber is part of start_task
    }
    else {
        ssize = JL_STACK_SIZE;
        char *stkbuf = jl_alloc_fiber(&ptls->base_ctx, &ssize, NULL);
        if (stkbuf != NULL) {
            ptls->stackbase = stkbuf + ssize;
            ptls->stacksize = ssize;
        }
    }
#endif

    if (jl_options.handle_signals == JL_OPTIONS_HANDLE_SIGNALS_ON)
        jl_install_thread_signal_handler(ptls);

    return ct;
}

JL_DLLEXPORT int jl_is_task_started(jl_task_t *t) JL_NOTSAFEPOINT
{
    return t->started;
}

JL_DLLEXPORT int16_t jl_get_task_tid(jl_task_t *t) JL_NOTSAFEPOINT
{
    return jl_atomic_load_relaxed(&t->tid);
}

JL_DLLEXPORT int8_t jl_get_task_threadpoolid(jl_task_t *t)
{
    return t->threadpoolid;
}


#ifdef _OS_WINDOWS_
#if defined(_CPU_X86_)
extern DWORD32 __readgsdword(int);
extern DWORD32 __readgs(void);
#endif
JL_DLLEXPORT void jl_gdb_dump_threadinfo(void)
{
#if defined(_CPU_X86_64_)
    DWORD64 gs0 = __readgsqword(0x0);
    DWORD64 gs8 = __readgsqword(0x8);
    DWORD64 gs16 = __readgsqword(0x10);
    jl_safe_printf("ThreadId: %u, Stack: %p -- %p to %p, SEH: %p\n",
                   (unsigned)GetCurrentThreadId(),
                   jl_get_frame_addr(),
                   (void*)gs8, (void*)gs16, (void*)gs0);
#elif defined(_CPU_X86_)
    DWORD32 fs0 = __readfsdword(0x0);
    DWORD32 fs4 = __readfsdword(0x4);
    DWORD32 fs8 = __readfsdword(0x8);
    jl_safe_printf("ThreadId: %u, Stack: %p -- %p to %p, SEH: %p\n",
                   (unsigned)GetCurrentThreadId(),
                   jl_get_frame_addr(),
                   (void*)fs4, (void*)fs8, (void*)fs0);
    if (__readgs()) { // WoW64 if GS is non-zero
        DWORD32 gs0 = __readgsdword(0x0);
        DWORD32 gs4 = __readgsdword(0x4);
        DWORD32 gs8 = __readgsdword(0x8);
        DWORD32 gs12 = __readgsdword(0xc);
        DWORD32 gs16 = __readgsdword(0x10);
        DWORD32 gs20 = __readgsdword(0x14);
        jl_safe_printf("Stack64: %p%p to %p%p, SEH64: %p%p\n",
                       (void*)gs12, (void*)gs8,
                       (void*)gs20, (void*)gs16,
                       (void*)gs4, (void*)gs0);
    }
#else
    jl_safe_printf("ThreadId: %u, Stack: %p\n",
                   (unsigned)GetCurrentThreadId(),
                   jl_get_frame_addr());
#endif
}
#endif


#ifdef __cplusplus
}
#endif
