/**
 * Percentile estimator (reservoir sampling)
 *
 * @file percentile_estimator.hpp
 * @brief Lightweight reservoir-based percentile estimator.
 *
 * This header introduces `scalable_echo::PercentileEstimator`, a small,
 * thread-safe reservoir-sampling estimator suitable for bounded-memory
 * percentile estimation of streaming values.
 * 
 * @copyright Copyright (c) 2025 WinUDPShardedEcho Contributors
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstddef>
#include <vector>
#include <random>
#include <algorithm>
#include <mutex>

/**
 * @class PercentileEstimator
 * @brief Thread-safe reservoir sampling percentile estimator.
 *
 * The estimator retains up to `capacity` samples and uses reservoir
 * sampling to ensure each observed value has an unbiased chance of
 * being retained. Percentiles are computed on-demand by copying and
 * sorting the retained reservoir.
 */
class PercentileEstimator {
public:
    // capacity: max number of samples to retain in reservoir (>=1).
    /**
     * @brief Construct a PercentileEstimator.
     * @param capacity Maximum number of samples to retain (must be >= 1).
     */
    explicit PercentileEstimator(size_t capacity = 8192);

    // Add an observation (response time in microseconds or ms as user prefers).
    /**
     * @brief Add an observation to the estimator.
     * @param value Value to record (units chosen by caller).
     */
    void add(double value);

    // Return estimated percentile in range [0.0, 1.0]. If no samples, returns NaN.
    /**
     * @brief Query the estimated percentile.
     * @param fraction Quantile in range [0.0, 1.0] (e.g. 0.90 for P90).
     * @return Estimated value at the requested quantile or NaN if empty.
     */
    double percentile(double fraction) const;

    // Convenience accessors for common percentiles. Return NaN if empty.
    double p50() const { return percentile(0.50); }
    double p90() const { return percentile(0.90); }
    double p99() const { return percentile(0.99); }

    // Number of samples added overall (may exceed capacity due to reservoir).
    /**
     * @brief Get the number of observations seen so far (total, not reservoir size).
     * @return Total observations processed.
     */
    size_t count() const;

    // Clear all samples and reset counters.
    /**
     * @brief Reset the estimator to an empty state.
     */
    void reset();

private:
    size_t capacity_;
    /** Mutex protecting internal reservoir state. */
    mutable std::mutex mu_;
    /** Reservoir of retained samples. */
    std::vector<double> samples_;
    /** RNG used for reservoir sampling. */
    std::mt19937_64 rng_;
    /** Total number of observations seen (monotonic). */
    uint64_t seen_ = 0; // total items seen
};