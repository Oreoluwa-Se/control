#include "test/checks.hpp"
#include <iostream>

int main() {
  try {
    return test::run_all() ? 0 : 1;
  } catch (const std::exception &e) {
    std::cerr << "[tests] exception: " << e.what() << "\n";
    return 2;
  } catch (...) {
    std::cerr << "[tests] unknown exception\n";
    return 3;
  }
}
