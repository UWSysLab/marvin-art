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
    void Clear();
    std::string Print(bool scaled);
  private:
    int * bins_;
    int numBins_;
    double min_;
    double max_;
};

} // namespace nielinst
} // namespace art

#endif // ART_RUNTIME_NIEL_HISTOGRAM_H_
