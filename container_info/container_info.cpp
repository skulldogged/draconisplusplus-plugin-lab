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
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <curl/curl.h>
#include <filesystem>
#include <format>
#include <glaze/glaze.hpp>
#include <glaze/toml.hpp>
#include <iterator>
#include <map>
#include <optional>
#include <string_view>
#include <utility>

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
  #include <objbase.h>
  #include <windows.h>
#endif

#if DRAC_PRECOMPILED_CONFIG && __has_include("config.hpp")
  #include "config.hpp"
  #define CONTAINER_INFO_HAS_PRECOMPILED_CONFIG 1
#else
  #define CONTAINER_INFO_HAS_PRECOMPILED_CONFIG 0
#endif

// Field spelling is part of the external Docker, LXD, Podman, and TOML schemas.
// These DTOs stay named so Glaze can reflect them consistently.
// NOLINTBEGIN(readability-identifier-naming,misc-use-internal-linkage)
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

  struct TomlConfig {
    Vec<String> backends;
  };

  struct TomlPlugins {
    TomlConfig container_info;
  };

  struct TomlMainConfig {
    TomlPlugins plugins;
  };

  struct PodmanConnection {
    String URI;
    String Identity;
    bool   IsMachine = false;
  };

  struct PodmanConnectionConfig {
    String                             Default;
    std::map<String, PodmanConnection> Connections;
  };

  struct PodmanConnectionsFile {
    PodmanConnectionConfig Connection;
  };
} // namespace container_info::dto
// NOLINTEND(readability-identifier-naming,misc-use-internal-linkage)

namespace {
  namespace dto = container_info::dto;
  namespace fs  = std::filesystem;

#ifdef _WIN32
  // These declarations mirror the undocumented WSLC COM ABI exactly. Names,
  // array extents, enum representation, vtable signatures, and GUID layout
  // must remain binary-compatible with the Windows implementation.
  // NOLINTBEGIN(readability-identifier-naming,cppcoreguidelines-use-enum-class,performance-enum-size,cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays,cppcoreguidelines-virtual-class-destructor,modernize-use-trailing-return-type,cppcoreguidelines-special-member-functions)
  namespace wslc_api {
    constexpr ULONG WSLC_CONTAINER_ID_LENGTH       = 64;
    constexpr ULONG WSLC_MAX_CONTAINER_NAME_LENGTH = 255;
    constexpr ULONG WSLC_MAX_IMAGE_NAME_LENGTH     = 255;
    constexpr DWORD WSLC_LIST_CONTAINERS_FLAGS_ALL = 1;

    enum WSLCContainerState {
      WslcContainerStateInvalid = 0,
      WslcContainerStateCreated = 1,
      WslcContainerStateRunning = 2,
      WslcContainerStateExited  = 3,
      WslcContainerStateDeleted = 4,
    };

    struct WSLCVersion {
      std::uint32_t Major;
      std::uint32_t Minor;
      std::uint32_t Revision;
    };

    struct WSLCFilter {
      LPCSTR Key;
      LPCSTR Value;
    };

    struct WSLCListContainersOptions {
      DWORD             Flags;
      LONG              Limit;
      const WSLCFilter* Filters;
      ULONG             FiltersCount;
    };

    using WSLCContainerId = char[WSLC_CONTAINER_ID_LENGTH + 1];

    struct WSLCContainerEntry {
      char               Name[WSLC_MAX_CONTAINER_NAME_LENGTH + 1];
      char               Image[WSLC_MAX_IMAGE_NAME_LENGTH + 1];
      WSLCContainerId    Id;
      ULONGLONG          StateChangedAt;
      ULONGLONG          CreatedAt;
      WSLCContainerState State;
    };

    struct WSLCContainerPortMapping;

    struct WSLCSessionListEntry {
      ULONG   SessionId;
      DWORD   CreatorPid;
      wchar_t DisplayName[256];
      wchar_t Sid[257];
    };

    struct IWSLCSession : IUnknown {
      virtual HRESULT STDMETHODCALLTYPE GetId(ULONG*)                                              = 0;
      virtual HRESULT STDMETHODCALLTYPE GetDisplayName(LPWSTR*)                                    = 0;
      virtual HRESULT STDMETHODCALLTYPE GetState(void*)                                            = 0;
      virtual HRESULT STDMETHODCALLTYPE GetTerminationEvent(HANDLE*)                               = 0;
      virtual HRESULT STDMETHODCALLTYPE GetTerminationReason(void*, LPWSTR*)                       = 0;
      virtual HRESULT STDMETHODCALLTYPE PullImage(LPCSTR, LPCSTR, void*, void*)                    = 0;
      virtual HRESULT STDMETHODCALLTYPE BuildImage(const void*, void*, HANDLE)                     = 0;
      virtual HRESULT STDMETHODCALLTYPE LoadImage(void*, ULONGLONG, void*, void*)                  = 0;
      virtual HRESULT STDMETHODCALLTYPE ImportImage(void*, LPCSTR, ULONGLONG, void*, LPSTR*)       = 0;
      virtual HRESULT STDMETHODCALLTYPE SaveImage(void*, LPCSTR, void*, HANDLE)                    = 0;
      virtual HRESULT STDMETHODCALLTYPE SaveImages(void*, const void*, void*, HANDLE)              = 0;
      virtual HRESULT STDMETHODCALLTYPE ListImages(const void*, void*, ULONG*)                     = 0;
      virtual HRESULT STDMETHODCALLTYPE DeleteImage(const void*, void*, ULONG*)                    = 0;
      virtual HRESULT STDMETHODCALLTYPE TagImage(const void*)                                      = 0;
      virtual HRESULT STDMETHODCALLTYPE InspectImage(LPCSTR, LPSTR*)                               = 0;
      virtual HRESULT STDMETHODCALLTYPE PruneImages(const void*, ULONG, void*, ULONG*, ULONGLONG*) = 0;
      virtual HRESULT STDMETHODCALLTYPE CreateContainer(const void*, void*, void**)                = 0;
      virtual HRESULT STDMETHODCALLTYPE OpenContainer(LPCSTR, void**)                              = 0;
      virtual HRESULT STDMETHODCALLTYPE ListContainers(
        const WSLCListContainersOptions*,
        WSLCContainerEntry**,
        ULONG*,
        WSLCContainerPortMapping**,
        ULONG*
      ) = 0;
    };

    struct IWSLCSessionManager : IUnknown {
      virtual HRESULT STDMETHODCALLTYPE GetVersion(WSLCVersion*)                                 = 0;
      virtual HRESULT STDMETHODCALLTYPE CreateSession(const void*, DWORD, void*, IWSLCSession**) = 0;
      virtual HRESULT STDMETHODCALLTYPE EnterSession(LPCWSTR, LPCWSTR, void*, IWSLCSession**)    = 0;
      virtual HRESULT STDMETHODCALLTYPE ListSessions(WSLCSessionListEntry**, ULONG*)             = 0;
      virtual HRESULT STDMETHODCALLTYPE OpenSession(ULONG, IWSLCSession**)                       = 0;
      virtual HRESULT STDMETHODCALLTYPE OpenSessionByName(LPCWSTR, IWSLCSession**)               = 0;
    };

    inline constexpr GUID CLSID_WSLCSessionManager {
      .Data1 = 0xa9b7a1b9,
      .Data2 = 0x0671,
      .Data3 = 0x405c,
      .Data4 = { 0x95, 0xf1, 0xe0, 0x61, 0x2c, 0xb4, 0xce, 0x8f }
    };

    inline constexpr GUID IID_IWSLCSessionManager {
      .Data1 = 0x82a7abc8,
      .Data2 = 0x6b50,
      .Data3 = 0x43fc,
      .Data4 = { 0xab, 0x96, 0x15, 0xfb, 0xbe, 0x7e, 0x87, 0x60 }
    };
  } // namespace wslc_api
  // NOLINTEND(readability-identifier-naming,cppcoreguidelines-use-enum-class,performance-enum-size,cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays,cppcoreguidelines-virtual-class-destructor,modernize-use-trailing-return-type,cppcoreguidelines-special-member-functions)
#endif

  constexpr long CONNECT_TIMEOUT_MS = 350;
  constexpr long TOTAL_TIMEOUT_MS   = 900;

  enum class RuntimeKind : u8 {
    Docker,
    Podman,
  };

  struct RuntimeInfo {
    String         id;
    String         displayName;
    String         kind;
    bool           available  = false;
    bool           configured = false;
    bool           active     = false;
    u64            running    = 0;
    u64            total      = 0;
    String         version;
    String         endpoint;
    Option<String> error = None;

    RuntimeInfo(String runtimeId, String displayName, String kind)
      : id(std::move(runtimeId)), displayName(std::move(displayName)), kind(std::move(kind)) {}
  };

  struct ContainerInfoData {
    bool             active          = false;
    u64              totalRunning    = 0;
    u64              totalContainers = 0;
    Vec<RuntimeInfo> runtimes;
  };

  struct ContainerInfoConfig {
    Vec<String> backends {
#ifdef _WIN32
      "docker",
      "podman",
      "wsl",
#else
      "docker",
      "podman",
      "lxd",
#endif
    };
  };

  auto HasPrefix(StringView value, StringView prefix) -> bool {
    return value.size() >= prefix.size() && value.starts_with(prefix);
  }

  auto ToLower(StringView value) -> String {
    String result(value);
    std::ranges::transform(result, result.begin(), [](unsigned char character) -> char {
      return static_cast<char>(std::tolower(character));
    });
    return result;
  }

  auto NormalizeBackend(StringView backend) -> String {
    String normalized = ToLower(backend);
    std::erase_if(normalized, [](unsigned char character) -> bool {
      return std::isspace(character) || character == '_' || character == '-';
    });
    if (normalized == "dockerengine")
      return "docker";
    if (normalized == "lxc")
      return "lxd";
    if (normalized == "wslc" || normalized == "wslcontainer" || normalized == "wslcontainers")
      return "wsl";
    return normalized;
  }

  auto BackendEnabled(const ContainerInfoConfig& config, StringView backend) -> bool {
    const String normalizedBackend = NormalizeBackend(backend);
    return std::ranges::any_of(config.backends, [&normalizedBackend](StringView configured) -> bool {
      return NormalizeBackend(configured) == normalizedBackend;
    });
  }

  auto ParseConfig(const dto::TomlConfig& tomlCfg) -> ContainerInfoConfig {
    ContainerInfoConfig config;
    if (!tomlCfg.backends.empty())
      config.backends = tomlCfg.backends;

    Vec<String> normalized;
    for (const StringView backend : config.backends) {
      const String value = NormalizeBackend(backend);
      if (value == "all")
        return ContainerInfoConfig {};
      if (value == "docker" || value == "podman" || value == "lxd" || value == "wsl")
        if (!std::ranges::contains(normalized, value))
          normalized.push_back(value);
    }

    config.backends = std::move(normalized);
    return config;
  }

  [[maybe_unused]] auto LoadConfigFromToml(StringView tomlConfig, StringView sourceName) -> Result<ContainerInfoConfig> {
    dto::TomlConfig tomlCfg;
    String          buffer(tomlConfig);
    glz::context    ctx {};
    ctx.current_file = String(sourceName);

    if (const auto readError = glz::read<glz::opts { .format = glz::TOML, .error_on_unknown_keys = false }>(tomlCfg, buffer, ctx); readError)
      ERR_FMT(ParseError, "Failed to parse container_info config: {}", glz::format_error(readError, buffer));

    return ParseConfig(tomlCfg);
  }

  auto LoadConfigFromPrecompiled() -> ContainerInfoConfig {
#if CONTAINER_INFO_HAS_PRECOMPILED_CONFIG
    dto::TomlConfig tomlCfg;
    for (const char* backend : draconis::config::CONTAINER_INFO_BACKENDS)
      tomlCfg.backends.emplace_back(backend);
    return ParseConfig(tomlCfg);
#else
    return ContainerInfoConfig {};
#endif
  }

  [[maybe_unused]] auto LoadConfigFromFilesystem(const fs::path& configDir) -> Result<ContainerInfoConfig> {
    const fs::path pluginConfigPath = configDir / "container_info.toml";
    if (fs::exists(pluginConfigPath)) {
      String       buffer;
      glz::context ctx {};
      ctx.current_file = pluginConfigPath.string();
      if (const auto fileError = glz::file_to_buffer(buffer, ctx.current_file); bool(fileError))
        ERR_FMT(IoError, "Failed to read {}", pluginConfigPath.string());

      dto::TomlConfig tomlCfg;
      if (const auto readError = glz::read<glz::opts { .format = glz::TOML, .error_on_unknown_keys = false }>(tomlCfg, buffer, ctx); readError)
        ERR_FMT(ParseError, "Failed to parse {}: {}", pluginConfigPath.string(), glz::format_error(readError, buffer));

      return ParseConfig(tomlCfg);
    }

    const fs::path mainConfigPath = configDir.parent_path() / "config.toml";
    if (fs::exists(mainConfigPath)) {
      String       buffer;
      glz::context ctx {};
      ctx.current_file = mainConfigPath.string();
      if (const auto fileError = glz::file_to_buffer(buffer, ctx.current_file); bool(fileError))
        ERR_FMT(IoError, "Failed to read {}", mainConfigPath.string());

      dto::TomlMainConfig tomlCfg;
      if (const auto readError = glz::read<glz::opts { .format = glz::TOML, .error_on_unknown_keys = false }>(tomlCfg, buffer, ctx); readError)
        ERR_FMT(ParseError, "Failed to parse {}: {}", mainConfigPath.string(), glz::format_error(readError, buffer));

      return ParseConfig(tomlCfg.plugins.container_info);
    }

    return ContainerInfoConfig {};
  }

  auto ParseDockerContainers(StringView body) -> Result<Pair<u64, u64>> {
    const String              buffer(body);
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
    const String       buffer(body);
    dto::DockerVersion version;
    if (auto errc = glz::read<glz::opts { .error_on_unknown_keys = false }>(version, buffer); errc.ec != glz::error_code::none)
      return {};
    return !version.Version.empty() ? version.Version : version.ApiVersion;
  }

  auto ParseLxdInstances(StringView body) -> Result<Pair<u64, u64>> {
    const String              buffer(body);
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
    const String         buffer(body);
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
    bool   configured = false;
  };

  class CurlGlobal {
   public:
    CurlGlobal() {
      curl_global_init(CURL_GLOBAL_DEFAULT);
    }
    ~CurlGlobal() {
      curl_global_cleanup();
    }

    CurlGlobal(const CurlGlobal&)                    = delete;
    CurlGlobal(CurlGlobal&&)                         = delete;
    auto operator=(const CurlGlobal&) -> CurlGlobal& = delete;
    auto operator=(CurlGlobal&&) -> CurlGlobal&      = delete;
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

    String       body;
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
  auto EnvValue(const char* name) -> Option<String> {
    char* value = nullptr;
    usize size  = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr)
      return None;
    String result(value);
    std::free(value); // NOLINT(cppcoreguidelines-no-malloc): _dupenv_s requires free().
    return result;
  }

  auto WideFromUtf8(StringView value) -> std::wstring {
    if (value.empty())
      return {};
    const int    needed = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring out(static_cast<usize>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), needed);
    return out;
  }

  auto DecodeChunkedHttpBody(StringView body) -> Result<String> {
    String decoded;
    usize  pos = 0;

    for (;;) {
      const usize lineEnd = body.find("\r\n", pos);
      if (lineEnd == StringView::npos)
        ERR(ParseError, "Chunked HTTP response ended before chunk size");

      StringView sizeText = body.substr(pos, lineEnd - pos);
      if (const usize extension = sizeText.find(';'); extension != StringView::npos)
        sizeText = sizeText.substr(0, extension);
      while (!sizeText.empty() && std::isspace(static_cast<unsigned char>(sizeText.front())))
        sizeText.remove_prefix(1);
      while (!sizeText.empty() && std::isspace(static_cast<unsigned char>(sizeText.back())))
        sizeText.remove_suffix(1);
      if (sizeText.empty())
        ERR(ParseError, "Chunked HTTP response contained an empty chunk size");

      usize       chunkSize = 0;
      const char* sizeEnd   = sizeText.data() + sizeText.size();                        // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic): from_chars requires a pointer pair.
      const auto [ptr, ec]  = std::from_chars(sizeText.data(), sizeEnd, chunkSize, 16); // NOLINT(bugprone-suspicious-stringview-data-usage): from_chars consumes the explicit bounded range.
      if (ec != std::errc {} || ptr != sizeEnd)
        ERR(ParseError, "Chunked HTTP response contained an invalid chunk size");

      pos = lineEnd + 2;
      if (chunkSize == 0)
        return decoded;
      if (body.size() - pos < chunkSize)
        ERR(ParseError, "Chunked HTTP response ended before chunk data");

      decoded.append(body.substr(pos, chunkSize));
      pos += chunkSize;
      if (body.size() - pos < 2 || body.substr(pos, 2) != "\r\n")
        ERR(ParseError, "Chunked HTTP response chunk was not CRLF terminated");
      pos += 2;
    }
  }

  auto ParseRawHttpResponse(const String& raw, StringView display) -> Result<HttpResponse> {
    const usize headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == String::npos)
      ERR(ParseError, "HTTP response did not contain headers");

    const StringView headers    = StringView(raw).substr(0, headerEnd);
    const StringView statusLine = headers.substr(0, headers.find("\r\n"));
    long             status     = 0;
    if (statusLine.size() >= 12) {
      const StringView statusCode = statusLine.substr(9, 3);
      const char*      statusEnd  = statusCode.data() + statusCode.size(); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic): from_chars requires a pointer pair.
      std::from_chars(statusCode.data(), statusEnd, status);               // NOLINT(bugprone-suspicious-stringview-data-usage): from_chars consumes the explicit bounded range.
    }

    if (status >= 400)
      ERR_FMT(ApiUnavailable, "{} returned HTTP {}", display, status);

    String       body         = raw.substr(headerEnd + 4);
    const String lowerHeaders = ToLower(headers);
    if (lowerHeaders.contains("transfer-encoding:") && lowerHeaders.contains("chunked"))
      body = TRY(DecodeChunkedHttpBody(body));

    return HttpResponse {
      .status = status,
      .body   = std::move(body),
    };
  }

  auto BuildHttpGetRequest(StringView path) -> String {
    constexpr StringView prefix = "GET ";
    constexpr StringView suffix = " HTTP/1.1\r\nHost: localhost\r\nUser-Agent: draconisplusplus-container-info/1\r\nConnection: close\r\n\r\n";

    String request;
    request.reserve(prefix.size() + path.size() + suffix.size());
    request += prefix;
    request += path;
    request += suffix;
    return request;
  }

  auto HttpGetNamedPipe(const HttpEndpoint& endpoint, StringView path) -> Result<HttpResponse> {
    const std::wstring pipe   = WideFromUtf8(endpoint.namedPipe);
    HANDLE             handle = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
      ERR_FMT(ApiUnavailable, "{} pipe unavailable: {}", endpoint.display, static_cast<unsigned long>(GetLastError()));

    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(handle, &mode, nullptr, nullptr);

    const String request = BuildHttpGetRequest(path);
    DWORD        written = 0;
    if (!WriteFile(handle, request.data(), static_cast<DWORD>(request.size()), &written, nullptr)) {
      const DWORD err = GetLastError();
      CloseHandle(handle);
      ERR_FMT(IoError, "{} pipe write failed: {}", endpoint.display, static_cast<unsigned long>(err));
    }

    String                 raw;
    std::array<char, 4096> buffer {};
    DWORD                  read = 0;
    while (ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read > 0)
      raw.append(buffer.data(), read);
    CloseHandle(handle);

    return ParseRawHttpResponse(raw, endpoint.display);
  }

  auto PodmanConnectionFilePath() -> fs::path {
    if (Option<String> appData = EnvValue("APPDATA"))
      return fs::path(*appData) / "containers" / "podman-connections.json";
    return {};
  }

  auto EndpointFromPodmanUri(StringView uri, StringView endpointId, bool configured = false) -> Option<HttpEndpoint> {
    if (HasPrefix(uri, "npipe://")) {
      String pipe(uri.substr(8));
      std::ranges::replace(pipe, '/', '\\');
      if (HasPrefix(pipe, R"(.\pipe\)"))
        pipe = R"(\\)" + pipe;
      if (HasPrefix(pipe, R"(\\.\pipe\)"))
        return HttpEndpoint { .id = String(endpointId), .display = "Podman", .urlBase = "http://d", .unixSocket = {}, .namedPipe = std::move(pipe), .configured = configured };
    }

    return None;
  }

  auto PodmanConnectionEndpoints() -> Vec<HttpEndpoint> {
    Vec<HttpEndpoint> endpoints;
    const fs::path    path = PodmanConnectionFilePath();
    if (path.empty() || !fs::exists(path))
      return endpoints;

    String       buffer;
    glz::context ctx {};
    ctx.current_file = path.string();
    if (const auto fileError = glz::file_to_buffer(buffer, ctx.current_file); bool(fileError))
      return endpoints;

    dto::PodmanConnectionsFile connections;
    if (const auto readError = glz::read<glz::opts { .error_on_unknown_keys = false }>(connections, buffer, ctx); readError)
      return endpoints;

    auto addEndpoint = [&endpoints](StringView name, const dto::PodmanConnection& connection) -> void {
      if (Option<HttpEndpoint> endpoint = EndpointFromPodmanUri(connection.URI, std::format("podman:{}", name), true))
        endpoints.push_back(std::move(*endpoint));
    };

    if (!connections.Connection.Default.empty()) {
      if (const auto iter = connections.Connection.Connections.find(connections.Connection.Default); iter != connections.Connection.Connections.end()) {
        addEndpoint(iter->first, iter->second);
        return endpoints;
      }
    }

    for (const auto& [name, connection] : connections.Connection.Connections)
      if (name != connections.Connection.Default)
        addEndpoint(name, connection);

    return endpoints;
  }
#endif

  auto HttpGet(const HttpEndpoint& endpoint, StringView path) -> Result<HttpResponse> {
#ifdef _WIN32
    if (!endpoint.namedPipe.empty())
      return HttpGetNamedPipe(endpoint, path);
#endif
    return HttpGetCurl(endpoint, path);
  }

  auto EndpointLabel(const HttpEndpoint& endpoint) -> String {
    if (!endpoint.unixSocket.empty())
      return endpoint.unixSocket;
    if (!endpoint.namedPipe.empty())
      return endpoint.namedPipe;
    return endpoint.urlBase;
  }

#ifndef _WIN32
  auto ExistingSocket(String path) -> Option<String> {
    std::error_code ec;
    if (std::filesystem::exists(path, ec))
      return path;
    return None;
  }
#endif

  auto DockerEndpoints() -> Vec<HttpEndpoint> {
    Vec<HttpEndpoint> endpoints;
#ifdef _WIN32
    endpoints.push_back({ .id = "docker-npipe", .display = "Docker Engine", .urlBase = "http://localhost", .unixSocket = {}, .namedPipe = R"(\\.\pipe\docker_engine)" });
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
    Vec<HttpEndpoint> configured = PodmanConnectionEndpoints();
    endpoints.insert(endpoints.end(), std::make_move_iterator(configured.begin()), std::make_move_iterator(configured.end()));
    if (!endpoints.empty())
      return endpoints;

    if (Option<String> containerHost = EnvValue("CONTAINER_HOST"))
      if (Option<HttpEndpoint> endpoint = EndpointFromPodmanUri(*containerHost, "podman:CONTAINER_HOST", true))
        endpoints.push_back(std::move(*endpoint));

    endpoints.push_back({ .id = "podman-npipe", .display = "Podman", .urlBase = "http://d", .unixSocket = {}, .namedPipe = R"(\\.\pipe\podman-machine-default)" });
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

  template <typename GetHttp>
  auto CollectDockerLikeEndpointWith(RuntimeKind kind, RuntimeInfo& runtime, GetHttp getHttp) -> Result<Unit> {
    Result<HttpResponse> containersResult = getHttp("/containers/json?all=true");
    if (!containersResult)
      return Err(containersResult.error());
    const HttpResponse containersResponse = std::move(*containersResult);

    Result<Pair<u64, u64>> countsResult = ParseDockerContainers(containersResponse.body);
    if (!countsResult)
      return Err(countsResult.error());
    Pair<u64, u64> counts = *countsResult;

    if (Result<HttpResponse> versionResponse = getHttp(kind == RuntimeKind::Podman ? "/libpod/version" : "/version"); versionResponse) {
      runtime.version = ParseDockerVersion(versionResponse->body);
    } else if (kind == RuntimeKind::Podman) {
      if (Result<HttpResponse> fallbackVersionResponse = getHttp("/version"); fallbackVersionResponse)
        runtime.version = ParseDockerVersion(fallbackVersionResponse->body);
    }

    const auto [running, total] = counts;
    runtime.available           = true;
    runtime.running             = running;
    runtime.total               = total;
    runtime.active              = running > 0;
    runtime.error               = None;
    return {};
  }

  auto CollectDockerLikeEndpoint(RuntimeKind kind, RuntimeInfo& runtime, const HttpEndpoint& endpoint) -> Result<Unit> {
    runtime.endpoint = EndpointLabel(endpoint);

    return CollectDockerLikeEndpointWith(kind, runtime, [&endpoint](StringView path) -> Result<HttpResponse> {
      return HttpGet(endpoint, path);
    });
  }

  auto CollectDockerLike(RuntimeKind kind, StringView runtimeId, StringView display, Vec<HttpEndpoint> endpoints) -> RuntimeInfo {
    RuntimeInfo runtime(String(runtimeId), String(display), kind == RuntimeKind::Docker ? "docker" : "podman");

    if (endpoints.empty()) {
      runtime.error = "No local API socket or named pipe found";
      return runtime;
    }

    runtime.configured = std::ranges::any_of(endpoints, [](const HttpEndpoint& endpoint) -> bool {
      return endpoint.configured;
    });

    Vec<String> failures;
    for (const HttpEndpoint& endpoint : endpoints) {
      Result<Unit> collected = CollectDockerLikeEndpoint(kind, runtime, endpoint);
      if (collected)
        return runtime;
      failures.push_back(collected.error().message);
    }

    runtime.error = failures.empty() ? Option<String>("No usable endpoint found") : Option<String>(failures.front());
    return runtime;
  }

  auto CollectLxd() -> RuntimeInfo {
    RuntimeInfo runtime("lxd", "LXD", "lxd");

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
      runtime.available           = true;
      runtime.running             = running;
      runtime.total               = total;
      runtime.active              = running > 0;
      runtime.error               = None;
      return runtime;
    }

    runtime.error = failures.empty() ? Option<String>("No usable LXD endpoint found") : Option<String>(failures.front());
    return runtime;
  }

  auto RuntimeFields(const RuntimeInfo& runtime) -> PluginFieldObject {
    PluginFieldObject fields {
      {           "id",          runtime.id },
      { "display_name", runtime.displayName },
      {         "kind",        runtime.kind },
      {    "available",   runtime.available },
      {       "active",      runtime.active },
      {      "running",     runtime.running },
      {        "total",       runtime.total },
      {      "version",     runtime.version },
      {     "endpoint",    runtime.endpoint },
    };
    if (runtime.error)
      fields.emplace("error", *runtime.error);
    return fields;
  }

#ifdef _WIN32
  auto HResultString(HRESULT result) -> String {
    return std::format("0x{:08X}", static_cast<unsigned int>(static_cast<std::uint32_t>(result)));
  }

  void ReleaseUnknown(IUnknown* unknown) {
    if (unknown != nullptr)
      unknown->Release();
  }

  void ConfigureComProxy(IUnknown* unknown) {
    if (unknown == nullptr)
      return;

    (void)CoSetProxyBlanket(
      unknown,
      RPC_C_AUTHN_DEFAULT,
      RPC_C_AUTHZ_DEFAULT,
      COLE_DEFAULT_PRINCIPAL,
      RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
      RPC_C_IMP_LEVEL_IMPERSONATE,
      nullptr,
      EOAC_NONE
    );
  }
#endif

  auto CollectWslContainers() -> RuntimeInfo {
    RuntimeInfo runtime("wsl", "WSL Containers", "wsl");

#ifdef _WIN32
    runtime.endpoint       = "WSLCSessionManager";
    HMODULE sdk            = LoadLibraryW(L"wslcsdk.dll");
    using WslcGetVersionFn = HRESULT(WINAPI*)(wslc_api::WSLCVersion*);
    if (sdk != nullptr) {
      // GetProcAddress exposes an untyped address by Win32 API design.
      auto* getVersionProc = reinterpret_cast<WslcGetVersionFn>(GetProcAddress(sdk, "WslcGetVersion")); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
      if (getVersionProc != nullptr) {
        wslc_api::WSLCVersion version {};
        if (SUCCEEDED(getVersionProc(&version)))
          runtime.version = std::format("{}.{}.{}", version.Major, version.Minor, version.Revision);
      }
      FreeLibrary(sdk);
    }

    const HRESULT initHr    = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool    uninitCom = SUCCEEDED(initHr);
    if (FAILED(initHr) && initHr != RPC_E_CHANGED_MODE) {
      runtime.error = std::format("Failed to initialize COM for WSL Containers: {}", HResultString(initHr));
      return runtime;
    }

    wslc_api::IWSLCSessionManager* manager    = nullptr;
    auto**                         managerOut = reinterpret_cast<void**>(&manager); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): COM out parameter ABI.
    HRESULT                        result     = CoCreateInstance(
      wslc_api::CLSID_WSLCSessionManager,
      nullptr,
      CLSCTX_LOCAL_SERVER,
      wslc_api::IID_IWSLCSessionManager,
      managerOut
    );
    if (FAILED(result)) {
      if (uninitCom)
        CoUninitialize();
      runtime.error = (result == REGDB_E_CLASSNOTREG) ? Option<String>("No local WSL container API found")
                                                      : Option<String>(std::format("Failed to open WSL container service: {}", HResultString(result)));
      return runtime;
    }

    ConfigureComProxy(manager);
    if (runtime.version.empty()) {
      wslc_api::WSLCVersion version {};
      if (SUCCEEDED(manager->GetVersion(&version)))
        runtime.version = std::format("{}.{}.{}", version.Major, version.Minor, version.Revision);
    }

    wslc_api::WSLCSessionListEntry* sessions     = nullptr;
    ULONG                           sessionCount = 0;
    result                                       = manager->ListSessions(&sessions, &sessionCount);
    if (FAILED(result)) {
      ReleaseUnknown(manager);
      if (uninitCom)
        CoUninitialize();
      runtime.error = std::format("Failed to list WSL container sessions: {}", HResultString(result));
      return runtime;
    }

    runtime.available = true;

    for (const wslc_api::WSLCSessionListEntry& sessionEntry : Span(sessions, sessionCount)) {
      wslc_api::IWSLCSession* session = nullptr;
      result                          = manager->OpenSession(sessionEntry.SessionId, &session);
      if (FAILED(result))
        continue;

      ConfigureComProxy(session);
      const wslc_api::WSLCListContainersOptions options {
        .Flags        = wslc_api::WSLC_LIST_CONTAINERS_FLAGS_ALL,
        .Limit        = -1,
        .Filters      = nullptr,
        .FiltersCount = 0,
      };
      wslc_api::WSLCContainerEntry*       containers     = nullptr;
      ULONG                               containerCount = 0;
      wslc_api::WSLCContainerPortMapping* ports          = nullptr;
      ULONG                               portsCount     = 0;
      result                                             = session->ListContainers(&options, &containers, &containerCount, &ports, &portsCount);
      if (SUCCEEDED(result)) {
        runtime.total += containerCount;
        for (const wslc_api::WSLCContainerEntry& container : Span(containers, containerCount))
          if (container.State == wslc_api::WslcContainerStateRunning)
            ++runtime.running;
      } else if (!runtime.error) {
        runtime.error = std::format("Failed to list WSL containers in session {}: {}", sessionEntry.SessionId, HResultString(result));
      }

      CoTaskMemFree(containers);
      CoTaskMemFree(ports);
      ReleaseUnknown(session);
    }

    CoTaskMemFree(sessions);
    ReleaseUnknown(manager);
    if (uninitCom)
      CoUninitialize();

    runtime.active = runtime.running > 0;
    return runtime;
#else
    runtime.error = "No local WSL container API found";
    return runtime;
#endif
  }

  auto IsAbsentRuntimeError(StringView error) -> bool {
    return HasPrefix(error, "No local ") || HasPrefix(error, "No usable ");
  }

  auto RuntimeDiagnostics(const ContainerInfoData& data) -> Vec<String> {
    Vec<String> diagnostics;
    for (const RuntimeInfo& runtime : data.runtimes)
      if (runtime.error && !IsAbsentRuntimeError(*runtime.error))
        diagnostics.push_back(std::format("{}: {}", runtime.displayName, *runtime.error));

    if (diagnostics.empty() && !std::ranges::any_of(data.runtimes, [](const RuntimeInfo& runtime) -> bool { return runtime.available; }))
      diagnostics.emplace_back("No selected container backend is available through a supported local API");

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

  auto CollectAllRuntimes(const ContainerInfoConfig& config) -> ContainerInfoData {
    static const CurlGlobal CURL_GLOBAL;

    ContainerInfoData data;
    if (BackendEnabled(config, "docker"))
      data.runtimes.push_back(CollectDockerLike(RuntimeKind::Docker, "docker", "Docker", DockerEndpoints()));
    if (BackendEnabled(config, "podman"))
      data.runtimes.push_back(CollectDockerLike(RuntimeKind::Podman, "podman", "Podman", PodmanEndpoints()));
    if (BackendEnabled(config, "wsl"))
      data.runtimes.push_back(CollectWslContainers());
    if (BackendEnabled(config, "lxd"))
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

    auto setConfig(StringView tomlConfig) -> Result<Unit> override {
      if (tomlConfig.empty())
        return {};

      m_runtimeConfig = String(tomlConfig);
      return {};
    }

    auto initialize(const PluginContext& ctx, PluginCache& cache) -> Result<Unit> override {
      (void)cache;

#if DRAC_PRECOMPILED_CONFIG
      (void)ctx;
      m_config = LoadConfigFromPrecompiled();
#else
      if (m_runtimeConfig)
        m_config = TRY(LoadConfigFromToml(*m_runtimeConfig, "container_info runtime config"));
      else
        m_config = TRY(LoadConfigFromFilesystem(ctx.configDir));
#endif

      if (m_config.backends.empty())
        ERR(ConfigurationError, "container_info has no enabled backends");

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
      m_data                        = CollectAllRuntimes(m_config);
      const Vec<String> diagnostics = RuntimeDiagnostics(m_data);
      m_lastError                   = None;
      if (!diagnostics.empty())
        m_lastError = JoinDiagnostics(diagnostics);
      return {};
    }

    [[nodiscard]] auto getFields() const -> PluginFields override {
      PluginFieldArray runtimes;
      runtimes.reserve(m_data.runtimes.size());
      for (const RuntimeInfo& runtime : m_data.runtimes) {
        if (!runtime.available && !runtime.configured)
          continue;
        runtimes.emplace_back(RuntimeFields(runtime));
      }

      return {
        {           "active",          m_data.active },
        {    "total_running",    m_data.totalRunning },
        { "total_containers", m_data.totalContainers },
        {         "runtimes",    std::move(runtimes) },
      };
    }

    [[nodiscard]] auto getDisplayValue() const -> Result<String> override {
      for (const RuntimeInfo& runtime : m_data.runtimes) {
        if (runtime.available)
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
    PluginMetadata      m_metadata;
    ContainerInfoConfig m_config;
    ContainerInfoData   m_data;
    Option<String>      m_lastError;
    Option<String>      m_runtimeConfig;
    bool                m_ready = false;
  };

#ifdef CONTAINER_INFO_ENABLE_TESTS
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
  }
#endif
} // namespace

DRAC_PLUGIN(ContainerInfoPlugin)
