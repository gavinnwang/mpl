/**
 * @file hierarchical-heap-collection.h
 *
 * @author Jatin Arora
 *
 * @brief
 * Definition of the Concurrent collection interface
 */

#ifndef CONCURRENT_COLLECTION_H_
#define CONCURRENT_COLLECTION_H_
#include "hierarchical-heap.h"
#include "concurrent-stack.h"
#include "objptr.h"


#if (defined (MLTON_GC_INTERNAL_FUNCS))

// Struct to pass around args. repList is the new chunklist.
typedef struct ConcurrentCollectArgs {
	HM_chunkList origList;
	HM_chunkList repList;
	// Can add this for faster isCandidateChunk test by checking for equality
	// Similar to concurrent-collection.c:62
	// HM_chunk cacheChunk;
} ConcurrentCollectArgs;

typedef struct ConcurrentPackage {
	HM_chunkList repList;
//  It is possible that the collection turned off and the stack isn't empty
//	This is a result of the non-atomicity in the write barrier implementation
//	from checking of isCollecting to addition into the stack
	concurrent_stack* rootList;
	//children roots
	objptr snapLeft;
	objptr snapRight;
	bool isCollecting;
} * ConcurrentPackage;

// Assume complete access in this function
// This function constructs a HM_chunkList of reachable chunks without copying them
// Then it adds the remaining chunks to the free list.
// The GC here proceeds here at the chunk level of granularity. i.e if one obj
// in the chunk is live then the whole chunk is.
void CC_collectWithRoots(GC_state s, struct HM_HierarchicalHeap * targetHH, ConcurrentPackage args);

void CC_addToStack(ConcurrentPackage cp, pointer p);

bool CC_isPointerMarked (pointer p);



#endif

#endif