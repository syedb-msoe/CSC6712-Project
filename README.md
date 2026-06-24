# CSC6712-Project
Must use Linux based system or WSL

## Build
Run the following line to build the project
cmake -S . -B build && cmake --build build

## Running Tests
Tests can be ran by calling the following after build from the root project directory
cd build && ctest --output-on-failure

## Running Benchmarks
There are two benchmark scripts that can be called, these can be called from the build folder
- bench_read_write
  - Does 3 sets of runs for 100, 1000, 10000, 100000, 1000000
  - prints the results
- bench_read_write
  - takes in additional arguments
    - <setup|read> <num_trials>
    - example call: `./bench_cache setup 5`
    - setup -> creates database for cold start
    - read -> reads data from databases from setup
    - num_trials -> number of seperate database connections
  - Call in setup mode, restart, and then call read mode to benchmark cold start reads
  - Performs 10 reads per trial and prints the results