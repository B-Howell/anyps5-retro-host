// Boots a real game through anyps5-retro-host exactly as the launcher does,
// waits until the launcher side has drawn visible frames, and writes the last
// frame to a PPM file.
#include "retro_host_proxy.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>

using namespace anyps5::retro_ipc;
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) try {
  if (argc != 7) throw std::runtime_error("Expected host, core, disc, system, saves and an output PPM path.");
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<std::function<void()>> tasks;
  std::atomic<int> frames{0};
  std::atomic<int64_t> next_texture{0};
  ProxyOptions options;
  options.host = std::filesystem::absolute(std::filesystem::u8path(argv[1]));
#if defined(_WIN32)
  options.texture_per_session = true;
#else
  options.texture_per_session = false;
#endif
  RetroHostProxy proxy(options, {[&] { return ++next_texture; }, [](int64_t) {}, [&](int64_t) { ++frames; },
                                 [&](std::function<void()> task) {
                                   std::lock_guard<std::mutex> lock(mutex);
                                   tasks.push_back(std::move(task));
                                   wake.notify_all();
                                 }});
  std::optional<int64_t> owner;
  const auto call = [&](const std::string& method, Value arguments) {
    if (owner) arguments = Value(Map{{Value("owner"), Value(*owner)}, {Value("value"), std::move(arguments)}});
    std::optional<ProxyResult> reply;
    proxy.Handle(method, std::move(arguments), [&](ProxyResult result) { reply = std::move(result); });
    while (!reply) {
      std::unique_lock<std::mutex> lock(mutex);
      wake.wait(lock, [&] { return !tasks.empty(); });
      auto task = std::move(tasks.front());
      tasks.pop_front();
      lock.unlock();
      task();
    }
    if (reply->kind != ProxyResult::Kind::kSuccess) throw std::runtime_error(method + ": " + reply->message);
    return reply->value;
  };
  const auto absolute = [](const char* path) { return std::filesystem::absolute(std::filesystem::u8path(path)).u8string(); };
  const auto started = call("start", Value(Map{{Value("core"), Value(absolute(argv[2]))},
                                               {Value("disc"), Value(absolute(argv[3]))},
                                               {Value("system"), Value(absolute(argv[4]))},
                                               {Value("saves"), Value(absolute(argv[5]))}}));
  int64_t texture;
  if (const auto* handle = std::get_if<Map>(&started)) {
    owner = handle->at(Value("session")).LongValue();
    texture = handle->at(Value("texture")).LongValue();
  } else {
    texture = started.LongValue();
  }
  std::cout << "host pid " << std::get<int64_t>(call("hostProcessId", Value())) << ", texture " << texture << std::endl;
  const auto begin = Clock::now();
  Map status;
  bool visible = false;
  while (Clock::now() - begin < std::chrono::seconds(90)) {
    proxy.Acquire(texture);
    status = std::get<Map>(call("status", Value()));
    if (std::get<bool>(status.at(Value("ended"))))
      throw std::runtime_error("The game ended: " + std::get<std::string>(status.at(Value("error"))));
    const auto shown = status.find(Value("startupVisible"));
    visible = shown != status.end() ? std::get<bool>(shown->second) : std::get<bool>(status.at(Value("presented")));
    if (visible && Clock::now() - begin > std::chrono::seconds(20)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(125));
  }
  if (!visible) throw std::runtime_error("No visible frame was presented.");
  const auto frame = proxy.Acquire(texture);
  if (!frame) throw std::runtime_error("No frame in shared memory.");
  std::ofstream out(std::filesystem::u8path(argv[6]), std::ios::binary);
  out << "P6\n" << frame->width << ' ' << frame->height << "\n255\n";
  for (size_t i = 0; i < frame->width * frame->height; ++i) out.write(reinterpret_cast<const char*>(frame->pixels + i * 4), 3);
  std::cout << frames << " frames announced; last frame " << frame->width << 'x' << frame->height << ", hardware "
            << std::get<bool>(status.at(Value("hardware"))) << ", " << std::chrono::duration_cast<std::chrono::seconds>(
                   Clock::now() - begin).count() << " s" << std::endl;
  call("stop", Value());
  for (int i = 0; i < 200; ++i) {
    if (std::get<bool>(std::get<Map>(call("status", Value())).at(Value("ended")))) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  call("release", Value());
  proxy.Shutdown();
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << std::endl;
  return 1;
}
