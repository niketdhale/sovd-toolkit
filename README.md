# sovd-toolkit

A modular SOVD (Service-Oriented Vehicle Diagnostics, ASAM / ISO 17978-3)
server and client stack for Linux, written in C++17 with a C-ABI adapter seam.

See `CLAUDE.md` for the full architecture, phase plan, and design rationale.

## Build & test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
./build/test_core                      # 245 assertions
./build/sovd_server 20002 domain       # port, role
cd build && ctest --output-on-failure
```
