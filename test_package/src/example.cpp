#include "nats.hpp"
#include <iostream>

int main() {
  try {
    natspp::client c; // default opts
    // Do not call connect() here to avoid relying on a running server.
    std::cout << "natspp linked OK\n";
    return 0;
  } catch (...) {
    return 1;
  }
}
