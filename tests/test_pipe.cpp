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
  // Exercise every possible read boundary, including misleading terminal-chunk
  // bytes inside payload data, long extensions, and trailers.
  const String       splitBody = "7;extension=yes\r\n0\r\n\r\n[]\r\n0\r\nX-Trailer: value\r\n\r\n";
  ChunkedHttpFraming splitFraming;
  for (usize length = 0; length <= splitBody.size(); ++length) {
    const auto result = splitFraming.update(StringView(splitBody).substr(0, length));
    if (!result || *result != (length == splitBody.size()))
      return 4;
  }
  for (const String body : { "z\r\n", "1\r\nxXX", "0\r\ninvalid trailer\r\n\r\n", "0\r\n\r\njunk" }) {
    ChunkedHttpFraming framing;
    if (framing.update(body))
      return 5;
  }
  const String       largeBody = "600000;" + String(65536, 'e') + "\r\n" + String(6 * 1024 * 1024, 'x') + "\r\n0\r\n\r\n";
  ChunkedHttpFraming largeFraming;
  for (usize length = 0; length < largeBody.size(); length += 127) {
    const auto result = largeFraming.update(StringView(largeBody).substr(0, length));
    if (!result || *result)
      return 6;
  }
  const auto framed  = largeFraming.update(largeBody);
  const auto decoded = DecodeChunkedHttpBody(largeBody);
  if (!framed || !*framed || !decoded || decoded->size() != 6 * 1024 * 1024)
    return 7;
  auto run = [&](bool silent, bool chunked = false) {
    const String name   = std::format(R"(\\.\pipe\drac-review-{}-{}-{})", GetCurrentProcessId(), silent, chunked);
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
        const String response = chunked
          ? "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + largeBody
          : "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n[]";
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
    std::cout << "silent=" << silent << " chunked=" << chunked << " success=" << result.has_value() << " elapsed=" << elapsed << '\n';
    return silent ? (!result && result.error().code == Timeout && elapsed < 4.0)
                  : (result && result->body == (chunked ? String(6 * 1024 * 1024, 'x') : String("[]")) && elapsed < 3.0);
  };
  return run(false) && run(false, true) && run(true) ? 0 : 1;
}
