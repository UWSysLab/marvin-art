#ifndef ART_RUNTIME_NIEL_HISTOGRAM_H_
#define ART_RUNTIME_NIEL_HISTOGRAM_H_

#include <string>

namespace art {

namespace niel {

namespace inst {

class Histogram {
  public:
    Histogram(int numBins, double min, double max);
    ~Histogram();
    void Add(double num);
    void AddMultiple(double num, long count);
    long Count();
    void Clear();
    double GetAverage();
    std::string Print(bool scaled, bool separateLines);
  private:
    static const std::string NEWLINE_DELIM_;
    static const std::string SPACE_DELIM_;

    int numBins_;
    double min_;
    double max_;

    /* state that is reset on Clear() */
    long * bins_;
    long belowMin_;
    long aboveMax_;
    double sum_; // used for average
};

} // namespace inst
} // namespace niel
} // namespace art

#endif // ART_RUNTIME_NIEL_HISTOGRAM_H_
