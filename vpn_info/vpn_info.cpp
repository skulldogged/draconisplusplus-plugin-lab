/**
 * @file vpn_info.cpp
 * @brief VPN interface detection plugin for Draconis++.
 *
 * @details This plugin uses OS network adapter/interface lists and conservative
 * name/type heuristics. It does not talk to individual VPN providers.
 */

#include <algorithm>
#include <array>
#include <cctype>
#include <initializer_list>
#include <string_view>

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
  #include <iphlpapi.h>
  #include <windows.h>
#else
  #include <ifaddrs.h>
  #include <net/if.h>
  #include <sys/types.h>
#endif

namespace {
  struct VpnInterface {
    String name;
    String kind;
  };

  struct VpnInfoData {
    bool              active = false;
    Vec<VpnInterface> interfaces;
  };

  auto ToLower(StringView value) -> String {
    String lowered(value);
    std::ranges::transform(lowered, lowered.begin(), [](unsigned char ch) -> char {
      return static_cast<char>(std::tolower(ch));
    });
    return lowered;
  }

  template <usize N>
  auto ContainsAny(StringView haystack, const std::array<StringView, N>& needles) -> bool {
    return std::ranges::any_of(needles, [haystack](StringView needle) {
      return haystack.find(needle) != StringView::npos;
    });
  }

  auto StartsWithAny(StringView haystack, const std::initializer_list<StringView>& prefixes) -> bool {
    return std::ranges::any_of(prefixes, [haystack](StringView prefix) {
      return haystack.starts_with(prefix);
    });
  }

  auto JoinInterfaceNames(const Vec<VpnInterface>& interfaces) -> String {
    String result;
    usize  size = 0;
    for (const VpnInterface& iface : interfaces)
      size += iface.name.size() + 2;
    result.reserve(size);

    for (usize i = 0; i < interfaces.size(); ++i) {
      if (i > 0)
        result += ", ";
      result += interfaces[i].name;
    }
    return result;
  }

  auto ClassifyVpnInterface(StringView name, StringView description = {}) -> Option<String> {
    const String lowerName        = ToLower(name);
    const String lowerDescription = ToLower(description);
    String       combined;
    combined.reserve(lowerName.size() + lowerDescription.size() + 1);
    combined += lowerName;
    combined += ' ';
    combined += lowerDescription;

    if (ContainsAny(combined, std::array<StringView, 1> { "wireguard" }) || StartsWithAny(lowerName, { "wg" }))
      return String { "wireguard" };
    if (ContainsAny(combined, std::array<StringView, 1> { "tailscale" }))
      return String { "tailscale" };
    if (ContainsAny(combined, std::array<StringView, 1> { "zerotier" }) || StartsWithAny(lowerName, { "zt" }))
      return String { "zerotier" };
    if (ContainsAny(combined, std::array<StringView, 3> { "openvpn", "tun", "tap" }) || StartsWithAny(lowerName, { "tun", "tap" }))
      return String { "tun/tap" };
    if (ContainsAny(combined, std::array<StringView, 1> { "utun" }) || StartsWithAny(lowerName, { "utun" }))
      return String { "utun" };
    if (ContainsAny(combined, std::array<StringView, 3> { "ppp", "pptp", "l2tp" }) || StartsWithAny(lowerName, { "ppp" }))
      return String { "ppp" };
    if (ContainsAny(combined, std::array<StringView, 3> { "ipsec", "strongswan", "ikev2" }))
      return String { "ipsec" };
    if (ContainsAny(combined, std::array<StringView, 6> { "anyconnect", "globalprotect", "fortinet", "forticlient", "pulse", "palo alto" }))
      return String { "enterprise" };
    if (ContainsAny(combined, std::array<StringView, 8> { "mullvad", "proton", "nordvpn", "expressvpn", "surfshark", "pia", "private internet access", "viscosity" }))
      return String { "client" };

    return None;
  }

#ifdef _WIN32
  using GetAdaptersAddressesFn = ULONG(WINAPI*)(ULONG, ULONG, PVOID, PIP_ADAPTER_ADDRESSES, PULONG);

  auto WideToUtf8(const wchar_t* input) -> String {
    if (input == nullptr || *input == L'\0')
      return {};

    const int size = WideCharToMultiByte(CP_UTF8, 0, input, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
      return {};

    String output(static_cast<usize>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input, -1, output.data(), size, nullptr, nullptr);
    return output;
  }

  auto CollectVpnInterfaces() -> Vec<VpnInterface> {
    HMODULE ipHelper = LoadLibraryA("iphlpapi.dll");
    if (ipHelper == nullptr)
      return {};

    auto* getAdaptersAddresses = reinterpret_cast<GetAdaptersAddressesFn>(GetProcAddress(ipHelper, "GetAdaptersAddresses"));
    if (getAdaptersAddresses == nullptr) {
      FreeLibrary(ipHelper);
      return {};
    }

    ULONG bufferSize = 15 * 1024;
    Vec<unsigned char> buffer(bufferSize);

    IP_ADAPTER_ADDRESSES* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG result = getAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST, nullptr, addresses, &bufferSize);

    if (result == ERROR_BUFFER_OVERFLOW) {
      buffer.resize(bufferSize);
      addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
      result = getAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST, nullptr, addresses, &bufferSize);
    }

    Vec<VpnInterface> interfaces;
    if (result != NO_ERROR) {
      FreeLibrary(ipHelper);
      return interfaces;
    }

    for (IP_ADAPTER_ADDRESSES* adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
      if (adapter->OperStatus != IfOperStatusUp)
        continue;

      const String name = adapter->AdapterName != nullptr ? String(adapter->AdapterName) : String {};
      const String description = WideToUtf8(adapter->Description);

      Option<String> kind = None;
      if (adapter->IfType == IF_TYPE_TUNNEL)
        kind = String { "tunnel" };
      else if (adapter->IfType == IF_TYPE_PPP)
        kind = String { "ppp" };
      else
        kind = ClassifyVpnInterface(name, description);

      if (kind)
        interfaces.push_back({ .name = !description.empty() ? description : name, .kind = *kind });
    }

    FreeLibrary(ipHelper);
    return interfaces;
  }
#else
  auto CollectVpnInterfaces() -> Vec<VpnInterface> {
    ifaddrs* addrs = nullptr;
    Vec<VpnInterface> interfaces;

    if (getifaddrs(&addrs) != 0 || addrs == nullptr)
      return interfaces;

    for (ifaddrs* iter = addrs; iter != nullptr; iter = iter->ifa_next) {
      if (iter->ifa_name == nullptr)
        continue;

      const unsigned int flags = iter->ifa_flags;
      if ((flags & IFF_UP) == 0 || (flags & IFF_LOOPBACK) != 0)
        continue;

      const String name(iter->ifa_name);
      if (std::ranges::any_of(interfaces, [&name](const VpnInterface& iface) { return iface.name == name; }))
        continue;

      if (auto kind = ClassifyVpnInterface(name))
        interfaces.push_back({ .name = name, .kind = *kind });
    }

    freeifaddrs(addrs);
    return interfaces;
  }
#endif

  class VpnInfoPlugin final : public IInfoProviderPlugin {
   public:
    VpnInfoPlugin() {
      m_metadata = {
        .name         = "VPN Info",
        .version      = "0.1.0",
        .author       = "Mars",
        .description  = "Detects active VPN-like network interfaces",
        .type         = PluginType::InfoProvider,
        .dependencies = { .requiresNetwork = true },
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
      return "vpn_info";
    }

    auto collectData(PluginCache& cache) -> Result<Unit> override {
      (void)cache;
      m_data.interfaces = CollectVpnInterfaces();
      m_data.active     = !m_data.interfaces.empty();
      m_lastError       = None;
      return {};
    }

    [[nodiscard]] auto getFields() const -> Map<String, String> override {
      return {
        { "active", m_data.active ? "true" : "false" },
        { "count", std::to_string(m_data.interfaces.size()) },
        { "primary", m_data.interfaces.empty() ? String {} : m_data.interfaces.front().name },
        { "interfaces", JoinInterfaceNames(m_data.interfaces) },
      };
    }

    [[nodiscard]] auto getDisplayValue() const -> Result<String> override {
      if (!m_data.active)
        ERR(NotFound, "No active VPN interface found");

      return m_data.interfaces.front().name;
    }

    [[nodiscard]] auto getDisplayIcon() const -> String override {
      return " 󰌾  ";
    }

    [[nodiscard]] auto getDisplayLabel() const -> String override {
      return "VPN";
    }

    [[nodiscard]] auto getLastError() const -> Option<String> override {
      return m_lastError;
    }

   private:
    PluginMetadata m_metadata;
    VpnInfoData    m_data;
    Option<String> m_lastError;
    bool           m_ready = false;
  };
} // namespace

DRAC_PLUGIN(VpnInfoPlugin)
