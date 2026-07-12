# CSC6712-Project

## Setup
Must use Linux based system or WSL.
CMake is required, it can be installed by the following command:
`sudo apt install cmake`

You will also need a compiler installed:
`sudo apt install g++`

## Build
Run the following line to build the project
`cmake -S . -B build && cmake --build build`

## Running Tests
Tests can be ran by calling the following after build from the root project directory
`cd build && ctest --output-on-failure`

test database files are stored in your home directory under the `test_data` folder

## Running Benchmarks
There are two benchmark scripts that can be called, these can be called from the build folder
- bench_read_write
  - Does 3 sets of runs for 100, 1000, 10000, 100000, 1000000
  - prints the results
    - alternativley you can pass in a filename as an argument and it will log the results
    - example: `./bench_read_write > results_rw.csv`
    - file will be generated in the build folder
- bench_read_write
  - takes in additional arguments
    - <setup|read> <num_trials>
    - example call: `./bench_cache setup 5`
    - setup -> creates database for cold start
    - read -> reads data from databases from setup
    - num_trials -> number of seperate database connections
  - Call in setup mode, restart, and then call read mode to benchmark cold start reads
  - Performs 10 reads per trial and prints the results

benchmark database files are stored in your home directory under the `bench_data` folder

## Running TCP Server/Client
Protocol documentation can be found here: [docs/PROTOCOL.md](docs/PROTOCOL.md)

- You can start a server by running `./db_server` in the `build` folder
  - Takes in a variety of arguments
     - `-h` or `--help` will list all arguments
     - `-p <number>` or `--port <number>` will allow you to set the port (defaults to 5555)
     - `-d <path>` or `--db <path>` will allow you to set the database file path (defaults to default.db)
     - `-v` or `--versbose` will enable verbose logging
- Clients can be spun up by running `./db_client` in the `build` folder
  - Takes in a variety of arguments
     - `-h` or `--help` will list all arguments
     - `-H` or `--host <ip>` will allow you to set the host ip (defaults to localhost)
     - `-p <number>` or `--port <number>` will allow you to set the port (defaults to 5555)
     - `-d` or `--debug` will return the raw requests/responses for debugging purposes