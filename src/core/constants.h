#pragma once

#include <cmath>
#include <numbers>
#include <limits>

namespace kimix {

// ---------------------------------------------------------------------------
// Mathematical constants
// ---------------------------------------------------------------------------

inline constexpr double pi = std::numbers::pi;
inline constexpr double inv_pi = std::numbers::inv_pi;
inline constexpr double pi_over_two = std::numbers::pi / 2.0;
inline constexpr double pi_over_four = std::numbers::pi / 4.0;
inline constexpr double two_pi = std::numbers::pi * 2.0;
inline constexpr double inv_two_pi = 0.5 * std::numbers::inv_pi;
inline constexpr double sqrt2 = std::numbers::sqrt2;
inline constexpr double inv_sqrt2 = 1.0 / std::numbers::sqrt2;
inline constexpr double e = std::numbers::e;
inline constexpr double log2e = std::numbers::log2e;
inline constexpr double log10e = std::numbers::log10e;
inline constexpr double ln2 = std::numbers::ln2;
inline constexpr double ln10 = std::numbers::ln10;

inline constexpr float pi_f = static_cast<float>(pi);
inline constexpr float inv_pi_f = static_cast<float>(inv_pi);
inline constexpr float pi_over_two_f = static_cast<float>(pi_over_two);
inline constexpr float pi_over_four_f = static_cast<float>(pi_over_four);
inline constexpr float two_pi_f = static_cast<float>(two_pi);
inline constexpr float inv_two_pi_f = static_cast<float>(inv_two_pi);

} // namespace kimix
