#include <iostream>

#include "cratchet/version.hpp"

int main() {
  std::cout << "cratchet " << cratchet::version() << '\n';
  return 0;
}
