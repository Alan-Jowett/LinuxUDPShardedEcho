/**
 * tdigest.hpp
 *
 * Lightweight t-digest implementation for partitioned, mergeable percentile
 * estimation.
 *
 * @file tdigest.hpp
 * @brief Mergeable, lock-free t-digest style estimator.
 *
 * @copyright Copyright (c) 2025 LinuxUDPShardedEcho Contributors
 * SPDX-License-Identifier: MIT
 *
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <vector>

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
    explicit TDigest(double compression)
        : compression_(compression), buffer_(), centroids_(), total_weight_(0.0) {
        if (!(compression_ > 0.0)) throw std::invalid_argument("compression must be > 0");
    }

    /**
     * @brief Add a sample to the digest. This implementation appends to an
     * internal buffer; call `compress()` to fold buffered samples into centroids. This method is
     * lock-free in this implementation; caller must ensure thread-safety if concurrently used.
     */
    void add(double x) {
        buffer_.push_back(x);
        total_weight_ += 1.0;
    }

    /**
     * @brief Compress buffered samples and existing centroids into a new set of
     * centroids according to the compression parameter.
     */
    void compress() {
        if (buffer_.empty() && centroids_.empty()) return;

        // Merge buffer and existing centroids into sorted list of (value, weight)
        std::vector<std::pair<double, double>> merged;
        merged.reserve(buffer_.size() + centroids_.size());

        std::transform(buffer_.begin(), buffer_.end(), std::back_inserter(merged),
                       [](double v) { return std::pair<double, double>(v, 1.0); });
        std::transform(
            centroids_.begin(), centroids_.end(), std::back_inserter(merged),
            [](const Centroid& c) { return std::pair<double, double>(c.mean, c.weight); });

        buffer_.clear();

        std::sort(merged.begin(), merged.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        build_from(merged);
    }

    /**
     * @brief Merge another TDigest into this digest. This is a destructive
     * operation for the receiving digest (it will compress and replace its
     * centroids). The caller must ensure proper synchronization if digests are used concurrently.
     */
    void merge(const TDigest& other) {
        // Combine centroids and other's centroids + buffers into a merged vector
        std::vector<std::pair<double, double>> merged;
        merged.reserve(centroids_.size() + other.centroids_.size() + other.buffer_.size());

        std::transform(
            centroids_.begin(), centroids_.end(), std::back_inserter(merged),
            [](const Centroid& c) { return std::pair<double, double>(c.mean, c.weight); });
        std::transform(buffer_.begin(), buffer_.end(), std::back_inserter(merged),
                       [](double v) { return std::pair<double, double>(v, 1.0); });

        std::transform(
            other.centroids_.begin(), other.centroids_.end(), std::back_inserter(merged),
            [](const Centroid& c) { return std::pair<double, double>(c.mean, c.weight); });
        std::transform(other.buffer_.begin(), other.buffer_.end(), std::back_inserter(merged),
                       [](double v) { return std::pair<double, double>(v, 1.0); });

        total_weight_ += other.total_weight_;

        std::sort(merged.begin(), merged.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        buffer_.clear();
        build_from(merged);
    }

    /**
     * @brief Estimate the q-th quantile (q in [0,1]). Returns NaN if empty.
     */
    double percentile(double q) const {
        if (!(q >= 0.0 && q <= 1.0)) throw std::invalid_argument("q must be in [0,1]");
        if (total_weight_ <= 0.0) return std::numeric_limits<double>::quiet_NaN();

        // If there are buffered points, we need to operate on a merged view.
        std::vector<std::pair<double, double>> merged;
        merged.reserve(centroids_.size() + buffer_.size());
        std::transform(
            centroids_.begin(), centroids_.end(), std::back_inserter(merged),
            [](const Centroid& c) { return std::pair<double, double>(c.mean, c.weight); });
        std::transform(buffer_.begin(), buffer_.end(), std::back_inserter(merged),
                       [](double v) { return std::pair<double, double>(v, 1.0); });
        if (!buffer_.empty())
            std::sort(merged.begin(), merged.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });

        // Walk merged list to find desired cumulative weight
        double target = q * total_weight_;
        double cumulative = 0.0;

        if (merged.empty()) return std::numeric_limits<double>::quiet_NaN();

        for (size_t i = 0; i < merged.size(); ++i) {
            double v = merged[i].first;
            double w = merged[i].second;
            if (cumulative + w >= target) {
                // simple linear interpolation within this item
                return v;
            }
            cumulative += w;
        }

        // If we didn't return, target is at or beyond the end — return max
        return merged.back().first;
    }

    /**
     * @brief Reset digest to empty state.
     */
    void reset() {
        buffer_.clear();
        centroids_.clear();
        total_weight_ = 0.0;
    }

    /**
     * @brief Total weight (number of samples added).
     */
    double total_weight() const { return total_weight_; }

   private:
    struct Centroid {
        double mean;
        double weight;
    };

    double compression_;
    std::vector<double> buffer_;       ///< Raw points waiting for compression
    std::vector<Centroid> centroids_;  ///< Compressed centroids
    double total_weight_ = 0.0;        ///< Sum of weights

    /**
     * @brief Build compressed centroids from a sorted list of (value,weight).
     */
    void build_from(const std::vector<std::pair<double, double>>& merged) {
        centroids_.clear();
        if (merged.empty()) return;

        double total = std::accumulate(
            merged.begin(), merged.end(), 0.0,
            [](double sum, const std::pair<double, double>& p) { return sum + p.second; });

        double k_limit = 4.0 * total / compression_;  // heuristic scaling
        double cumulative = 0.0;
        double current_mean = merged[0].first;
        double current_weight = merged[0].second;

        for (size_t i = 1; i < merged.size(); ++i) {
            double v = merged[i].first;
            double w = merged[i].second;
            // projected is intentionally unused in this heuristic
            (void)cumulative;
            (void)current_weight;
            (void)w;

            if (current_weight + w <= std::max(1.0, k_limit)) {
                // merge into current centroid
                current_mean = (current_mean * current_weight + v * w) / (current_weight + w);
                current_weight += w;
            } else {
                // push current and start a new centroid
                centroids_.push_back({current_mean, current_weight});
                cumulative += current_weight;
                current_mean = v;
                current_weight = w;
            }
        }
        // push last
        centroids_.push_back({current_mean, current_weight});
    }
};
