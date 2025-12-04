
/*
 * percentile_estimator.cpp
 *
 * Implementation for `scalable_echo::PercentileEstimator`.
 *
 * @copyright Copyright (c) 2025 WinUDPShardedEcho Contributors
 * SPDX-License-Identifier: MIT
 */

#include "percentile_estimator.hpp"

#include <cmath>
#include <stdexcept>

/**
 * @brief Construct a PercentileEstimator with given capacity.
 */
PercentileEstimator::PercentileEstimator(size_t capacity)
    : capacity_(capacity), samples_(), rng_(std::random_device{}()), seen_(0) {
    if (capacity_ < 1) {
        throw std::invalid_argument("capacity must be >= 1");
    }
    samples_.reserve(capacity_);
}

/**
 * @brief Add a sample to the reservoir. Thread-safe.
 * @param value Sample value to add.
 */
void PercentileEstimator::add(double value) {
    std::lock_guard<std::mutex> lock(mu_);
    ++seen_;
    if (samples_.size() < capacity_) {
        samples_.push_back(value);
        return;
    }

    // Reservoir sampling: replace an existing element with probability capacity_/seen_
    std::uniform_int_distribution<uint64_t> dist(0, seen_ - 1);
    uint64_t idx = dist(rng_);
    if (idx < capacity_) {
        samples_[static_cast<size_t>(idx)] = value;
    }
}

/**
 * @brief Compute an estimated percentile from the retained reservoir.
 * @param fraction Quantile in [0,1].
 * @return Estimated value or NaN if no samples.
 */
double PercentileEstimator::percentile(double fraction) const {
    if (!(fraction >= 0.0 && fraction <= 1.0)) {
        throw std::invalid_argument("fraction must be between 0.0 and 1.0");
    }

    std::lock_guard<std::mutex> lock(mu_);
    if (samples_.empty()) return std::numeric_limits<double>::quiet_NaN();

    // copy samples for sorting to avoid mutating reservoir
    std::vector<double> temp = samples_;
    std::sort(temp.begin(), temp.end());

    // Use nearest-rank method (interpolating could be added later)
    double rank = fraction * (temp.size() - 1);
    size_t lo = static_cast<size_t>(std::floor(rank));
    size_t hi = static_cast<size_t>(std::ceil(rank));
    if (lo == hi) return temp[lo];
    double weight = rank - lo;
    return temp[lo] * (1.0 - weight) + temp[hi] * weight;
}

/**
 * @brief Return total observations seen so far.
 */
size_t PercentileEstimator::count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return static_cast<size_t>(seen_);
}

/**
 * @brief Reset estimator state to empty.
 */
void PercentileEstimator::reset() {
    std::lock_guard<std::mutex> lock(mu_);
    samples_.clear();
    seen_ = 0;
}
