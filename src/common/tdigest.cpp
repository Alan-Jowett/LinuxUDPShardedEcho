
/*
 * tdigest.cpp
 *
 * Implementation for `scalable_echo::TDigest`.
 * 
 * @copyright Copyright (c) 2025 WinUDPShardedEcho Contributors
 * SPDX-License-Identifier: MIT
 */

#include "tdigest.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>


/**
 * @brief Construct a TDigest with a given compression parameter.
 */
TDigest::TDigest(double compression)
    : compression_(compression), buffer_(), centroids_(), total_weight_(0.0) {
    if (!(compression_ > 0.0)) throw std::invalid_argument("compression must be > 0");
}

/**
 * @brief Add a sample to the digest. This implementation appends to an
 * internal buffer; call `compress()` to fold buffered samples into centroids.
 */
void TDigest::add(double x) {
    buffer_.push_back(x);
    total_weight_ += 1.0;
}

/**
 * @brief Compress buffered samples and existing centroids into a new set of
 * centroids according to the compression parameter.
 */
void TDigest::compress() {
    if (buffer_.empty() && centroids_.empty()) return;

    // Merge buffer and existing centroids into sorted list of (value, weight)
    std::vector<std::pair<double,double>> merged;
    merged.reserve(buffer_.size() + centroids_.size());

    for (double v : buffer_) merged.emplace_back(v, 1.0);
    for (const auto &c : centroids_) merged.emplace_back(c.mean, c.weight);

    buffer_.clear();

    std::sort(merged.begin(), merged.end(), [](auto &a, auto &b){ return a.first < b.first; });
    build_from(merged);
}

/**
 * @brief Build compressed centroids from a sorted list of (value,weight).
 */
void TDigest::build_from(const std::vector<std::pair<double,double>>& merged) {
    centroids_.clear();
    if (merged.empty()) return;

    double total = 0.0;
    for (const auto &p : merged) total += p.second;

    double k_limit = 4.0 * total / compression_; // heuristic scaling
    double cumulative = 0.0;
    double current_mean = merged[0].first;
    double current_weight = merged[0].second;

    for (size_t i = 1; i < merged.size(); ++i) {
        double v = merged[i].first;
        double w = merged[i].second;
        double projected = cumulative + current_weight + w;
        double q = projected / total; // quantile after adding
        double k = compression_ * q;

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

/**
 * @brief Merge another TDigest into this digest. This is a destructive
 * operation for the receiving digest (it will compress and replace its
 * centroids). The caller must ensure proper synchronization if digests are used concurrently.
 */
void TDigest::merge(const TDigest& other) {
    // Combine centroids and other's centroids + buffers into a merged vector
    std::vector<std::pair<double,double>> merged;
    merged.reserve(centroids_.size() + other.centroids_.size() + other.buffer_.size());

    for (const auto &c : centroids_) merged.emplace_back(c.mean, c.weight);
    for (double v : buffer_) merged.emplace_back(v, 1.0);

    for (const auto &c : other.centroids_) merged.emplace_back(c.mean, c.weight);
    for (double v : other.buffer_) merged.emplace_back(v, 1.0);

    total_weight_ += other.total_weight_;

    std::sort(merged.begin(), merged.end(), [](auto &a, auto &b){ return a.first < b.first; });
    buffer_.clear();
    build_from(merged);
}

/**
 * @brief Estimate the q-th quantile from the digest (including buffered points).
 */
double TDigest::percentile(double q) const {
    if (!(q >= 0.0 && q <= 1.0)) throw std::invalid_argument("q must be in [0,1]");
    if (total_weight_ <= 0.0) return std::numeric_limits<double>::quiet_NaN();

    // If there are buffered points, we need to operate on a merged view.
    std::vector<std::pair<double,double>> merged;
    merged.reserve(centroids_.size() + buffer_.size());
    for (const auto &c : centroids_) merged.emplace_back(c.mean, c.weight);
    for (double v : buffer_) merged.emplace_back(v, 1.0);
    if (!buffer_.empty()) std::sort(merged.begin(), merged.end(), [](auto &a, auto &b){ return a.first < b.first; });

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
void TDigest::reset() {
    buffer_.clear();
    centroids_.clear();
    total_weight_ = 0.0;
}
