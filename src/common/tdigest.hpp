/**
 * tdigest.hpp
 *
 * Lightweight t-digest implementation for partitioned, mergeable percentile
 * estimation. 
 *
 * @file tdigest.hpp
 * @brief Mergeable, lock-free t-digest style estimator.
 *
 * @copyright Copyright (c) 2025 WinUDPShardedEcho Contributors
 * SPDX-License-Identifier: MIT
 *
 */
#pragma once

#include <cstddef>
#include <vector>
#include <cmath>

/**
 * @class TDigest
 * @brief Mergeable t-digest implementation without internal locking.
 *
 * TDigest is intended for per-CPU or per-thread usage where each partition
 * accumulates values locally via `add()` and later the digests are merged
 * using `merge()` to compute global percentiles. The implementation buffers
 * raw points and compresses them into centroids when `compress()` or `merge()`
 * is called.
 */
class TDigest {
public:
    /**
     * @brief Construct a TDigest.
     * @param compression Tuning parameter (higher => more accuracy).
     */
    explicit TDigest(double compression = 100.0);

    /**
     * @brief Add a sample (weight = 1). This method is lock-free in this
     * implementation; caller must ensure thread-safety if concurrently used.
     */
    void add(double x);

    /**
     * @brief Estimate the q-th quantile (q in [0,1]). Returns NaN if empty.
     */
    double percentile(double q) const;

    /**
     * @brief Merge another TDigest into this one. Caller must ensure there is
     * no concurrent modification of either digest during the merge.
     */
    void merge(const TDigest& other);

    /**
     * @brief Compress buffered samples into centroids.
     */
    void compress();

    /**
     * @brief Total weight (number of samples added).
     */
    double total_weight() const { return total_weight_; }

    /**
     * @brief Reset digest to empty state.
     */
    void reset();

private:
    struct Centroid { double mean; double weight; };

    double compression_;
    std::vector<double> buffer_; ///< Raw points waiting for compression
    std::vector<Centroid> centroids_; ///< Compressed centroids
    double total_weight_ = 0.0; ///< Sum of weights

    /**
     * @brief Build centroids from a sorted list of (value, weight) pairs.
     */
    void build_from(const std::vector<std::pair<double,double>>& merged);
};

