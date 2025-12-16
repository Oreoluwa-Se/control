#include "cap/scheduler.hpp"
#include "test/checks.hpp"

#include <iostream>
#include <vector>

int main(int argc, char **argv) {
  int threads = 4;
  if (argc >= 2)
    threads = std::max(1, std::atoi(argv[1]));

  test::run_all();

  return 0;
}
