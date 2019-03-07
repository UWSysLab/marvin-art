#ifndef ART_RUNTIME_MARVIN_SCOPED_TIMER_H_
#define ART_RUNTIME_MARVIN_SCOPED_TIMER_H_

#include "base/logging.h"

namespace art {

namespace marvin {

class ScopedTimer {
  public:
    ScopedTimer(const std::string & name) : timerName(name) {
        LOG(INFO) << "MARVIN starting " << timerName;
        startTime = NanoTime();
    }

    ~ScopedTimer() {
        uint64_t duration = NanoTime() - startTime;
        LOG(INFO) << "MARVIN ending " << timerName << ": took " << PrettyDuration(duration);
    }

  private:
    uint64_t startTime;
    std::string timerName;
};

} // namespace marvin
} // namespace art

#endif // ART_RUNTIME_MARVIN_SCOPED_TIMER_H_
