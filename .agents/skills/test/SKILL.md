---
name: test
description: Boost.UT test layout, adding tests, and running them with xmake.
---

# KimixBase Test Guide

Tests are standalone executables using [Boost.UT](https://github.com/boost-ext/ut), vendored at `tests/ut/ut.hpp`. Only xmake is supported (there is no CMake build).

## Layout

All test source files live in `tests/` under the directories below.

| Directory | Content | Needs Device |
|---|---|---|
| `unit/core/` | core library unit tests for `kimix-core` types, math, utilities | No |
| `ut/` | vendored Boost.UT single header (`ut.hpp`) | — |

Include path setup in `tests/xmake.lua` exposes `tests/` so test sources just write `#include "ut/ut.hpp"`. Do **not** use `../../` relative paths.

## Adding a Test

xmake (`tests/xmake.lua`):

```lua
-- Signature: test_proj(name, source[, callable])
--   callable: optional config callback for extra deps/includes/defines
--   kind:     always "binary"
test_proj("test_kimix_core", "unit/core/test_kimix_core.cpp")

-- With extra config:
test_proj("test_advanced", "unit/core/test_advanced.cpp", function()
    add_deps("some-extra-dep")
end)
```

## C++ Test Templates & Style

> **Important rule:** Always keep test logic (assertions, setup, exercise, verify) in `main` function scope — never in file-scope `static auto` lambdas. File-scope static registrations can have unpredictable static initialization order and make it harder to control test filtering via CLI arguments.

### Template: No-Device Unit Test (main-scope pattern)

For simple CPU-only tests of the `kimix-core` library.

```cpp
// Test for <header>.h
// This test covers: <list of features>

#include "ut/ut.hpp"
#include <kimix_core.h>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "<scenario_name>"_test = [] {
        expect(true) << "description";
    };

    "<scenario_name2>"_test = [] {
        expect(condition) << "message on failure";
    };
}
```

### Simpler template (main scope, no separate test functions):

```cpp
#include "ut/ut.hpp"
#include <kimix_core.h>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "<feature>"_test = [] {
        // test body
        expect(condition);
    };
}
```

### Includes — canonical order

1. Test framework: `"ut/ut.hpp"`
2. Project headers: `<kimix_core.h>`
3. Standard library: `<cstdio>`, `<cmath>`, `<vector>`, etc.

### Using declarations

```cpp
using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;    // when using kimix:: functions
```

### Naming conventions

| Element | Convention | Example |
|---|---|---|
| Test source file | `test_<feature>.cpp` | `test_kimix_core.cpp` |
| Main scope test lambda | `"<snake_case_description>"_test` | `"add_basic"_test`, `"multiply_negative"_test` |
| Test executable | `test_<feature>` | `test_kimix_core` |

### Assertions

```cpp
expect(condition);
expect(condition) << "descriptive message on failure";
expect(ptr != nullptr);
expect(eq(a, b)) << "values should be equal";        // Boost.UT eq()
expect(neq(a, b));
expect(gt(a, b));
expect(lt(a, b));

// Float comparison — always use epsilon, never direct ==
expect(std::abs(result - expected) < 1e-4f);

// For complex validation — accumulate errors, expect once
bool all_correct = true;
for (size_t i = 0; i < n; i++) {
    if (results[i] != expected[i]) {
        printf("Mismatch at [%zu]: got %d expected %d\n", i, results[i], expected[i]);
        all_correct = false;
    }
}
expect(all_correct) << "all elements must match expected values";
```

### File header comment

Every test file starts with a descriptive comment block:

```cpp
// Test for <module/feature>.
// This test covers:
// - <feature 1>
// - <feature 2>
```

### Test organization within a file

- Each test function covers one logical area
- Test function bodies are self-contained: create their own objects, run, validate
- Prefer many small `"name"_test` lambdas over one giant test

## Build registration

**xmake** (`tests/xmake.lua`):

```lua
test_proj("test_kimix_core", "unit/core/test_kimix_core.cpp")
```

## Running

Before running any test binary, complete a full build:

```bash
xmake f -m debug -c -y       # configure
xmake build                  # build all targets (tests included when kimix_enable_tests=true)
xmake build test_kimix_core # build just the test target
xmake run test_kimix_core   # run the test
./bin/debug/test_kimix_core # or run directly
./bin/debug/test_kimix_core "add*"  # filter by name (Boost.UT CLI)
```

### Running Tests with Sanitizers

To detect memory errors, undefined behavior, or data races in tests, rebuild with sanitizer policies enabled:

```bash
# AddressSanitizer (use-after-free, buffer overflows)
xmake f -m debug --policies=build.sanitizer.address -c -y
xmake build
xmake run test_kimix_core

# Combined: AddressSanitizer + UndefinedBehaviorSanitizer
xmake f -m debug --policies=build.sanitizer.address,build.sanitizer.undefined -c -y
xmake build
xmake run test_kimix_core
```

> **Tip:** Sanitizers produce detailed stack traces on the first error. Use `-m debug` for debug symbols so traces show file/line info.

Available sanitizer policies:

| Policy | Detects |
|---|---|
| `build.sanitizer.address` | Use-after-free, heap/stack buffer overflows, memory leaks |
| `build.sanitizer.thread` | Data races, deadlocks |
| `build.sanitizer.memory` | Uninitialized memory reads |
| `build.sanitizer.leak` | Memory leaks (standalone) |
| `build.sanitizer.undefined` | Integer overflow, shift overflow, misaligned pointers |

Combine multiple: `--policies=build.sanitizer.address,build.sanitizer.undefined`

## Dependencies

Tests link `kimix-core`. The include path `tests/` is already exposed so `#include "ut/ut.hpp"` works.

## What Not to Do

- Do not put new test sources directly under `tests/`. Pick the right subfolder.
- Do not create ad-hoc top-level folders (e.g. `for_agent/`, `next/`, `tmp/`). The layout above is the entire test taxonomy.
- Do not reintroduce other test frameworks. The framework is Boost.UT only.
- Do not delete or `// skip` failing tests to make a build pass — fix the code under test instead.
