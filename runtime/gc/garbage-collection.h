/* Copyright (C) 2009 Matthew Fluet.
 * Copyright (C) 1999-2005 Henry Cejtin, Matthew Fluet, Suresh
 *    Jagannathan, and Stephen Weeks.
 * Copyright (C) 1997-2000 NEC Research Institute.
 *
 * MLton is released under a BSD-style license.
 * See the file MLton-LICENSE for details.
 */

#if (defined (MLTON_GC_INTERNAL_FUNCS))

static void minorGC (GC_state s);
static void majorGC (GC_state s, size_t bytesRequested, bool mayResize);
static inline void growStackCurrent (GC_state s, bool allocInOldGen);
static inline void enterGC (GC_state s);
static inline void leaveGC (GC_state s);
static void performGC (GC_state s,
                       size_t oldGenBytesRequested,
                       size_t nurseryBytesRequested,
                       bool forceMajor,
                       bool mayResize);
static size_t fillGap (pointer start, pointer end);
static void ensureHasHeapBytesFreeAndOrInvariantForMutator (GC_state s,
                                                            bool forceGC,
                                                            bool ensureFrontier,
                                                            bool ensureStack,
                                                            size_t oldGenBytesRequested,
                                                            size_t nurseryBytesRequested);

#endif /* (defined (MLTON_GC_INTERNAL_FUNCS)) */

#if (defined (MLTON_GC_INTERNAL_BASIS))

PRIVATE void GC_collect (GC_state s, size_t bytesRequested, bool force);
/* SPOONHOWER_NOTE: spoons should probably go somewhere else... or just get removed */
#warning Is the return type correct?
PRIVATE pointer FFI_getArgs (GC_state s);

#endif /* (defined (MLTON_GC_INTERNAL_BASIS)) */
