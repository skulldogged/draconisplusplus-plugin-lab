#include <Drac++/Core/Plugin.hpp>
#undef DRAC_PLUGIN
#define DRAC_PLUGIN(...)
#include <future>

#include "../container_info/container_info.cpp"

auto main(int argc, char** argv) -> int {
  if (argc != 2)
    return 1;
  const HttpEndpoint endpoint { .id         = "fixture",
                                .display    = "Fixture",
                                .urlBase    = argv[1],
                                .unixSocket = {},
                                .namedPipe  = {} };
  HttpSessions       sessions;
  auto               check = [&](StringView path) -> bool {
    const auto response = HttpGetCurl(endpoint, path, &sessions);
    return response && response->body == path;
  };
  if (!check("/first") || !check("/second"))
    return 2;
  if (HttpGetCurl(endpoint, "/error", &sessions))
    return 3;
  if (!check("/after-error") || !check("/close") || !check("/reconnected"))
    return 4;

  // Concurrent callers must never exchange response buffers or operate one CURL
  // handle simultaneously. Each response echoes its own path.
  Vec<std::future<bool>> workers;
  for (int worker = 0; worker < 4; ++worker)
    workers.push_back(std::async(std::launch::async, [&, worker] {
      for (int request = 0; request < 8; ++request)
        if (!check(std::format("/worker/{}/{}", worker, request)))
          return false;
      return true;
    }));
  for (auto& worker : workers)
    if (!worker.get())
      return 5;

  HttpEndpoint other = endpoint;
  other.urlBase += "/other";
  for (int request = 0; request < 2; ++request) {
    const auto response = HttpGetCurl(other, "/endpoint", &sessions);
    if (!response || response->body != "/other/endpoint")
      return 6;
  }
  sessions.clear();
  if (!check("/after-clear"))
    return 7;
}
