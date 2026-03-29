#include "session.h"
#include <random>

void Session::init() {
    std::mt19937 rng(std::random_device{}());
    id       = rng();
    start_us = get_current_time_us();
}
