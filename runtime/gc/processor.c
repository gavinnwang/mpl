#include "processor.h"

#include <pthread.h>

/****************/
/* Global Types */
/****************/


/***************************/
/* Static Global Variables */
/***************************/
/* variables used in processor initialization */
static volatile bool Proc_beginInit = FALSE;
static volatile int32_t Proc_initializedCount = 0;

/* variables used in Proc_{begin,end}CriticalSection() */
static volatile int32_t Proc_syncCount;
static volatile int32_t Proc_criticalTicket;

/* variables used in Proc_BSP() */
static volatile enum BSPState Proc_bspState;

/* variables used in multiple places */
static struct rusage ru_crit;

/*************/
/* Constants */
/*************/
/* different start values to allow for comparison without arithmetic */
#define Proc_SYNC_COUNT_INITIALIZER 0
#define Proc_SYNC_COUNT_FIRST 1
#define Proc_CRITICAL_TICKET_INITIALIZER -1
#define Proc_BSP_COUNT_INITIALIZER 0
#define Proc_BSP_COUNT_FIRST 1

/************************/
/* Function definitions */
/************************/

/* RAM_NOTE: Lack of barriers in these functions only works on x86! */

int32_t Proc_processorNumber (GC_state s) {
  for (int proc = 0; proc < s->numberOfProcs; proc ++) {
    if (s == &(s->procStates[proc])) {
      return (int32_t)proc;
    }
  }

  /* SPOONHOWER_NOTE: shouldn't get here */
  fprintf (stderr, "don't know my own processor number (signals?)\n");
  exit (1);
  return 0;
}

bool Proc_amPrimary (GC_state s) {
  return Proc_processorNumber (s) == 0;
}

void Proc_waitForInitialization (GC_state s) {
  while (!Proc_beginInit) { }

  __sync_add_and_fetch (&Proc_initializedCount, 1);

  while (!Proc_isInitialized (s)) { }
}

void Proc_signalInitialization (GC_state s) {
  Proc_syncCount = Proc_SYNC_COUNT_INITIALIZER;
  Proc_criticalTicket = Proc_CRITICAL_TICKET_INITIALIZER;
  Proc_bspState = DONE;

  Proc_initializedCount = 1;
  Proc_beginInit = TRUE;

  while (!Proc_isInitialized (s)) { }
}

bool Proc_isInitialized (GC_state s) {
  return Proc_initializedCount == s->numberOfProcs;
}

void Proc_beginCriticalSection (GC_state s) {
  static pthread_mutex_t Proc_syncCountLock = PTHREAD_MUTEX_INITIALIZER;
  static struct rusage ru_sync;

  if (Proc_isInitialized (s)) {
    int32_t myTicket = Proc_processorNumber (s);

    pthread_mutex_lock_safe(&Proc_syncCountLock);
    int32_t mySyncCount = __sync_add_and_fetch(&Proc_syncCount, 1);

    if ((Proc_SYNC_COUNT_FIRST == mySyncCount) && needGCTime(s)) {
      /* first thread in this round, and need to keep track of sync time */
      startTiming (RUSAGE_SELF, &ru_sync);
    }

    if (mySyncCount == s->numberOfProcs) {
      /* We are the last to synchronize, so signal this */
      if (needGCTime (s)) {
        /* deal with the timers */
        stopTiming (RUSAGE_SELF, &ru_sync, &s->cumulativeStatistics->ru_sync);
        startTiming (RUSAGE_SELF, &ru_crit);
      }
      Proc_criticalTicket = 0;
    }
    pthread_mutex_unlock_safe(&Proc_syncCountLock);

    /*
     * This allows for each processor to have its own critical section at each
     * round
     */
    /* RAM_NOTE: This really should be a condition variable */
    while (Proc_criticalTicket != myTicket) {}
  }
  else {
    Proc_syncCount = 1;
  }
}

void Proc_endCriticalSection (GC_state s) {
  if (Proc_isInitialized (s)) {
    int32_t myTicket = __sync_add_and_fetch (&Proc_criticalTicket, 1);
    if (myTicket == s->numberOfProcs) {
      /* We are the last to finish, so allow everyone to leave */

      if (needGCTime (s)) {
        /* deal with timing */
        stopTiming (RUSAGE_SELF, &ru_crit, &s->cumulativeStatistics->ru_crit);
      }

      /* reset for next round */
      Proc_syncCount = Proc_SYNC_COUNT_INITIALIZER;
      Proc_criticalTicket = Proc_CRITICAL_TICKET_INITIALIZER;
      __sync_synchronize ();
    }

    /* RAM_NOTE: This should also be a condition variable */
    while (Proc_criticalTicket >= 0) {}
  }
  else {
    Proc_syncCount = 0;
  }
}

bool Proc_threadInSection (void) {
  return Proc_syncCount > Proc_SYNC_COUNT_INITIALIZER;
}

bool Proc_BSP(GC_state s,
              bspFunction* functions,
              size_t numFunctions,
              void** args) {
  static pthread_mutex_t Proc_bspCountLock = PTHREAD_MUTEX_INITIALIZER;
  static volatile int32_t Proc_bspCount = Proc_BSP_COUNT_INITIALIZER;

  static struct rusage ru_sync;
  static struct rusage ru_bsp;

  static volatile bool initiatorStart = FALSE;
  static volatile bool participantStart = FALSE;
  static volatile size_t numParticipants;
  static volatile size_t numParticipantsFinished;
  static bspFunction * volatile sharedFunctions;
  static volatile size_t sharedNumFunctions;
  static void* * volatile sharedArgs;

  if (!Proc_isInitialized(s)) {
    DIE(s, "Processors are not initialized!");
  }

  bool amInitiator = (NULL != functions);
  enum BSPState bspState = Proc_BSPState();
  if ((IN_PROGRESS == bspState) ||
      (amInitiator && (WAITING == bspState)) ||
      (!amInitiator && (DONE == bspState))) {
    return FALSE;
  }

  pthread_mutex_lock_safe(&Proc_bspCountLock);
  int32_t myBSPCount = __sync_add_and_fetch(&Proc_bspCount, 1);

  if ((Proc_BSP_COUNT_FIRST != myBSPCount) && amInitiator) {
    /* I lost the BSP race */
    assert(WAITING == Proc_BSPState());
    __sync_sub_and_fetch(&Proc_bspCount, 1);
    pthread_mutex_unlock_safe(&Proc_bspCountLock);
    return FALSE;
  }

  if (Proc_BSP_COUNT_FIRST == myBSPCount) {
    if (!amInitiator) {
      /* participant joined a non-existent BSP round */
      pthread_mutex_unlock_safe(&Proc_bspCountLock);
      return FALSE;
    } else {
      assert(amInitiator);
#pragma message "Need to wrap atomics portably"
#if 0
      __atomic_store_n(&Proc_bspState, WAITING, __ATOMIC_SEQ_CST);
#else
      __sync_synchronize();
      Proc_bspState = WAITING;
      __sync_synchronize();
#endif

      if (needGCTime(s)) {
        /* first thread in this round, and need to keep track of sync time */
        startTiming (RUSAGE_SELF, &ru_sync);
      }
    }
  }

  if (myBSPCount == s->numberOfProcs) {
    /* We are the last to synchronize, so signal this */
    if (needGCTime (s)) {
      /* deal with the timers */
      stopTiming (RUSAGE_SELF, &ru_sync, &s->cumulativeStatistics->ru_sync);
      startTiming (RUSAGE_SELF, &ru_bsp);
    }
#pragma message "Need to wrap atomics portably"
#if 0
    __atomic_store_n(&Proc_bspState, IN_PROGRESS, __ATOMIC_SEQ_CST);
#else
      __sync_synchronize();
      Proc_bspState = IN_PROGRESS;
      __sync_synchronize();
#endif
    initiatorStart = TRUE;
  }
  pthread_mutex_unlock_safe(&Proc_bspCountLock);

  if (amInitiator) {
    /* I am the initiator for this BSP round */

    /* wait until everyone is synchronized */
    /* RAM_NOTE: This should also be a condition variable */
    while (FALSE == initiatorStart) { }

    /* setup the BSP */
    /* All BSP rounds start with all processors being participants */
    numParticipants = s->numberOfProcs;
    numParticipantsFinished = 0;
    sharedFunctions = functions;
    sharedNumFunctions = numFunctions;
    sharedArgs = args;

    /* start the BSP round */
    participantStart = TRUE;
  }

  /* wait until initiator starts the round */
  /* RAM_NOTE: This should also be a condition variable */
  while (FALSE == participantStart) { }

  /* cache constant shared values */
  functions = ((bspFunction*)(sharedFunctions));
  numFunctions = ((size_t)(sharedNumFunctions));
  args = ((void**)(sharedArgs));
  for (size_t i = 0; i < numFunctions; i++) {
    if (functions[i](args[i])) {
      /* I continue being a participant */
      __sync_add_and_fetch(&numParticipantsFinished, 1);
    } else {
      /* I am no longer a participant */
      break;
    }

    /*
     * wait until all participants have finished before moving onto the next
     * function
     */
    while (numParticipantsFinished < numParticipants) { }
  }
  /* I am done, so decrement number of participants */
  __sync_sub_and_fetch(&numParticipants, 1);

  if (amInitiator) {
    /*
     * As initiator, I need to stick around until the BSP is finished, even if
     * my participant "alter-ego" finished early
     */
    while (0 != numParticipants) { }

    /* reset for next BSP */
    initiatorStart = FALSE;
    participantStart = FALSE;
    numParticipants = 0;
    numParticipantsFinished = 0;
    sharedFunctions = NULL;
    sharedNumFunctions = 0;
    sharedArgs = NULL;

    /* stop timing */
    stopTiming (RUSAGE_SELF, &ru_bsp, &s->cumulativeStatistics->ru_bsp);

#pragma message "Need to wrap atomics portably"
#if 0
    __atomic_store_n(&Proc_bspState, DONE, __ATOMIC_SEQ_CST);
#else
      __sync_synchronize();
      Proc_bspState = DONE;
      __sync_synchronize();
#endif
  }

  return TRUE;
}

enum BSPState Proc_BSPState(void) {
#pragma message "Need to wrap atomics portably"
#if 0
  return __atomic_load_n(&Proc_bspState, __ATOMIC_SEQ_CST);
#else
  __sync_synchronize();
  enum BSPState bspState = Proc_bspState;
  __sync_synchronize();

  return bspState;
#endif
}
