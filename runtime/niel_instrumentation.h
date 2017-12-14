#ifndef ART_RUNTIME_NIEL_INSTRUMENTATION_H_
#define ART_RUNTIME_NIEL_INSTRUMENTATION_H_

#include "globals.h"

namespace art {

class Thread;

void NiRecordRosAllocThreadLocalAlloc(Thread * self, size_t size);
void NiRecordRosAllocNormalAlloc(Thread * self, size_t size);
void NiRecordLargeObjectAlloc(Thread * self, size_t size);

void maybePrintLog();

}

#endif
