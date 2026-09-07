#include <Drac++/Core/Plugin.hpp>
#undef DRAC_PLUGIN
#define DRAC_PLUGIN(...)
#include <future>
#include <iostream>

#include "../container_info/container_info.cpp"

auto main() -> int {
  if (DecodeChunkedHttpBody("1\r\nx\r\n0\r\n"))
    return 2;
  auto complete = DecodeChunkedHttpBody("1\r\nx\r\n0\r\n\r\n");
  if (!complete || *complete != "x")
    return 3;
  auto run = [](bool silent) {
    const String name   = std::format(R"(\\.\pipe\drac-review-{}-{})", GetCurrentProcessId(), silent);
    const auto   wide   = WideFromUtf8(name);
    HANDLE       server = CreateNamedPipeW(wide.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
    if (server == INVALID_HANDLE_VALUE)
      return false;
    std::promise<void> finished;
    auto               done   = finished.get_future();
    auto               worker = std::async(std::launch::async, [&] {
      HANDLE     event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      OVERLAPPED connect {};
      connect.hEvent = event;
      if (!ConnectNamedPipe(server, &connect) && GetLastError() == ERROR_IO_PENDING)
        WaitForSingleObject(event, 4000);
      if (!silent) {
        const String response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n[]";
        OVERLAPPED   write {};
        write.hEvent = event;
        ResetEvent(event);
        DWORD written = 0;
        if (!WriteFile(server, response.data(), response.size(), &written, &write) && GetLastError() == ERROR_IO_PENDING)
          GetOverlappedResult(server, &write, &written, TRUE);
      }
      done.wait_for(std::chrono::seconds(5));
      CancelIoEx(server, nullptr);
      DisconnectNamedPipe(server);
      CloseHandle(event);
      CloseHandle(server);
    });
    HttpEndpoint       endpoint { .id = "fixture", .display = "fixture", .urlBase = "http://d", .unixSocket = {}, .namedPipe = name, .configured = true };
    const auto         start   = std::chrono::steady_clock::now();
    const auto         result  = HttpGetNamedPipe(endpoint, "/fixture");
    const auto         elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    finished.set_value();
    worker.get();
    std::cout << "silent=" << silent << " success=" << result.has_value() << " elapsed=" << elapsed << '\n';
    return silent ? (!result && result.error().code == Timeout && elapsed < 4.0) : (result && result->body == "[]" && elapsed < 1.0);
  };
  return run(false) && run(true) ? 0 : 1;
}
