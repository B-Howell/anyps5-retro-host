#ifndef RUNNER_PS3_PACKAGE_BATCH_H_
#define RUNNER_PS3_PACKAGE_BATCH_H_

#include <cstddef>
#include <string>
#include <vector>

template <typename Load, typename Unload, typename Cancelled, typename Progress>
bool InstallPs3PackageBatch(const std::vector<std::string>& packages, Load load,
                            Unload unload, Cancelled cancelled, Progress progress) {
  std::size_t completed = 0;
  for (const auto& package : packages) {
    if (cancelled() || !load(package)) return false;
    unload();
    progress(++completed);
  }
  return !packages.empty() && !cancelled();
}

#endif  // RUNNER_PS3_PACKAGE_BATCH_H_
