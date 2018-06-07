/* Copyright (C) 2018 Sam Westrick
 * Copyright (C) 2015 Ram Raghunathan.
 *
 * MLton is released under a BSD-style license.
 * See the file MLton-LICENSE for details.
 */

#include "chunk.h"

/******************************/
/* Static Function Prototypes */
/******************************/
/**
 * This function appends 'chunkList' to 'destinationChunkList'.
 *
 * @note
 * 'chunkList's level head is demoted to a regular chunk and its level is
 * switched to 'destinationChunkList's
 *
 * @param destinationChunkList The head of the chunk list to append to. Must be
 * the full level chunk list.
 * @param chunkList the chunk list to append. Must be a full level chunk list,
 * <em>not</em> in a level list
 * @param sentinel The sentinel value to populate the chunkList with in ASSERT
 * builds.
 */
static void appendChunkList(HM_chunkList destinationChunkList,
                            HM_chunkList chunkList,
                            ARG_USED_FOR_ASSERT size_t sentinel);

#if ASSERT
/**
 * This function asserts the chunk invariants
 *
 * @attention
 * If an assertion fails, this function aborts the program, as per the assert()
 * macro.
 *
 * @param chunk The chunk to assert invariants for.
 * @param hh The hierarchical heap 'chunk' belongs to.
 * @param levelHeadChunk The head chunk of the level 'chunk' belongs to
 */
static void HM_assertChunkInvariants(HM_chunk chunk,
                                     HM_chunkList levelHead);

static HM_chunkList getLevelHead(HM_chunk chunk);
#endif

/**
 * This function asserts the chunk list invariants
 *
 * @attention
 * If an assertion fails, this function aborts the program, as per the assert()
 * macro.
 *
 * @param chunkList The chunk list to assert invariants for.
 * @param hh The hierarchical heap the chunks in 'chunkList' belong to.
 */
static void HM_assertChunkListInvariants(HM_chunkList chunkList,
                                         const struct HM_HierarchicalHeap* hh);

/**
 * A function to pass to ChunkPool_iteratedFree() for batch freeing of chunks
 * from a level list
 *
 * @param arg a struct FreeLevelListIteratorArgs* cast to void*
 *
 * @return pointer to chunk if it exists, NULL otherwise.
 */
// void* HM_freeLevelListIterator(void* arg);

#if ASSERT
void assertObjptrInHH(objptr op) {
  assert(HM_getChunkOf(objptrToPointer(op, NULL)));
}
#else
void assertObjptrInHH(objptr op) {
  ((void)op);
}
#endif

/************************/
/* Function Definitions */
/************************/
#if (defined (MLTON_GC_INTERNAL_FUNCS))

size_t HM_BLOCK_SIZE;
size_t HM_ALLOC_SIZE;

void HM_configChunks(GC_state s) {
  assert(isAligned(s->controls->minChunkSize, GC_MODEL_MINALIGN));
  assert(s->controls->minChunkSize >= GC_HEAP_LIMIT_SLOP);
  assert(isAligned(s->controls->allocChunkSize, s->controls->minChunkSize));
  HM_BLOCK_SIZE = s->controls->minChunkSize;
  HM_ALLOC_SIZE = s->controls->allocChunkSize;
}

void HM_prependChunk(HM_chunkList levelHead, HM_chunk chunk) {
  assert(HM_isLevelHead(levelHead));
  assert(HM_isUnlinked(chunk));

  chunk->levelHead = levelHead;
  chunk->nextChunk = levelHead->firstChunk;
  if (levelHead->firstChunk != NULL) {
    levelHead->firstChunk->prevChunk = chunk;
  }
  if (levelHead->lastChunk == NULL) {
    levelHead->lastChunk = chunk;
  }
  levelHead->firstChunk = chunk;
  levelHead->size += HM_getChunkSize(chunk);
}

void HM_appendChunk(HM_chunkList levelHead, HM_chunk chunk) {
  assert(HM_isLevelHead(levelHead));
  assert(HM_isUnlinked(chunk));

  chunk->levelHead = levelHead;
  chunk->prevChunk = levelHead->lastChunk;
  if (levelHead->lastChunk != NULL) {
    levelHead->lastChunk->nextChunk = chunk;
  }
  if (levelHead->firstChunk == NULL) {
    levelHead->firstChunk = chunk;
  }
  levelHead->lastChunk = chunk;
  levelHead->size += HM_getChunkSize(chunk);
}


/* Set up and return a pointer to a new chunk between start and end. Note that
 * the returned pointer is equal to start, and thus each of
 * {start, end, end - start} must be aligned on the block size. */
HM_chunk HM_initializeChunk(pointer start, pointer end);
HM_chunk HM_initializeChunk(pointer start, pointer end) {
  assert(isAligned((size_t)start, HM_BLOCK_SIZE));
  assert(isAligned((size_t)end, HM_BLOCK_SIZE));
  assert(start + HM_BLOCK_SIZE <= end);
  HM_chunk chunk = (HM_chunk)start;

  chunk->frontier = start + sizeof(struct HM_chunk);
  chunk->limit = end;
  chunk->nextChunk = NULL;
  chunk->prevChunk = NULL;
  chunk->nextAdjacent = NULL;
  chunk->prevAdjacent = NULL;
  chunk->levelHead = NULL;
  chunk->mightContainMultipleObjects = TRUE;
  chunk->magic = CHUNK_MAGIC;

#if ASSERT
  /* clear out memory to quickly catch some memory safety errors */
  memset(chunk->frontier, 0xAE, (size_t)(chunk->limit - chunk->frontier));
#endif

  assert(HM_isUnlinked(chunk));
  return chunk;
}

void HM_coalesceChunks(HM_chunk left, HM_chunk right) {
  assert(left->nextAdjacent == right);
  assert(right->prevAdjacent == left);
  assert(left->limit == (pointer)right);
  assert(HM_isUnlinked(left));
  assert(HM_isUnlinked(right));
  assert(left->frontier == HM_getChunkStart(left));
  assert(right->frontier == HM_getChunkStart(right));

  left->limit = right->limit;
  left->nextAdjacent = right->nextAdjacent;

  if (right->nextAdjacent != NULL) {
    right->nextAdjacent->prevAdjacent = left;
  }

// #if ASSERT
//   memset((void*)right, 0xBF, sizeof(struct HM_chunk));
// #endif
}

HM_chunk splitChunkAt(HM_chunk chunk, pointer splitPoint);
HM_chunk splitChunkAt(HM_chunk chunk, pointer splitPoint) {
  assert(chunk->frontier <= splitPoint);
  assert(splitPoint + sizeof(struct HM_chunk) <= chunk->limit);
  assert(isAligned((uintptr_t)splitPoint, HM_BLOCK_SIZE));

  HM_chunkList levelHead = HM_getLevelHeadPathCompress(chunk);

  pointer limit = chunk->limit;
  chunk->limit = splitPoint;
  HM_chunk result = HM_initializeChunk(splitPoint, limit);
  result->levelHead = levelHead;

  if (NULL == chunk->nextChunk) {
    assert(levelHead->lastChunk == chunk);
    levelHead->lastChunk = result;
  } else {
    chunk->nextChunk->prevChunk = result;
  }

  result->prevChunk = chunk;
  result->nextChunk = chunk->nextChunk;
  chunk->nextChunk = result;

  if (chunk->nextAdjacent != NULL) {
    chunk->nextAdjacent->prevAdjacent = result;
  }
  result->nextAdjacent = chunk->nextAdjacent;
  result->prevAdjacent = chunk;
  chunk->nextAdjacent = result;

// #if ASSERT
//   HM_assertChunkListInvariants(levelHead, getHierarchicalHeapCurrent(s));
// #endif

  assert(chunk->nextChunk == result);
  assert(chunk->nextAdjacent == result);
  assert(result->prevChunk == chunk);
  assert(result->prevAdjacent == chunk);
  assert(chunk->limit == (pointer)result);
  if (result->nextAdjacent != NULL) {
    assert(result->limit == (pointer)result->nextAdjacent);
    assert(result->nextAdjacent->prevAdjacent == result);
  }

  return result;
}

HM_chunk HM_splitChunk(HM_chunk chunk, size_t bytesRequested) {
  assert((size_t)(chunk->limit - chunk->frontier) >= bytesRequested);
  assert(!HM_isUnlinked(chunk));

  size_t totalSize = bytesRequested + sizeof(struct HM_chunk);
  totalSize = align(totalSize, HM_BLOCK_SIZE);

  pointer limit = chunk->limit;
  pointer splitPoint = limit - totalSize;

  if (splitPoint < chunk->frontier) {
    // not enough space to split this chunk
    return NULL;
  }

  return splitChunkAt(chunk, splitPoint);
}

HM_chunk HM_splitChunkFront(HM_chunk chunk, size_t bytesRequested);
HM_chunk HM_splitChunkFront(HM_chunk chunk, size_t bytesRequested) {
  assert((size_t)(chunk->limit - chunk->frontier) >= bytesRequested);
  assert(!HM_isUnlinked(chunk));

  pointer splitPoint = (pointer)(uintptr_t)align((uintptr_t)(chunk->frontier + bytesRequested), HM_BLOCK_SIZE);

  if (splitPoint == chunk->limit) {
    // not enough space to split this chunk
    return NULL;
  }

  return splitChunkAt(chunk, (pointer)splitPoint);
}

HM_chunk mmapNewChunk(size_t chunkWidth);
HM_chunk mmapNewChunk(size_t chunkWidth) {
  assert(isAligned(chunkWidth, HM_BLOCK_SIZE));
  size_t bs = HM_BLOCK_SIZE;
  pointer start = (pointer)GC_mmapAnon(NULL, chunkWidth + bs);
  if (NULL == start) {
    return NULL;
  }
  start = (pointer)(uintptr_t)align((size_t)start, bs);
  HM_chunk result = HM_initializeChunk(start, start + chunkWidth);

  LOG(LM_CHUNK, LL_INFO,
    "Mapped a new region of size %zu",
    chunkWidth + bs);

  return result;
}

HM_chunk HM_getFreeChunk(GC_state s, size_t bytesRequested);
HM_chunk HM_getFreeChunk(GC_state s, size_t bytesRequested) {
  HM_chunk chunk = s->freeChunks->firstChunk;
  if (chunk == NULL || (size_t)(chunk->limit - chunk->frontier) < bytesRequested) {
    size_t bytesNeeded = align(bytesRequested + sizeof(struct HM_chunk), HM_BLOCK_SIZE);
    size_t allocSize = max(bytesNeeded, s->nextChunkAllocSize);
    s->nextChunkAllocSize = 2 * allocSize;
    chunk = mmapNewChunk(allocSize);
    HM_prependChunk(s->freeChunks, chunk);
  }

  HM_splitChunkFront(chunk, bytesRequested);
  HM_unlinkChunk(chunk);
  return chunk;
}

HM_chunk HM_allocateChunk(HM_chunkList levelHead, size_t bytesRequested) {
  assert(HM_isLevelHead(levelHead));
  HM_chunk chunk = HM_getFreeChunk(pthread_getspecific(gcstate_key), bytesRequested);

  if (NULL == chunk) {
    return NULL;
  }

  HM_appendChunk(levelHead, chunk);

  LOG(LM_CHUNK, LL_DEBUG,
      "Allocate chunk %p at level %u",
      (void*)chunk,
      levelHead->level);

  return chunk;
}

HM_chunkList HM_newChunkList(struct HM_HierarchicalHeap* hh, Word32 level) {

  // SAM_NOTE: replace with custom arena allocation if a performance bottleneck
  HM_chunkList list = (HM_chunkList) malloc(sizeof(struct HM_chunkList));

  list->firstChunk = NULL;
  list->lastChunk = NULL;
  list->parent = list;
  list->nextHead = NULL;
  list->containingHH = hh;
  list->toChunkList = NULL;
  list->size = 0;
  list->isInToSpace = (hh == COPY_OBJECT_HH_VALUE);
  list->level = level;

  return list;
}

HM_chunk HM_allocateLevelHeadChunk(HM_chunkList * levelList,
                                   size_t bytesRequested,
                                   Word32 level,
                                   struct HM_HierarchicalHeap* hh)
{
  HM_chunk chunk = HM_getFreeChunk(pthread_getspecific(gcstate_key), bytesRequested);

  if (NULL == chunk) {
    return NULL;
  }

  HM_chunkList levelHead = HM_newChunkList(hh, level);
  HM_appendChunk(levelHead, chunk);

  /* insert into level list */
  HM_mergeLevelList(levelList,
                    levelHead,
                    hh,
                    false);

  LOG(LM_CHUNK, LL_DEBUG,
      "Allocate chunk %p at level %u",
      (void*)chunk,
      level);

  return chunk;
}

void HM_unlinkChunk(HM_chunk chunk) {
  HM_chunkList levelHead = HM_getLevelHeadPathCompress(chunk);

  if (NULL == chunk->prevChunk) {
    assert(levelHead->firstChunk == chunk);
    levelHead->firstChunk = chunk->nextChunk;
  } else {
    assert(levelHead->firstChunk != chunk);
    chunk->prevChunk->nextChunk = chunk->nextChunk;
  }

  if (NULL == chunk->nextChunk) {
    assert(levelHead->lastChunk == chunk);
    levelHead->lastChunk = chunk->prevChunk;
  } else {
    assert(levelHead->lastChunk != chunk);
    chunk->nextChunk->prevChunk = chunk->prevChunk;
  }

  levelHead->size -= HM_getChunkSize(chunk);

  chunk->levelHead = NULL;
  chunk->prevChunk = NULL;
  chunk->nextChunk = NULL;

#if ASSERT
  HM_assertChunkListInvariants(levelHead, levelHead->containingHH);
#endif

  assert(HM_isUnlinked(chunk));
}

void HM_mergeFreeList(HM_chunkList parentFreeList, HM_chunkList freeList) {
  appendChunkList(parentFreeList, freeList, 0xfeeb1efab1edbabe);
}

void HM_forwardHHObjptrsInChunkList(
  GC_state s,
  pointer start,
  ObjptrPredicateFunction predicate,
  void* predicateArgs,
  struct ForwardHHObjptrArgs* forwardHHObjptrArgs)
{
  HM_chunk chunk;
  if (blockOf(start) == start) {
    /* `start` is on the boundary of a chunk! The actual chunk which "contains"
     * this pointer is therefore the previous chunk. */
    chunk = HM_getChunkOf(start-1);
    assert(start == chunk->limit);
    assert(chunk->frontier == chunk->limit);
  } else {
    chunk = HM_getChunkOf(start);
  }

  pointer p = start;
  size_t i = 0;

  if (chunk == NULL) {
    DIE("could not find chunk of %p", (void*)chunk);
  }

  while (NULL != chunk) {

    /* Can I use foreachObjptrInRange() for this? */
    while (p != chunk->frontier) {
      assert(p < chunk->frontier);
      p = advanceToObjectData(s, p);

      p = foreachObjptrInObject(s,
                                p,
                                FALSE,
                                predicate,
                                predicateArgs,
                                forwardHHObjptr,
                                forwardHHObjptrArgs);
      if ((i++ % 1024) == 0) {
        Trace3(EVENT_COPY,
               (EventInt)forwardHHObjptrArgs->bytesCopied,
               (EventInt)forwardHHObjptrArgs->objectsCopied,
               (EventInt)forwardHHObjptrArgs->stacksCopied);
      }
    }

    Trace3(EVENT_COPY,
           (EventInt)forwardHHObjptrArgs->bytesCopied,
           (EventInt)forwardHHObjptrArgs->objectsCopied,
           (EventInt)forwardHHObjptrArgs->stacksCopied);

    chunk = chunk->nextChunk;
    if (chunk != NULL) {
      p = HM_getChunkStart(chunk);
    }
  }
}

void HM_forwardHHObjptrsInLevelList(
  GC_state s,
  HM_chunkList * levelList,
  ObjptrPredicateFunction predicate,
  void* predicateArgs,
  struct ForwardHHObjptrArgs* forwardHHObjptrArgs,
  bool expectEntanglement)
{
  Word32 savedMaxLevel = forwardHHObjptrArgs->maxLevel;
  forwardHHObjptrArgs->maxLevel = 0;

  for (HM_chunkList levelHead = *levelList;
       NULL != levelHead;
       levelHead = levelHead->nextHead) {
    LOCAL_USED_FOR_ASSERT void* savedLevelList = *levelList;

    assert(levelHead->firstChunk != NULL);

    LOG(LM_HH_COLLECTION, LL_DEBUG,
        "Sweeping level %u in %p",
        levelHead->level,
        (void*)levelList);

    /* RAM_NOTE: Changing of maxLevel here is redundant sometimes */
    if (expectEntanglement) {
      forwardHHObjptrArgs->maxLevel = savedMaxLevel;
    } else {
      forwardHHObjptrArgs->maxLevel = levelHead->level;
    }

    HM_forwardHHObjptrsInChunkList(
      s,
      HM_getChunkStart(levelHead->firstChunk),
      predicate,
      predicateArgs,
      forwardHHObjptrArgs);

    /* asserts that no new lower level has been created */
    assert(savedLevelList == *levelList);
  }

  forwardHHObjptrArgs->maxLevel = savedMaxLevel;
}

void HM_freeChunks(HM_chunkList* levelList, HM_chunkList freeList, Word32 minLevel, bool coalesce) {
  LOG(LM_CHUNK, LL_DEBUGMORE,
      "START FreeChunks levelList = %p, minLevel = %u",
      ((void*)(levelList)),
      minLevel);

  HM_chunkList list = *levelList;
  while (list != NULL && list->level >= minLevel) {
    HM_chunk chunk = list->firstChunk;
    while (NULL != chunk) {
      HM_chunk next = chunk->nextChunk;
      HM_unlinkChunk(chunk);
      chunk->frontier = HM_getChunkStart(chunk);
      chunk->mightContainMultipleObjects = TRUE;
      if (coalesce) {
        if (chunk->prevAdjacent != NULL && HM_getLevelHead(chunk->prevAdjacent) == freeList) {
          assert(chunk->prevAdjacent->nextAdjacent == chunk);
          HM_unlinkChunk(chunk->prevAdjacent);
          chunk = chunk->prevAdjacent;
          HM_coalesceChunks(chunk, chunk->nextAdjacent);
        }
        if (chunk->nextAdjacent != NULL && HM_getLevelHead(chunk->nextAdjacent) == freeList) {
          HM_unlinkChunk(chunk->nextAdjacent);
          HM_coalesceChunks(chunk, chunk->nextAdjacent);
        }
      }
      HM_appendChunk(freeList, chunk);
#if ASSERT
      /* clear out memory to quickly catch some memory safety errors */
      pointer start = HM_getChunkStart(chunk);
      size_t length = (size_t)(chunk->limit - start);
      memset(start, 0xBF, length);
#endif
      chunk = next;
    }
    list = list->nextHead;
  }

  // size_t count = 0;
  // for (HM_chunk chunk = freeList->firstChunk; chunk != NULL; chunk = chunk->nextChunk) {
  //   count++;
  // }
  // printf("After freeing: %zu chunks in freelist\n", count);

  *levelList = list;

  HM_assertChunkListInvariants(freeList, freeList->containingHH);
  LOG(LM_CHUNK, LL_DEBUGMORE,
      "END FreeChunks levelList = %p, minLevel = %u",
      (void*)levelList,
      minLevel);
}

pointer HM_getChunkFrontier(HM_chunk chunk) {
  return chunk->frontier;
}

pointer HM_getChunkLimit(HM_chunk chunk) {
  return chunk->limit;
}

Word64 HM_getChunkSize(HM_chunk chunk) {
  return chunk->limit - (pointer)chunk;
}

pointer HM_getChunkStart(HM_chunk chunk) {
  return (pointer)chunk + sizeof(struct HM_chunk);
}

Word32 HM_getChunkListLevel(HM_chunkList levelHead) {
  assert(HM_isLevelHead(levelHead));
  return levelHead->level;
}

HM_chunk HM_getChunkListLastChunk(HM_chunkList levelHead) {
  if (NULL == levelHead) {
    return NULL;
  }

  assert(HM_isLevelHead(levelHead));
  return levelHead->lastChunk;
}

HM_chunkList HM_getChunkListToChunkList(HM_chunkList levelHead) {
  assert(NULL != levelHead);
  assert(HM_isLevelHead(levelHead));

  return levelHead->toChunkList;
}

/* SAM_NOTE: TODO: presumably looking up the level list with a linear search is
 * not very expensive since the number of levels in well-behaved parallel
 * programs is small. That being said... can't we store them in a dynamically-
 * sized array? */
Word64 HM_getLevelSize(HM_chunkList levelList, Word32 level) {
  HM_chunkList cursor = levelList;
  assert(NULL == cursor || cursor->parent == cursor);
  while (cursor != NULL && cursor->level > level) {
    assert(HM_isLevelHead(cursor));
    cursor = cursor->nextHead;
  }

  if ((NULL == cursor) || cursor->level != level) {
    return 0;
  }

  return cursor->size;
}

void HM_setChunkListToChunkList(HM_chunkList levelHead, HM_chunkList toChunkList) {
  assert(NULL != levelHead);
  assert(HM_isLevelHead(levelHead));

  levelHead->toChunkList = toChunkList;
  LOG(LM_CHUNK, LL_DEBUGMORE,
      "Set toChunkList of chunk %p to %p",
      (void*)levelHead,
      (void*)toChunkList);
}

HM_chunkList HM_getLevelHead(HM_chunk chunk) {
  assert(chunk != NULL);
  HM_chunkList cursor = chunk->levelHead;
  while (cursor != NULL && cursor->parent != cursor) {
    cursor = cursor->parent;
  }
  return cursor;
}

HM_chunkList HM_getLevelHeadPathCompress(HM_chunk chunk) {
  HM_chunkList levelHead = HM_getLevelHead(chunk);
  assert(levelHead != NULL);

  HM_chunkList cursor = chunk->levelHead;
  chunk->levelHead = levelHead;

  /* SAM_NOTE: TODO: free levelheads with reference counting */
  while (cursor != levelHead) {
    HM_chunkList parent = cursor->parent;
    cursor->parent = levelHead;
    cursor = parent;
  }

  return levelHead;
}

void HM_getObjptrInfo(GC_state s,
                      objptr object,
                      struct HM_ObjptrInfo* info) {
  assertObjptrInHH(object);

  HM_chunk chunk = HM_getChunkOf(objptrToPointer(object, s->heap->start));
  assert(NULL != chunk);

  HM_chunkList chunkList = HM_getLevelHeadPathCompress(chunk);

  assert(HM_isLevelHead(chunkList));
  info->hh = chunkList->containingHH;
  info->chunkList = chunkList;
  info->level = chunkList->level;
}

Word32 HM_getHighestLevel(HM_chunkList levelList) {
  if (NULL == levelList) {
    return CHUNK_INVALID_LEVEL;
  }

  return levelList->level;
}

void HM_mergeLevelList(
  HM_chunkList * destinationLevelList,
  HM_chunkList levelList,
  struct HM_HierarchicalHeap * const hh,
  bool resetToFromSpace)
{
  LOG(LM_CHUNK, LL_DEBUG,
      "Merging %p into %p",
      ((void*)(levelList)),
      ((void*)(*destinationLevelList)));

  HM_chunkList newLevelList = NULL;

  /* construct newLevelList */
  {
    HM_chunkList * previousChunkList = &newLevelList;
    HM_chunkList cursor1 = *destinationLevelList;
    HM_chunkList cursor2 = levelList;
    while ((NULL != cursor1) && (NULL != cursor2)) {
      size_t level1 = cursor1->level;
      size_t level2 = cursor2->level;
      assert(HM_isLevelHead(cursor1));
      assert(HM_isLevelHead(cursor2));

      if (level1 > level2) {
        /* append the first list */
        *previousChunkList = cursor1;

        /* advance cursor1 */
        cursor1 = cursor1->nextHead;
      } else if (level1 < level2) {
        /* append the second list */
        *previousChunkList = cursor2;

        /* advance cursor2 */
        cursor2 = cursor2->nextHead;
      } else {
        /* level1 == level2 */
        /* advance cursor 2 early since appendChunkList will unlink it */
        void* savedCursor2 = cursor2;
        cursor2 = cursor2->nextHead;

        /* merge second list into first before inserting */
        appendChunkList(cursor1, savedCursor2, 0xcafed00dbaadf00d);

        /* append the first list */
        *previousChunkList = cursor1;

        /* advance cursor1 */
        cursor1 = cursor1->nextHead;
      }

      /* set HH of this chunk list */
      (*previousChunkList)->containingHH = hh;

      /* advance previousChunkList */
      previousChunkList =
          &((*previousChunkList)->nextHead);
    }

    if (NULL != cursor1) {
      assert(NULL == cursor2);

      /* append the remainder of cursor1 */
      *previousChunkList = cursor1;
    } else if (NULL != cursor2) {
      assert(NULL == cursor1);

      /* append the remainder of cursor2 */
      *previousChunkList = cursor2;
    }

    /* set HH for remaining chunk lists */
    for (HM_chunkList chunkList = *previousChunkList;
         NULL != chunkList;
         chunkList = chunkList->nextHead) {
      chunkList->containingHH = hh;
    }
  }

  /* mark every chunk as in from-space since they have been merged */
  if (resetToFromSpace) {
    for (HM_chunkList chunkList = newLevelList;
         chunkList != NULL;
         chunkList = chunkList->nextHead) {
      chunkList->isInToSpace = false;
    }
  }

#if ASSERT
  if (newLevelList) {
    bool toSpace = newLevelList->containingHH
      == COPY_OBJECT_HH_VALUE;
    HM_assertLevelListInvariants(newLevelList,
                                 hh,
                                 HM_HH_INVALID_LEVEL,
                                 toSpace);
  }
#endif

  /* update destinationChunkList */
  *destinationLevelList = newLevelList;
}

void HM_promoteChunks(HM_chunkList * levelList, size_t level) {
  LOG(LM_CHUNK, LL_DEBUG,
      "Promoting level %zu in level list %p",
      level,
      ((void*)(*levelList)));

  const struct HM_HierarchicalHeap* hh =
    (*levelList)->containingHH;

  HM_assertLevelListInvariants(*levelList, hh, HM_HH_INVALID_LEVEL, false);

  /* find the pointer to level list of level 'level' */
  HM_chunkList * cursor;
  for (cursor = levelList;

#if ASSERT
       (NULL != *cursor) &&
#endif
                ((*cursor)->level > level);

       cursor = &((*cursor)->nextHead)) {
    assert(HM_isLevelHead(*cursor));
  }
  assert(HM_isLevelHead(*cursor));

  assert(NULL != *cursor);
  if ((*cursor)->level < level) {
    /* no chunks to promote */
    HM_assertLevelListInvariants(*levelList, hh, HM_HH_INVALID_LEVEL, false);
    return;
  }

  HM_chunkList chunkList = *cursor;
  /* unlink level list */
  *cursor = chunkList->nextHead;

  if ((NULL != *cursor) && (level - 1 == (*cursor)->level)) {
    /* need to merge into cursor */
    appendChunkList(*cursor, chunkList, 0xcafed00dbaadd00d);
  } else {
    /* need to reassign levelList to level - 1 */
    assert((NULL == *cursor) || (level - 1 > (*cursor)->level));
    chunkList->level = level - 1;

    /* insert chunkList where *cursor is */
    chunkList->nextHead = *cursor;
    *cursor = chunkList;
  }

  HM_assertLevelListInvariants(*levelList, hh, HM_HH_INVALID_LEVEL, false);
}

#if ASSERT
void HM_assertChunkInLevelList(HM_chunkList levelList, HM_chunk chunk) {
  for (HM_chunkList chunkList = levelList;
       NULL != chunkList;
       chunkList = chunkList->nextHead) {
    for (HM_chunk cursor = chunkList->firstChunk;
         NULL != cursor;
         cursor = cursor->nextChunk) {
      if (chunk == cursor) {
        /* found! */
        return;
      }
    }
  }

  /* If I get here, I couldn't find the chunk */
  ASSERTPRINT(FALSE,
              "Could not find chunk %p!",
              (void*)chunk);
}

void HM_assertLevelListInvariants(HM_chunkList levelList,
                                  const struct HM_HierarchicalHeap* hh,
                                  Word32 stealLevel,
                                  bool inToSpace) {
  Word32 previousLevel = ~((Word32)(0));
  for (HM_chunkList chunkList = levelList;
       NULL != chunkList;
       chunkList = chunkList->nextHead) {
    Word32 level = chunkList->level;
    struct HM_HierarchicalHeap* levelListHH =
      chunkList->containingHH;

    assert(chunkList->isInToSpace == inToSpace);

    assert(HM_isLevelHead(chunkList));
    assert(level < previousLevel);
    ASSERTPRINT((HM_HH_INVALID_LEVEL == stealLevel) || (level > stealLevel),
      "stealLevel %d; level %d",
      stealLevel,
      level);
    previousLevel = level;

    assert(hh == levelListHH);

    HM_assertChunkListInvariants(chunkList, levelListHH);
  }
}
#else
void HM_assertChunkInLevelList(HM_chunkList levelList, HM_chunk chunk) {
  ((void)(levelList));
  ((void)(chunk));
}

void HM_assertLevelListInvariants(HM_chunkList levelList,
                                  const struct HM_HierarchicalHeap* hh,
                                  Word32 stealLevel,
                                  bool inToSpace) {
  ((void)(levelList));
  ((void)(hh));
  ((void)(stealLevel));
  ((void)(inToSpace));
}
#endif /* ASSERT */

void HM_updateChunkValues(HM_chunk chunk, pointer frontier) {
  assert(chunk->frontier <= frontier && frontier <= chunk->limit);
  chunk->frontier = frontier;
}

void HM_updateLevelListPointers(HM_chunkList levelList,
                                struct HM_HierarchicalHeap* hh) {
  for (HM_chunkList cursor = levelList;
       NULL != cursor;
       cursor = cursor->nextHead) {
    cursor->containingHH = hh;
  }
}
#endif /* MLTON_GC_INTERNAL_FUNCS */

#if ASSERT
HM_chunkList getLevelHead(HM_chunk chunk) {
  HM_chunkList cursor = chunk->levelHead;
  assert(NULL != cursor);
  while (cursor->parent != cursor) {
    cursor = cursor->parent;
    assert(NULL != cursor);
  }

  assert(HM_isLevelHead(cursor));
  return cursor;
}
#endif

void appendChunkList(HM_chunkList list1,
                     HM_chunkList list2,
                     ARG_USED_FOR_ASSERT size_t sentinel) {
  LOG(LM_CHUNK, LL_DEBUGMORE,
      "Appending %p into %p",
      ((void*)(list2)),
      ((void*)(list1)));

  assert(NULL != list1);
  assert(HM_isLevelHead(list1));
  assert(HM_isLevelHead(list2));

  if (NULL == list2) {
    /* nothing to append */
    return;
  }

  if (list1->lastChunk == NULL) {
    assert(list1->firstChunk == NULL);
    list1->firstChunk = list2->firstChunk;
  } else {
    assert(list1->lastChunk->nextChunk == NULL);
    list1->lastChunk->nextChunk = list2->firstChunk;
  }

  if (list2->firstChunk != NULL) {
    list2->firstChunk->prevChunk = list1->lastChunk;
  }

  list1->lastChunk = list2->lastChunk;
  list1->size += list2->size;
  list2->parent = list1;

#if ASSERT
  list2->nextHead = ((void*)(sentinel));
  list2->lastChunk = ((void*)(sentinel));
  list2->containingHH = ((struct HM_HierarchicalHeap*)(sentinel));
  list2->toChunkList = ((void*)(sentinel));
#endif

  HM_assertChunkListInvariants(list1,
                               list1->containingHH);
}

#if ASSERT
void HM_assertChunkInvariants(HM_chunk chunk,
                              HM_chunkList levelHead) {
  assert(HM_getChunkStart(chunk) <= chunk->frontier && chunk->frontier <= chunk->limit);
  assert(levelHead == getLevelHead(chunk));
}

void HM_assertChunkListInvariants(HM_chunkList chunkList,
                                  const struct HM_HierarchicalHeap* hh) {
  assert(HM_isLevelHead(chunkList));
  Word64 size = 0;
  HM_chunk chunk = chunkList->firstChunk;
  while (NULL != chunk) {
    HM_assertChunkInvariants(chunk, chunkList);
    size += HM_getChunkSize(chunk);
    if (chunk->nextChunk == NULL) {
      break;
    }
    assert(chunk->nextChunk->prevChunk == chunk);
    chunk = chunk->nextChunk;
  }

  assert(chunkList->containingHH == hh);
  assert(chunkList->size == size);
  assert(chunkList->lastChunk == chunk);
}
#else
void HM_assertChunkListInvariants(HM_chunkList chunkList,
                                  const struct HM_HierarchicalHeap* hh) {
  ((void)(chunkList));
  ((void)(hh));
}
#endif /* ASSERT */

struct HM_HierarchicalHeap *HM_getObjptrHH(GC_state s, objptr object) {
  struct HM_ObjptrInfo objInfo;
  HM_getObjptrInfo(s, object, &objInfo);
  return objInfo.hh;
}

rwlock_t *HM_getObjptrHHLock(GC_state s, objptr object) {
  return &HM_getObjptrHH(s, object)->lock;
}

bool HM_isObjptrInToSpace(GC_state s, objptr object) {
  /* SAM_NOTE: why is this commented out? why are there two ways to check if
   * an object is in the toSpace? Does promotion use one, while collection
   * uses the other? */
  /* return HM_getObjptrLevelHeadChunk(s, object)->split.levelHead.isInToSpace; */
  HM_chunk c = HM_getChunkOf(objptrToPointer(object, s->heap->start));
  return HM_getLevelHeadPathCompress(c)->containingHH == COPY_OBJECT_HH_VALUE;
}

