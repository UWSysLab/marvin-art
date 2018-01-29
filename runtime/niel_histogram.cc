#include "niel_histogram.h"

#include <sstream>

namespace art {

namespace nielinst {

Histogram::Histogram(int numBins, double min, double max) {
    bins_ = new int[numBins];
    min_ = min;
    max_ = max;
    numBins_ = numBins;
    Clear();
}

Histogram::~Histogram() {
    delete bins_;
}

void Histogram::Add(double num) {
    int binIndex = (int)(((num - min_) * numBins_) / (max_ - min_));
    bins_[binIndex]++;
}

void Histogram::Clear() {
    for (int i = 0; i < numBins_; i++) {
        bins_[i] = 0;
    }
}

std::string Histogram::Print(bool scaled) {
    int total = 0;
    for (int i = 0; i < numBins_; i++) {
        total += bins_[i];
    }
    double binWidth = (max_ - min_) / numBins_;
    std::stringstream output;
    for (int i = 0; i < numBins_; i++) {
        double binStart = min_ + i * binWidth;
        double binEnd = min_ + (i + 1) * binWidth;
        output << "[" << binStart << "-" << binEnd << "): ";
        if (scaled) {
            if (total == 0) {
                output << "0";
            }
            else {
                output << (double)bins_[i] / total;
            }
        }
        else {
            output << bins_[i];
        }
        if (i < numBins_ - 1) {
            output << " ";
        }
    }
    return output.str();
}

} // namespace nielinst
} // namespace art
