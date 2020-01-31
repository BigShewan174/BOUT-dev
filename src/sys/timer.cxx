#include "bout/sys/timer.hxx"

Timer::Timer() : timing(getInfo("")) {
  if (timing.counter == 0) {
    timing.started = clock_type::now();
    timing.running = true;
  }
  timing.counter += 1;
}

Timer::Timer(const std::string& label) : timing(getInfo(label)) {
  if (timing.counter == 0) {
    timing.started = clock_type::now();
    timing.running = true;
  }
  timing.counter += 1;
}

Timer::~Timer() {
  timing.counter -= 1;
  if (timing.counter == 0) {
    auto finished = clock_type::now();
    timing.running = false;
    timing.time += finished - timing.started;
  }
}

void Timer::cleanup() { info.clear(); }

std::map<std::string, Timer::timer_info> Timer::info;

Timer::timer_info& Timer::getInfo(const std::string& label) {
  auto it = info.find(label);
  if (it == info.end()) {
    auto timer = info.emplace(
      label, timer_info{seconds{0}, false, clock_type::now(), 0});
    return timer.first->second;
  }
  return it->second;
}

double Timer::getTime(const Timer::timer_info& info) {
  if (info.running) {
    return seconds{info.time + (clock_type::now() - info.started)}.count();
  }
  return seconds{info.time}.count();
}

double Timer::resetTime(Timer::timer_info& info) {
  auto val = info.time;
  info.time = clock_type::duration{0};
  if (info.running) {
    auto cur_time = clock_type::now();
    val += cur_time - info.started;
    info.started = cur_time;
  }
  return seconds{val}.count();
}
