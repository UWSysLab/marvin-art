#include "niel_bivariate_histogram.h"

#include <cmath>
#include <iomanip>
#include <sstream>

namespace art {

namespace nielinst {

#define BIN_FMT std::defaultfloat << std::setprecision(6)
#define SCALED_NUM_FMT std::fixed << std::setprecision(3)

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
    double binWidthX = (maxX_ - minX_) / numBinsX_;
    double binWidthY = (maxY_ - minY_) / numBinsY_;

    std::stringstream output;

    // print Y variable bin labels
    output << BIN_FMT << "X\\Y: ";
    for (int k = 0; k < numBinsY_ + 2; k++) {
        int binNumY = k - 1;
        double binStartY = minY_ + binNumY * binWidthY;
        double binEndY = minY_ + (binNumY + 1) * binWidthY;
        output << "[";
        if (binStartY < minY_) {
            output << "-inf";
        }
        else {
            output << binStartY;
        }
        output << ",";
        if (binEndY > maxY_) {
            output << "inf";
        }
        else {
            output << binEndY;
        }
        output << "): ";
    }
    output << std::endl;

    for (int i = 0; i < numBinsX_ + 2; i++) {
        // print X variable bin label
        int binNumX = i - 1;
        double binStartX = minX_ + binNumX * binWidthX;
        double binEndX = minX_ + (binNumX + 1) * binWidthX;
        output << BIN_FMT << "[";
        if (binStartX < minX_) {
            output << "-inf";
        }
        else {
            output << binStartX;
        }
        output << ",";
        if (binEndX > maxX_) {
            output << "inf";
        }
        else {
            output << binEndX;
        }
        output << "): ";

        for (int j = 0; j < numBinsY_ + 2; j++) {
            if (scaled) {
                output << SCALED_NUM_FMT;
                if (count == 0) {
                    output << 0;
                }
                else {
                    output << (double)bins_[i][j] / count;
                }
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
