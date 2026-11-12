#include "ps3_package_batch.h"

#include <cassert>

int main() {
  const std::vector<std::string> packages{"01.19.pkg", "01.20.pkg", "01.21.pkg"};
  std::vector<std::string> opened;
  size_t closed = 0;
  size_t completed = 0;
  bool stop = false;
  bool fail = false;
  const auto load = [&](const std::string& package) {
    opened.push_back(package);
    return !fail || opened.size() != 2;
  };
  const auto unload = [&] { ++closed; };
  const auto cancelled = [&] { return stop; };
  const auto progress = [&](size_t count) { completed = count; };

  assert(InstallPs3PackageBatch(packages, load, unload, cancelled, progress));
  assert(opened == packages && closed == 3 && completed == 3);

  opened.clear();
  closed = completed = 0;
  fail = true;
  assert(!InstallPs3PackageBatch(packages, load, unload, cancelled, progress));
  assert(opened.size() == 2 && opened.back() == packages[1]);
  assert(closed == 1 && completed == 1);

  opened.clear();
  closed = completed = 0;
  assert(!InstallPs3PackageBatch(packages, load, unload, cancelled, [&](size_t count) {
    progress(count);
    stop = true;
  }));
  assert(opened.size() == 1 && closed == 1 && completed == 1);
}
