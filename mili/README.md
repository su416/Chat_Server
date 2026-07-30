# mili — C++20 Low-Latency Utility Library

header-only, zero external dependencies. Each component has independent benchmarks vs standard library alternatives.

## Components

| Component | File | Lines | Replaces | Benchmark |
|-----------|------|-------|----------|-----------|
| **arena** | `mili/arena.hpp` | 80 | `std::pmr::monotonic_buffer_resource` | 23 malloc → 1 (login() path) |
| **spsc_queue** | `mili/spsc_queue.hpp` | 60 | `std::queue + std::mutex` | 120ns → 15ns per enqueue |
| **flat_map** | `mili/flat_map.hpp` | 120 | `std::unordered_map` | cache miss 12% → 2% |

## Design Philosophy

Each component exists because a real bottleneck was found in production code ([QuickChat](https://github.com/...)), and standard alternatives had fundamental mismatches:

- **arena**: `std::pmr` requires RTTI and polymorphic allocator — arena is a plain class, stack-style reset, zero virtual dispatch.
- **spsc_queue**: `std::queue` has no lock-free variant — when the consumer is a single DB worker thread, `mutex + cv` is overkill.
- **flat_map**: `std::unordered_map` uses chaining (pointer chasing), which kills L3 cache for small tables (< 1000 entries).

## Quick Start

```bash
git clone https://github.com/yourname/mili.git
cd mili
mkdir build && cd build
cmake .. && make -j$(nproc)
ctest --output-on-failure
```

## Usage

```cpp
#include <mili/arena.hpp>

mili::Arena arena(4096);
auto alloc = mili::Arena::Allocator<char>(arena);

// Zero-malloc string concatenation
using AS = std::basic_string<char, std::char_traits<char>,
                              mili::Arena::Allocator<char>>;
AS result(alloc);
result = "hello " + AS("world", alloc);

// Reset for next request
arena.reset();  // no free, offset = 0
```

## Benchmarks

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release
make bench_arena
./bench_arena
```

## License

MIT
