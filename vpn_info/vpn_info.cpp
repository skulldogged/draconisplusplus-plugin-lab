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
#include <span>
#include <string_view>

#include <matchit.hpp>

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
    String displayName;
  };

  struct VpnClassification {
    String kind;
    String displayName;
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

  struct VpnRule {
    StringView              kind;
    std::span<const StringView> needles;
    std::span<const StringView> prefixes;
  };

  auto ContainsAny(StringView haystack, std::span<const StringView> needles) -> bool {
    return std::ranges::any_of(needles, [haystack](StringView needle) {
      return haystack.find(needle) != StringView::npos;
    });
  }

  auto StartsWithAny(StringView haystack, std::span<const StringView> prefixes) -> bool {
    return std::ranges::any_of(prefixes, [haystack](StringView prefix) {
      return haystack.starts_with(prefix);
    });
  }

  auto DisplayNameForKind(StringView kind) -> StringView {
    using matchit::match, matchit::is, matchit::_;

    return match(kind)(
      is | "tailscale"     = StringView { "Tailscale" },
      is | "zerotier"      = StringView { "ZeroTier" },
      is | "mullvad"       = StringView { "Mullvad" },
      is | "proton"        = StringView { "Proton VPN" },
      is | "nordvpn"       = StringView { "NordVPN" },
      is | "expressvpn"    = StringView { "ExpressVPN" },
      is | "surfshark"     = StringView { "Surfshark" },
      is | "pia"           = StringView { "Private Internet Access" },
      is | "viscosity"     = StringView { "Viscosity" },
      is | "anyconnect"    = StringView { "Cisco AnyConnect" },
      is | "globalprotect" = StringView { "GlobalProtect" },
      is | "fortinet"      = StringView { "Fortinet VPN" },
      is | "pulse"         = StringView { "Pulse Secure" },
      is | "wireguard"     = StringView { "WireGuard" },
      is | "tun/tap"       = StringView { "OpenVPN" },
      is | "utun"          = StringView { "VPN Tunnel" },
      is | "tunnel"        = StringView { "VPN Tunnel" },
      is | "ppp"           = StringView { "PPP" },
      is | "ipsec"         = StringView { "IPsec" },
      is | _               = kind
    );
  }

  auto MakeClassification(StringView kind) -> VpnClassification {
    return {
      .kind        = String { kind },
      .displayName = String { DisplayNameForKind(kind) },
    };
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

  auto JoinInterfaceDisplayNames(const Vec<VpnInterface>& interfaces) -> String {
    String result;
    usize  size = 0;
    for (const VpnInterface& iface : interfaces)
      size += iface.displayName.size() + 2;
    result.reserve(size);

    for (usize i = 0; i < interfaces.size(); ++i) {
      if (i > 0)
        result += ", ";
      result += interfaces[i].displayName;
    }
    return result;
  }

  auto ClassifyVpnInterface(StringView name, StringView description = {}) -> Option<VpnClassification> {
    const String lowerName        = ToLower(name);
    const String lowerDescription = ToLower(description);
    String       combined;
    combined.reserve(lowerName.size() + lowerDescription.size() + 1);
    combined += lowerName;
    combined += ' ';
    combined += lowerDescription;

    static constexpr std::array<StringView, 0> noPrefixes {};
    static constexpr std::array<StringView, 1> tailscaleNeedles { "tailscale" };
    static constexpr std::array<StringView, 1> zerotierNeedles { "zerotier" };
    static constexpr std::array<StringView, 1> zerotierPrefixes { "zt" };
    static constexpr std::array<StringView, 1> mullvadNeedles { "mullvad" };
    static constexpr std::array<StringView, 2> protonNeedles { "protonvpn", "proton vpn" };
    static constexpr std::array<StringView, 2> nordvpnNeedles { "nordvpn", "nordlynx" };
    static constexpr std::array<StringView, 1> expressvpnNeedles { "expressvpn" };
    static constexpr std::array<StringView, 1> surfsharkNeedles { "surfshark" };
    static constexpr std::array<StringView, 2> piaNeedles { "pia", "private internet access" };
    static constexpr std::array<StringView, 1> viscosityNeedles { "viscosity" };
    static constexpr std::array<StringView, 1> anyconnectNeedles { "anyconnect" };
    static constexpr std::array<StringView, 2> globalprotectNeedles { "globalprotect", "palo alto" };
    static constexpr std::array<StringView, 2> fortinetNeedles { "fortinet", "forticlient" };
    static constexpr std::array<StringView, 1> pulseNeedles { "pulse" };
    static constexpr std::array<StringView, 1> wireguardNeedles { "wireguard" };
    static constexpr std::array<StringView, 1> wireguardPrefixes { "wg" };
    static constexpr std::array<StringView, 3> openvpnNeedles { "openvpn", "tun", "tap" };
    static constexpr std::array<StringView, 2> openvpnPrefixes { "tun", "tap" };
    static constexpr std::array<StringView, 1> utunNeedles { "utun" };
    static constexpr std::array<StringView, 1> utunPrefixes { "utun" };
    static constexpr std::array<StringView, 3> pppNeedles { "ppp", "pptp", "l2tp" };
    static constexpr std::array<StringView, 1> pppPrefixes { "ppp" };
    static constexpr std::array<StringView, 3> ipsecNeedles { "ipsec", "strongswan", "ikev2" };

    static constexpr std::array<VpnRule, 18> rules {
      VpnRule { .kind = "tailscale", .needles = tailscaleNeedles, .prefixes = noPrefixes },
      VpnRule { .kind = "zerotier", .needles = zerotierNeedles, .prefixes = zerotierPrefixes },
      VpnRule { .kind = "mullvad", .needles = mullvadNeedles, .prefixes = noPrefixes },
      VpnRule { .kind = "proton", .needles = protonNeedles, .prefixes = noPrefixes },
      VpnRule { .kind = "nordvpn", .needles = nordvpnNeedles, .prefixes = noPrefixes },
      VpnRule { .kind = "expressvpn", .needles = expressvpnNeedles, .prefixes = noPrefixes },
      VpnRule { .kind = "surfshark", .needles = surfsharkNeedles, .prefixes = noPrefixes },
      VpnRule { .kind = "pia", .needles = piaNeedles, .prefixes = noPrefixes },
      VpnRule { .kind = "viscosity", .needles = viscosityNeedles, .prefixes = noPrefixes },
      VpnRule { .kind = "anyconnect", .needles = anyconnectNeedles, .prefixes = noPrefixes },
      VpnRule { .kind = "globalprotect", .needles = globalprotectNeedles, .prefixes = noPrefixes },
      VpnRule { .kind = "fortinet", .needles = fortinetNeedles, .prefixes = noPrefixes },
      VpnRule { .kind = "pulse", .needles = pulseNeedles, .prefixes = noPrefixes },
      VpnRule { .kind = "wireguard", .needles = wireguardNeedles, .prefixes = wireguardPrefixes },
      VpnRule { .kind = "tun/tap", .needles = openvpnNeedles, .prefixes = openvpnPrefixes },
      VpnRule { .kind = "utun", .needles = utunNeedles, .prefixes = utunPrefixes },
      VpnRule { .kind = "ppp", .needles = pppNeedles, .prefixes = pppPrefixes },
      VpnRule { .kind = "ipsec", .needles = ipsecNeedles, .prefixes = noPrefixes },
    };

    for (const VpnRule& rule : rules)
      if (ContainsAny(combined, rule.needles) || StartsWithAny(lowerName, rule.prefixes))
        return MakeClassification(rule.kind);

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

      Option<VpnClassification> classification = None;
      if (adapter->IfType == IF_TYPE_TUNNEL)
        classification = MakeClassification("tunnel");
      else if (adapter->IfType == IF_TYPE_PPP)
        classification = MakeClassification("ppp");
      else
        classification = ClassifyVpnInterface(name, description);

      if (classification)
        interfaces.push_back({
          .name        = !description.empty() ? description : name,
          .kind        = classification->kind,
          .displayName = classification->displayName,
        });
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

      if (auto classification = ClassifyVpnInterface(name))
        interfaces.push_back({
          .name        = name,
          .kind        = classification->kind,
          .displayName = classification->displayName,
        });
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
        { "primary_kind", m_data.interfaces.empty() ? String {} : m_data.interfaces.front().kind },
        { "primary_display", m_data.interfaces.empty() ? String {} : m_data.interfaces.front().displayName },
        { "interfaces", JoinInterfaceNames(m_data.interfaces) },
        { "display_names", JoinInterfaceDisplayNames(m_data.interfaces) },
      };
    }

    [[nodiscard]] auto getDisplayValue() const -> Result<String> override {
      if (!m_data.active)
        ERR(NotFound, "No active VPN interface found");

      return m_data.interfaces.front().displayName;
    }

    [[nodiscard]] auto getDisplayIcon() const -> String override {
      return " 󰖂  ";
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
