# Agent instructions
Educational C++17 / Qt 6 desktop market monitor. Main UI is in Russian.
Purpose: display Russian TQBR equities from official API endpoints without CSV.
Structure: include/quant numerical library; src batch calculations; app Qt UI,
async providers and parsers; tests doctest and payload fixtures; benchmarks Google Benchmark;
docs math; .github/workflows CI and Pages.
Build: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=PATH_TO_QT
Then: cmake --build build --config Release --parallel 2
Tests: ctest --test-dir build -C Release --output-on-failure
Linux benchmark: ./build/quant_benchmark --benchmark_min_time=0.1s
Windows benchmark: .\build\Release\quant_benchmark.exe --benchmark_min_time=0.1s
Docs: doxygen Doxyfile
Use four-space indentation, C++17 and clear snake_case names. Document public APIs.
Before completion build, run all tests, and run the deterministic GUI smoke test.
Do not change CI/CD or pinned dependencies without a user request.
Network must be asynchronous. Never add blocking requests or sleeps to the UI thread.
Bound history and memory. Never annualize irregular tick returns by sqrt(252).
Label simulated and delayed feeds prominently; do not relabel them as real-time.
Do not save tokens or user quotes, print raw auth replies or disable TLS verification.
Do not add order-placement code. Never require a broker token for CI tests.
Test market data using nonsecret fixtures and demo mode. Live credentials stay local.
