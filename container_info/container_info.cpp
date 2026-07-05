/**
 * @file container_info.cpp
 * @brief Local container runtime status provider for Draconis++.
 *
 * @details The provider talks directly to supported local APIs. It never uses
 * command-line runtime clients for discovery or collection.
 */

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <curl/curl.h>
#include <format>
#include <glaze/glaze.hpp>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/grpcpp.h>

#include <Drac++/Core/Plugin.hpp>

#include <Drac++/Utils/Error.hpp>
#include <Drac++/Utils/Types.hpp>

using namespace draconis::core::plugin;
using namespace draconis::utils::error;
using namespace draconis::utils::types;
using enum DracErrorCode;

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <filesystem>
#endif

namespace container_info::dto {
  struct DockerContainer {
    String State;
    String Status;
  };

  struct DockerVersion {
    String Version;
    String ApiVersion;
  };

  struct LxdInstance {
    String type = "container";
    String status;
  };

  struct LxdInstancesResponse {
    Vec<LxdInstance> metadata;
  };

  struct LxdInfoMetadata {
    String api_version;
  };

  struct LxdInfoResponse {
    LxdInfoMetadata metadata;
  };
} // namespace container_info::dto

namespace {
  namespace dto = container_info::dto;

  constexpr long CONNECT_TIMEOUT_MS = 350;
  constexpr long TOTAL_TIMEOUT_MS   = 900;

  enum class RuntimeKind {
    Docker,
    Podman,
    Containerd,
    Cri,
    Lxd,
  };

  struct RuntimeInfo {
    String      id;
    String      displayName;
    String      kind;
    bool        available = false;
    bool        active    = false;
    u64         running   = 0;
    u64         total     = 0;
    String      version;
    String      endpoint;
    Option<String> error = None;
  };

  struct ContainerInfoData {
    bool             active          = false;
    u64              totalRunning    = 0;
    u64              totalContainers = 0;
    Vec<RuntimeInfo> runtimes;
  };

  auto HasPrefix(StringView value, StringView prefix) -> bool {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
  }

  auto ToLower(StringView value) -> String {
    String result(value);
    std::ranges::transform(result, result.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    return result;
  }

  auto ParseDockerContainers(StringView body) -> Result<Pair<u64, u64>> {
    const String buffer(body);
    Vec<dto::DockerContainer> containers;
    if (auto errc = glz::read<glz::opts { .error_on_unknown_keys = false }>(containers, buffer); errc.ec != glz::error_code::none)
      ERR_FMT(ParseError, "Failed to parse Docker-compatible containers response: {}", glz::format_error(errc, buffer));

    u64 running = 0;
    for (const dto::DockerContainer& container : containers) {
      const String state  = ToLower(container.State);
      const String status = ToLower(container.Status);
      if (state == "running" || HasPrefix(status, "up "))
        ++running;
    }

    return Pair<u64, u64> { running, static_cast<u64>(containers.size()) };
  }

  auto ParseDockerVersion(StringView body) -> String {
    const String buffer(body);
    dto::DockerVersion version;
    if (auto errc = glz::read<glz::opts { .error_on_unknown_keys = false }>(version, buffer); errc.ec != glz::error_code::none)
      return {};
    return !version.Version.empty() ? version.Version : version.ApiVersion;
  }

  auto ParseLxdInstances(StringView body) -> Result<Pair<u64, u64>> {
    const String buffer(body);
    dto::LxdInstancesResponse response;
    if (auto errc = glz::read<glz::opts { .error_on_unknown_keys = false }>(response, buffer); errc.ec != glz::error_code::none)
      ERR_FMT(ParseError, "Failed to parse LXD instances response: {}", glz::format_error(errc, buffer));

    u64 running = 0;
    u64 total   = 0;
    for (const dto::LxdInstance& instance : response.metadata) {
      const String type = ToLower(instance.type);
      if (!type.empty() && type != "container")
        continue;
      ++total;
      if (ToLower(instance.status) == "running")
        ++running;
    }

    return Pair<u64, u64> { running, total };
  }

  auto ParseLxdVersion(StringView body) -> String {
    const String buffer(body);
    dto::LxdInfoResponse response;
    if (auto errc = glz::read<glz::opts { .error_on_unknown_keys = false }>(response, buffer); errc.ec != glz::error_code::none)
      return {};
    return response.metadata.api_version;
  }

  struct HttpResponse {
    long   status = 0;
    String body;
  };

  struct HttpEndpoint {
    String id;
    String display;
    String urlBase;
    String unixSocket;
    String namedPipe;
  };

  class CurlGlobal {
   public:
    CurlGlobal() {
      curl_global_init(CURL_GLOBAL_DEFAULT);
    }
    ~CurlGlobal() {
      curl_global_cleanup();
    }
  };

  auto CurlWriteCallback(char* ptr, usize size, usize nmemb, void* userdata) -> usize {
    auto* out = static_cast<String*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
  }

  auto HttpGetCurl(const HttpEndpoint& endpoint, StringView path) -> Result<HttpResponse> {
    CURL* curl = curl_easy_init();
    if (curl == nullptr)
      ERR(ApiUnavailable, "curl_easy_init() failed");

    String body;
    const String url = endpoint.urlBase + String(path);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, CONNECT_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, TOTAL_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "draconisplusplus-container-info/1");
    if (!endpoint.unixSocket.empty())
      curl_easy_setopt(curl, CURLOPT_UNIX_SOCKET_PATH, endpoint.unixSocket.c_str());

    const CURLcode code = curl_easy_perform(curl);
    long           http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK)
      ERR_FMT(ApiUnavailable, "{}: {}", endpoint.display, curl_easy_strerror(code));
    if (http >= 400)
      ERR_FMT(ApiUnavailable, "{} returned HTTP {}", endpoint.display, http);
    return HttpResponse { .status = http, .body = std::move(body) };
  }

#ifdef _WIN32
  auto WideFromUtf8(StringView value) -> std::wstring {
    if (value.empty())
      return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring out(static_cast<usize>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), needed);
    return out;
  }

  auto HttpGetNamedPipe(const HttpEndpoint& endpoint, StringView path) -> Result<HttpResponse> {
    const std::wstring pipe = WideFromUtf8(endpoint.namedPipe);
    HANDLE handle = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
      ERR_FMT(ApiUnavailable, "{} pipe unavailable: {}", endpoint.display, static_cast<unsigned long>(GetLastError()));

    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(handle, &mode, nullptr, nullptr);

    const String request = std::format("GET {} HTTP/1.1\r\nHost: localhost\r\nUser-Agent: draconisplusplus-container-info/1\r\nConnection: close\r\n\r\n", path);
    DWORD written = 0;
    if (!WriteFile(handle, request.data(), static_cast<DWORD>(request.size()), &written, nullptr)) {
      const DWORD err = GetLastError();
      CloseHandle(handle);
      ERR_FMT(IoError, "{} pipe write failed: {}", endpoint.display, static_cast<unsigned long>(err));
    }

    String raw;
    std::array<char, 4096> buffer {};
    DWORD read = 0;
    while (ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read > 0)
      raw.append(buffer.data(), read);
    CloseHandle(handle);

    const usize headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == String::npos)
      ERR(ParseError, "Named-pipe HTTP response did not contain headers");

    const StringView statusLine = StringView(raw).substr(0, raw.find("\r\n"));
    long status = 0;
    if (statusLine.size() >= 12)
      std::from_chars(statusLine.data() + 9, statusLine.data() + 12, status);

    if (status >= 400)
      ERR_FMT(ApiUnavailable, "{} returned HTTP {}", endpoint.display, status);

    return HttpResponse {
      .status = status,
      .body   = raw.substr(headerEnd + 4),
    };
  }
#endif

  auto HttpGet(const HttpEndpoint& endpoint, StringView path) -> Result<HttpResponse> {
#ifdef _WIN32
    if (!endpoint.namedPipe.empty())
      return HttpGetNamedPipe(endpoint, path);
#endif
    return HttpGetCurl(endpoint, path);
  }

  auto ExistingSocket(String path) -> Option<String> {
#ifdef _WIN32
    (void)path;
    return None;
#else
    std::error_code ec;
    if (std::filesystem::exists(path, ec))
      return path;
    return None;
#endif
  }

  auto DockerEndpoints() -> Vec<HttpEndpoint> {
    Vec<HttpEndpoint> endpoints;
#ifdef _WIN32
    endpoints.push_back({ .id = "docker-npipe", .display = "Docker Engine", .urlBase = "http://localhost", .namedPipe = R"(\\.\pipe\docker_engine)" });
#else
    constexpr std::array<StringView, 7> paths {
      "/var/run/docker.sock",
      "/run/docker.sock",
      "/run/user/1000/docker.sock",
      "/var/run/docker-desktop/docker.sock",
      "/run/docker-desktop/docker.sock",
      "/var/run/colima/docker.sock",
      "/var/run/lima/docker.sock",
    };
    for (StringView path : paths)
      if (ExistingSocket(String(path)))
        endpoints.push_back({ .id = std::format("docker:{}", path), .display = "Docker Engine", .urlBase = "http://localhost", .unixSocket = String(path) });

    if (const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR"); runtimeDir != nullptr) {
      const String path = String(runtimeDir) + "/docker.sock";
      if (ExistingSocket(path))
        endpoints.push_back({ .id = "docker:xdg", .display = "Docker Engine", .urlBase = "http://localhost", .unixSocket = path });
    }
#endif
    return endpoints;
  }

  auto PodmanEndpoints() -> Vec<HttpEndpoint> {
    Vec<HttpEndpoint> endpoints;
#ifdef _WIN32
    return endpoints;
#else
    constexpr std::array<StringView, 3> paths {
      "/run/podman/podman.sock",
      "/var/run/podman/podman.sock",
      "/var/run/docker.sock",
    };
    for (StringView path : paths)
      if (ExistingSocket(String(path)))
        endpoints.push_back({ .id = std::format("podman:{}", path), .display = "Podman", .urlBase = "http://d", .unixSocket = String(path) });

    if (const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR"); runtimeDir != nullptr) {
      const String path = String(runtimeDir) + "/podman/podman.sock";
      if (ExistingSocket(path))
        endpoints.push_back({ .id = "podman:xdg", .display = "Podman", .urlBase = "http://d", .unixSocket = path });
    }
    return endpoints;
#endif
  }

  auto LxdEndpoints() -> Vec<HttpEndpoint> {
    Vec<HttpEndpoint> endpoints;
#ifdef _WIN32
    return endpoints;
#else
    constexpr std::array<StringView, 3> paths {
      "/var/snap/lxd/common/lxd/unix.socket",
      "/run/lxd/unix.socket",
      "/var/lib/lxd/unix.socket",
    };
    for (StringView path : paths)
      if (ExistingSocket(String(path)))
        endpoints.push_back({ .id = std::format("lxd:{}", path), .display = "LXD", .urlBase = "http://lxd", .unixSocket = String(path) });
    return endpoints;
#endif
  }

  auto CollectDockerLike(RuntimeKind kind, StringView id, StringView display, Vec<HttpEndpoint> endpoints) -> RuntimeInfo {
    RuntimeInfo runtime {
      .id          = String(id),
      .displayName = String(display),
      .kind        = kind == RuntimeKind::Docker ? "docker" : "podman",
    };

    if (endpoints.empty()) {
      runtime.error = "No local API socket or named pipe found";
      return runtime;
    }

    Vec<String> failures;
    for (const HttpEndpoint& endpoint : endpoints) {
      runtime.endpoint = !endpoint.unixSocket.empty() ? endpoint.unixSocket : endpoint.namedPipe;

      Result<HttpResponse> ping = HttpGet(endpoint, "/_ping");
      if (!ping) {
        failures.push_back(ping.error().message);
        continue;
      }

      Result<HttpResponse> versionResponse = HttpGet(endpoint, kind == RuntimeKind::Podman ? "/libpod/version" : "/version");
      if (!versionResponse && kind == RuntimeKind::Podman)
        versionResponse = HttpGet(endpoint, "/version");

      if (versionResponse)
        runtime.version = ParseDockerVersion(versionResponse->body);

      Result<HttpResponse> containersResponse = HttpGet(endpoint, "/containers/json?all=true");
      if (!containersResponse) {
        failures.push_back(containersResponse.error().message);
        continue;
      }

      Result<Pair<u64, u64>> counts = ParseDockerContainers(containersResponse->body);
      if (!counts) {
        failures.push_back(counts.error().message);
        continue;
      }

      const auto [running, total] = *counts;
      runtime.available = true;
      runtime.running   = running;
      runtime.total     = total;
      runtime.active    = running > 0;
      runtime.error     = None;
      return runtime;
    }

    runtime.error = failures.empty() ? Option<String>("No usable endpoint found") : Option<String>(failures.front());
    return runtime;
  }

  auto CollectLxd() -> RuntimeInfo {
    RuntimeInfo runtime {
      .id          = "lxd",
      .displayName = "LXD",
      .kind        = "lxd",
    };

    const Vec<HttpEndpoint> endpoints = LxdEndpoints();
    if (endpoints.empty()) {
      runtime.error = "No local LXD socket found";
      return runtime;
    }

    Vec<String> failures;
    for (const HttpEndpoint& endpoint : endpoints) {
      runtime.endpoint = endpoint.unixSocket;

      if (Result<HttpResponse> versionResponse = HttpGet(endpoint, "/1.0"); versionResponse)
        runtime.version = ParseLxdVersion(versionResponse->body);

      Result<HttpResponse> instancesResponse = HttpGet(endpoint, "/1.0/instances?recursion=1");
      if (!instancesResponse) {
        failures.push_back(instancesResponse.error().message);
        continue;
      }

      Result<Pair<u64, u64>> counts = ParseLxdInstances(instancesResponse->body);
      if (!counts) {
        failures.push_back(counts.error().message);
        continue;
      }

      const auto [running, total] = *counts;
      runtime.available = true;
      runtime.running   = running;
      runtime.total     = total;
      runtime.active    = running > 0;
      runtime.error     = None;
      return runtime;
    }

    runtime.error = failures.empty() ? Option<String>("No usable LXD endpoint found") : Option<String>(failures.front());
    return runtime;
  }

  struct ProtoField {
    u64    number = 0;
    u64    wire   = 0;
    u64    varint = 0;
    String bytes;
  };

  auto ReadVarint(StringView input, usize& pos, u64& value) -> bool {
    value = 0;
    u32 shift = 0;
    while (pos < input.size() && shift < 64) {
      const u8 byte = static_cast<u8>(input[pos++]);
      value |= static_cast<u64>(byte & 0x7F) << shift;
      if ((byte & 0x80) == 0)
        return true;
      shift += 7;
    }
    return false;
  }

  auto ProtoFields(StringView input) -> Vec<ProtoField> {
    Vec<ProtoField> fields;
    usize pos = 0;
    while (pos < input.size()) {
      u64 key = 0;
      if (!ReadVarint(input, pos, key))
        break;
      ProtoField field {
        .number = key >> 3,
        .wire   = key & 0x7,
      };
      if (field.wire == 0) {
        if (!ReadVarint(input, pos, field.varint))
          break;
      } else if (field.wire == 2) {
        u64 len = 0;
        if (!ReadVarint(input, pos, len) || pos + len > input.size())
          break;
        field.bytes = String(input.substr(pos, static_cast<usize>(len)));
        pos += static_cast<usize>(len);
      } else if (field.wire == 5) {
        pos += 4;
      } else if (field.wire == 1) {
        pos += 8;
      } else {
        break;
      }
      fields.push_back(std::move(field));
    }
    return fields;
  }

  auto ProtoRepeatedMessages(StringView message, u64 fieldNumber) -> Vec<String> {
    Vec<String> out;
    for (const ProtoField& field : ProtoFields(message))
      if (field.number == fieldNumber && field.wire == 2)
        out.push_back(field.bytes);
    return out;
  }

  auto ProtoString(StringView message, u64 fieldNumber) -> String {
    for (const ProtoField& field : ProtoFields(message))
      if (field.number == fieldNumber && field.wire == 2)
        return field.bytes;
    return {};
  }

  auto ProtoVarint(StringView message, u64 fieldNumber) -> Option<u64> {
    for (const ProtoField& field : ProtoFields(message))
      if (field.number == fieldNumber && field.wire == 0)
        return field.varint;
    return None;
  }

  auto GrpcAddressFromEndpoint(StringView endpoint) -> String {
    if (HasPrefix(endpoint, "npipe:"))
      return String(endpoint);
    if (HasPrefix(endpoint, "unix:"))
      return String(endpoint);
    return "unix:" + String(endpoint);
  }

  auto ByteBufferToString(grpc::ByteBuffer* buffer) -> Result<String> {
    std::vector<grpc::Slice> slices;
    const grpc::Status status = buffer->Dump(&slices);
    if (!status.ok())
      ERR_FMT(ApiUnavailable, "Failed to read gRPC response: {}", status.error_message());

    String out;
    for (const grpc::Slice& slice : slices)
      out.append(reinterpret_cast<const char*>(slice.begin()), slice.size());
    return out;
  }

  auto GrpcUnary(StringView endpoint, StringView method, StringView request, const Map<String, String>& metadata = {}) -> Result<String> {
    grpc::ChannelArguments args;
    args.SetInt(GRPC_ARG_ENABLE_RETRIES, 0);
    auto channel = grpc::CreateCustomChannel(GrpcAddressFromEndpoint(endpoint), grpc::InsecureChannelCredentials(), args);
    grpc::GenericStub stub(channel);

    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(TOTAL_TIMEOUT_MS));
    for (const auto& [key, value] : metadata)
      context.AddMetadata(key, value);

    grpc::Slice slice(request.data(), request.size());
    grpc::ByteBuffer requestBuffer(&slice, 1);
    grpc::ByteBuffer responseBuffer;
    grpc::Status     status;
    grpc::CompletionQueue cq;
    const String grpcMethod = String(method);
    auto rpc = stub.PrepareUnaryCall(&context, grpcMethod, requestBuffer, &cq);
    if (!rpc)
      ERR_FMT(ApiUnavailable, "Failed to prepare gRPC call {}", method);

    rpc->StartCall();
    rpc->Finish(&responseBuffer, &status, reinterpret_cast<void*>(1));

    void* tag = nullptr;
    bool  ok  = false;
    if (!cq.Next(&tag, &ok) || !ok || tag != reinterpret_cast<void*>(1))
      ERR_FMT(ApiUnavailable, "gRPC call {} did not complete", method);
    cq.Shutdown();
    if (!status.ok())
      ERR_FMT(ApiUnavailable, "{}: {}", method, status.error_message());

    return ByteBufferToString(&responseBuffer);
  }

  auto ContainerdEndpoints() -> Vec<String> {
    Vec<String> endpoints;
#ifdef _WIN32
    endpoints.push_back("npipe:////./pipe/containerd-containerd");
#else
    constexpr std::array<StringView, 3> paths {
      "/run/containerd/containerd.sock",
      "/var/run/containerd/containerd.sock",
      "/run/k3s/containerd/containerd.sock",
    };
    for (StringView path : paths)
      if (ExistingSocket(String(path)))
        endpoints.push_back(String(path));
#endif
    return endpoints;
  }

  auto CriEndpoints() -> Vec<String> {
    Vec<String> endpoints;
#ifdef _WIN32
    endpoints.push_back("npipe:////./pipe/containerd-containerd");
#else
    constexpr std::array<StringView, 5> paths {
      "/run/crio/crio.sock",
      "/var/run/crio/crio.sock",
      "/run/containerd/containerd.sock",
      "/var/run/containerd/containerd.sock",
      "/run/k3s/containerd/containerd.sock",
    };
    for (StringView path : paths)
      if (ExistingSocket(String(path)))
        endpoints.push_back(String(path));
#endif
    return endpoints;
  }

  auto CollectContainerd() -> RuntimeInfo {
    RuntimeInfo runtime {
      .id          = "containerd",
      .displayName = "containerd",
      .kind        = "containerd",
    };

    const Vec<String> endpoints = ContainerdEndpoints();
    if (endpoints.empty()) {
      runtime.error = "No local containerd socket found";
      return runtime;
    }

    Vec<String> failures;
    for (const String& endpoint : endpoints) {
      runtime.endpoint = endpoint;
      Result<String> namespacesResponse = GrpcUnary(endpoint, "/containerd.services.namespaces.v1.Namespaces/List", {});
      if (!namespacesResponse) {
        failures.push_back(namespacesResponse.error().message);
        continue;
      }

      Vec<String> namespaces;
      for (const String& nsMessage : ProtoRepeatedMessages(*namespacesResponse, 1)) {
        String name = ProtoString(nsMessage, 1);
        if (!name.empty())
          namespaces.push_back(name);
      }
      if (namespaces.empty())
        namespaces.push_back("default");

      std::set<String> containerIds;
      std::set<String> runningIds;
      for (const String& ns : namespaces) {
        const Map<String, String> metadata { { "containerd-namespace", ns } };
        if (Result<String> containersResponse = GrpcUnary(endpoint, "/containerd.services.containers.v1.Containers/List", {}, metadata); containersResponse) {
          for (const String& container : ProtoRepeatedMessages(*containersResponse, 1)) {
            const String id = ProtoString(container, 1);
            if (!id.empty())
              containerIds.insert(ns + "/" + id);
          }
        }

        if (Result<String> tasksResponse = GrpcUnary(endpoint, "/containerd.services.tasks.v1.Tasks/List", {}, metadata); tasksResponse) {
          for (const String& task : ProtoRepeatedMessages(*tasksResponse, 1)) {
            const String id = ProtoString(task, 1);
            const Option<u64> status = ProtoVarint(task, 3);
            if (!id.empty() && (!status || *status == 2))
              runningIds.insert(ns + "/" + id);
          }
        }
      }

      runtime.available = true;
      runtime.running   = static_cast<u64>(runningIds.size());
      runtime.total     = static_cast<u64>(containerIds.size());
      runtime.active    = runtime.running > 0;
      runtime.error     = None;
      return runtime;
    }

    runtime.error = failures.empty() ? Option<String>("No usable containerd endpoint found") : Option<String>(failures.front());
    return runtime;
  }

  auto CollectCri() -> RuntimeInfo {
    RuntimeInfo runtime {
      .id          = "cri",
      .displayName = "CRI",
      .kind        = "cri",
    };

    const Vec<String> endpoints = CriEndpoints();
    if (endpoints.empty()) {
      runtime.error = "No local CRI socket found";
      return runtime;
    }

    Vec<String> failures;
    constexpr std::array<StringView, 2> servicePrefixes {
      "/runtime.v1.RuntimeService",
      "/runtime.v1alpha2.RuntimeService",
    };

    for (const String& endpoint : endpoints) {
      runtime.endpoint = endpoint;

      for (StringView service : servicePrefixes) {
        Result<String> status = GrpcUnary(endpoint, String(service) + "/Status", {});
        if (!status) {
          failures.push_back(status.error().message);
          continue;
        }

        u64 running = 0;
        u64 total   = 0;

        if (Result<String> containers = GrpcUnary(endpoint, String(service) + "/ListContainers", {}); containers) {
          for (const String& container : ProtoRepeatedMessages(*containers, 1)) {
            ++total;
            if (const Option<u64> state = ProtoVarint(container, 7); state && *state == 1)
              ++running;
          }
        }

        if (Result<String> sandboxes = GrpcUnary(endpoint, String(service) + "/ListPodSandbox", {}); sandboxes) {
          for (const String& sandbox : ProtoRepeatedMessages(*sandboxes, 1)) {
            ++total;
            if (const Option<u64> state = ProtoVarint(sandbox, 6); state && *state == 0)
              ++running;
          }
        }

        runtime.available = true;
        runtime.running   = running;
        runtime.total     = total;
        runtime.active    = running > 0;
        runtime.error     = None;
        return runtime;
      }
    }

    runtime.error = failures.empty() ? Option<String>("No usable CRI endpoint found") : Option<String>(failures.front());
    return runtime;
  }

  auto RuntimeFields(const RuntimeInfo& runtime) -> PluginFieldObject {
    return PluginFieldObject {
      { "id", runtime.id },
      { "display_name", runtime.displayName },
      { "kind", runtime.kind },
      { "available", runtime.available },
      { "active", runtime.active },
      { "running", runtime.running },
      { "total", runtime.total },
      { "version", runtime.version },
      { "endpoint", runtime.endpoint },
    };
  }

  auto IsAbsentRuntimeError(StringView error) -> bool {
    return HasPrefix(error, "No local ") || HasPrefix(error, "No usable ");
  }

  auto RuntimeDiagnostics(const ContainerInfoData& data) -> Vec<String> {
    Vec<String> diagnostics;
    for (const RuntimeInfo& runtime : data.runtimes)
      if (runtime.error && !IsAbsentRuntimeError(*runtime.error))
        diagnostics.push_back(std::format("{}: {}", runtime.displayName, *runtime.error));
    return diagnostics;
  }

  auto JoinDiagnostics(const Vec<String>& diagnostics) -> String {
    String joined;
    for (const String& diagnostic : diagnostics) {
      if (!joined.empty())
        joined += "; ";
      joined += diagnostic;
    }
    return joined;
  }

  auto CollectAllRuntimes() -> ContainerInfoData {
    static CurlGlobal curlGlobal;

    ContainerInfoData data;
    data.runtimes.push_back(CollectDockerLike(RuntimeKind::Docker, "docker", "Docker", DockerEndpoints()));
    data.runtimes.push_back(CollectDockerLike(RuntimeKind::Podman, "podman", "Podman", PodmanEndpoints()));
    data.runtimes.push_back(CollectContainerd());
    data.runtimes.push_back(CollectCri());
    data.runtimes.push_back(CollectLxd());

    for (const RuntimeInfo& runtime : data.runtimes) {
      data.totalRunning += runtime.running;
      data.totalContainers += runtime.total;
    }
    data.active = data.totalRunning > 0;
    return data;
  }

  class ContainerInfoPlugin final : public IInfoProviderPlugin {
   public:
    ContainerInfoPlugin() {
      m_metadata = {
        .name         = "Container Info",
        .version      = "1.0.0",
        .author       = "Mars",
        .description  = "Reports local container runtime availability and container counts",
        .type         = PluginType::InfoProvider,
        .dependencies = { .requiresFilesystem = true },
      };
    }

    [[nodiscard]] auto getMetadata() const -> const PluginMetadata& override {
      return m_metadata;
    }

    auto initialize(const PluginContext& ctx, PluginCache& cache) -> Result<Unit> override {
      (void)ctx;
      (void)cache;
      m_ready = true;
      return {};
    }

    auto shutdown() -> Unit override {
      m_ready = false;
    }

    [[nodiscard]] auto isReady() const -> bool override {
      return m_ready;
    }

    [[nodiscard]] auto isEnabled() const -> bool override {
      return true;
    }

    [[nodiscard]] auto getProviderId() const -> String override {
      return "container_info";
    }

    auto collectData(PluginCache& cache) -> Result<Unit> override {
      (void)cache;
      m_data = CollectAllRuntimes();
      Vec<String> diagnostics = RuntimeDiagnostics(m_data);
      m_lastError = None;
      if (!diagnostics.empty())
        m_lastError = JoinDiagnostics(diagnostics);
      return {};
    }

    [[nodiscard]] auto getFields() const -> PluginFields override {
      PluginFieldArray runtimes;
      runtimes.reserve(m_data.runtimes.size());
      for (const RuntimeInfo& runtime : m_data.runtimes)
        runtimes.emplace_back(RuntimeFields(runtime));

      return {
        { "active", m_data.active },
        { "total_running", m_data.totalRunning },
        { "total_containers", m_data.totalContainers },
        { "runtimes", std::move(runtimes) },
      };
    }

    [[nodiscard]] auto getDisplayValue() const -> Result<String> override {
      for (const RuntimeInfo& runtime : m_data.runtimes) {
        if (runtime.active)
          return std::format("{} {}/{}", runtime.displayName, runtime.running, runtime.total);
      }

      if (m_lastError)
        ERR(NotFound, *m_lastError);

      ERR(NotFound, "No running local containers found");
    }

    [[nodiscard]] auto getDisplayIcon() const -> String override {
      return " 󰡨  ";
    }

    [[nodiscard]] auto getDisplayLabel() const -> String override {
      return "Containers";
    }

    [[nodiscard]] auto getLastError() const -> Option<String> override {
      return m_lastError;
    }

   private:
    PluginMetadata    m_metadata;
    ContainerInfoData m_data;
    Option<String>    m_lastError;
    bool              m_ready = false;
  };

#if defined(CONTAINER_INFO_ENABLE_TESTS)
  auto RunContainerInfoSelfTests() -> void {
    {
      const auto [running, total] = *ParseDockerContainers(R"json([{"State":"running"},{"State":"exited","Status":"Exited (0)"},{"Status":"Up 2 minutes"}])json");
      assert(running == 2);
      assert(total == 3);
    }

    {
      const auto [running, total] = *ParseLxdInstances(R"json({"metadata":[{"type":"container","status":"Running"},{"type":"virtual-machine","status":"Running"},{"type":"container","status":"Stopped"}]})json");
      assert(running == 1);
      assert(total == 2);
    }

    {
      const String containerdTask {
        '\x0A', '\x05', 'a', 'l', 'p', 'i', 'n', 'e',
        '\x18', '\x02',
      };
      assert(ProtoString(containerdTask, 1) == "alpine");
      assert(ProtoVarint(containerdTask, 3) == 2);
    }

    {
      const String criContainer {
        '\x3A', '\x00',
        '\x38', '\x01',
      };
      assert(ProtoVarint(criContainer, 7) == 1);
    }
  }
#endif
} // namespace

DRAC_PLUGIN(ContainerInfoPlugin)
