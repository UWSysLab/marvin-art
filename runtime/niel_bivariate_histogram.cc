#include "niel_bivariate_histogram.h"

#include <cmath>
#include <sstream>

namespace art {

namespace nielinst {

BivariateHistogram::BivariateHistogram(int numBinsX, double minX, double maxX,
                                       int numBinsY, double minY, double maxY) {
    numBinsX_ = numBinsX;
    minX_ = minX;
    maxX_ = maxX;

    numBinsY_ = numBinsY;
    minY_ = minY;
    maxY_ = maxY;

    bins_ = new long*[numBinsX_ + 2];
    for (int i = 0; i < numBinsX_ + 2; i++) {
        bins_[i] = new long[numBinsY_ + 2];
    }

    Clear();
}

BivariateHistogram::~BivariateHistogram() {
    for (int i = 0; i < numBinsX_ + 2; i++) {
        delete[] bins_[i];
    }
    delete[] bins_;
}

void BivariateHistogram::Add(double valX, double valY) {
    int binNumX = (int)std::floor(((valX - minX_) * numBinsX_) / (maxX_ - minX_));
    int binNumY = (int)std::floor(((valY - minY_) * numBinsY_) / (maxY_ - minY_));

    int indexX = binNumX + 1;
    int indexY = binNumY + 1;

    if (indexX < 0) {
        indexX = 0;
    }
    else if (indexX > numBinsX_ + 1) {
        indexX = numBinsX_ + 1;
    }

    if (indexY < 0) {
        indexY = 0;
    }
    else if (indexY > numBinsY_ + 1) {
        indexY = numBinsY_ + 1;
    }

    bins_[indexX][indexY]++;
}

long BivariateHistogram::TotalCount() {
    long count = 0;
    for (int i = 0; i < numBinsX_ + 2; i++) {
        for (int j = 0; j < numBinsY_ + 2; j++) {
            count += bins_[i][j];
        }
    }
    return count;
}

std::string BivariateHistogram::Print(bool scaled) {
    long count = BivariateHistogram::TotalCount();
    std::stringstream output;
    for (int i = 0; i < numBinsX_ + 2; i++) {
        for (int j = 0; j < numBinsY_ + 2; j++) {
            if (scaled) {
                double scaledVal = (double)bins_[i][j] / count;
                output << scaledVal;
            }
            else {
                output << bins_[i][j];
            }

            if (j < numBinsY_ + 1) {
                output << "\t";
            }
        }
        output << std::endl;
    }
    return output.str();
}

void BivariateHistogram::Clear() {
    for (int i = 0; i < numBinsX_ + 2; i++) {
        for (int j = 0; j < numBinsY_ + 2; j++) {
            bins_[i][j] = 0;
        }
    }
}

} // namespace nielinst
} // namespace art
