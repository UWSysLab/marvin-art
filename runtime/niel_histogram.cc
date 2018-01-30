#include "niel_histogram.h"

#include <sstream>

namespace art {

namespace nielinst {

Histogram::Histogram(int numBins, double min, double max) {
    bins_ = new int[numBins];
    belowMin_ = 0;
    aboveMax_ = 0;
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
    if (binIndex < 0) {
        belowMin_++;
    }
    else if (binIndex >= numBins_) {
        aboveMax_++;
    }
    else {
        bins_[binIndex]++;
    }

    sum_ += num;
}

void Histogram::Clear() {
    for (int i = 0; i < numBins_; i++) {
        bins_[i] = 0;
    }
    belowMin_ = 0;
    aboveMax_ = 0;
    sum_ = 0;
}

int Histogram::Count() {
    int count = 0;
    for (int i = 0; i < numBins_; i++) {
        count += bins_[i];
    }
    return count;
}

double Histogram::GetAverage() {
    return sum_ / Count();
}

std::string Histogram::Print(bool scaled) {
    int count = Count();
    double binWidth = (max_ - min_) / numBins_;

    std::stringstream output;

    output << "[-inf," << min_ << "): ";
    if (scaled) {
        if (count == 0) {
            output << "0";
        }
        else {
            output << (double)belowMin_ / count;
        }
    }
    else {
        output << belowMin_;
    }
    output << " ";

    for (int i = 0; i < numBins_; i++) {
        double binStart = min_ + i * binWidth;
        double binEnd = min_ + (i + 1) * binWidth;
        output << "[" << binStart << "," << binEnd << "): ";
        if (scaled) {
            if (count == 0) {
                output << "0";
            }
            else {
                output << (double)bins_[i] / count;
            }
        }
        else {
            output << bins_[i];
        }
        output << " ";
    }

    output << "[" << max_ << ",inf): ";
    if (scaled) {
        if (count == 0) {
            output << "0";
        }
        else {
            output << (double)aboveMax_ / count;
        }
    }
    else {
        output << aboveMax_;
    }

    return output.str();
}

} // namespace nielinst
} // namespace art
