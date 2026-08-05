/*
 * pch.h — Precompiled header for the kimix-core target.
 *
 * Includes the entire KimixBase Core umbrella header plus common
 * system / STL headers used across all core translation units.
 *
 * xmake:
 *   set_pcxxheader("core/pch.h")
 */
#pragma once

// ============================================================================
// System & STL headers — stable, widely used
// ============================================================================

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cassert>
#include <cstdio>
#include <cstdarg>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cinttypes>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#include <deque>
#include <list>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <set>
#include <stack>
#include <variant>
#include <iostream>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <charconv>
#include <random>
#include <initializer_list>

// ============================================================================
// KimixBase Core — umbrella header (includes stl/*, basic_types,
// memory management, platform, file I/O, string utilities, etc.)
// ============================================================================

#include <core/kimix_core.h>
