#ifndef ART_RUNTIME_NIEL_HISTOGRAM_H_
#define ART_RUNTIME_NIEL_HISTOGRAM_H_

#include <string>

namespace art {

namespace nielinst {

class Histogram {
  public:
    Histogram(int numBins, double min, double max);
    ~Histogram();
    void Add(double num);
    int Count();
    void Clear();
    double GetAverage();
    std::string Print(bool scaled);
  private:
    int numBins_;
    double min_;
    double max_;

    /* state that is reset on Clear() */
    int * bins_;
    int belowMin_;
    int aboveMax_;
    double sum_; // used for average
};

} // namespace nielinst
} // namespace art

#endif // ART_RUNTIME_NIEL_HISTOGRAM_H_
