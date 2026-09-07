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
#include <matchit.hpp>
#include <span>
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
// clang-format off: Winsock must precede windows.h and IP Helper headers.
  #include <winsock2.h>
  #include <windows.h>
  #include <iphlpapi.h>
  #include <ws2tcpip.h>
// clang-format on
#elifdef __linux__
  #include <filesystem>
  #include <fstream>
  #include <net/if.h>
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
    bool   active = false;
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
    std::ranges::transform(lowered, lowered.begin(), [](unsigned char character) -> char {
      return static_cast<char>(std::tolower(character));
    });
    return lowered;
  }

  struct VpnRule {
    StringView                  kind;
    std::span<const StringView> needles;
    std::span<const StringView> prefixes;
  };

  auto ContainsAny(StringView haystack, std::span<const StringView> needles) -> bool {
    return std::ranges::any_of(needles, [haystack](StringView needle) -> bool {
      return haystack.contains(needle);
    });
  }

  auto StartsWithAny(StringView haystack, std::span<const StringView> prefixes) -> bool {
    return std::ranges::any_of(prefixes, [haystack](StringView prefix) -> bool {
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
      is | "openvpn"       = StringView { "OpenVPN" },
      is | "tun/tap"       = StringView { "VPN Tunnel" },
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

  auto ClassifyVpnInterface(StringView name, StringView description = {}) -> Option<VpnClassification> {
    const String lowerName        = ToLower(name);
    const String lowerDescription = ToLower(description);
    String       combined;
    combined.reserve(lowerName.size() + lowerDescription.size() + 1);
    combined += lowerName;
    combined += ' ';
    combined += lowerDescription;

    static constexpr std::array<StringView, 0> NO_PREFIXES {};
    static constexpr std::array<StringView, 1> TAILSCALE_NEEDLES { "tailscale" };
    static constexpr std::array<StringView, 1> ZEROTIER_NEEDLES { "zerotier" };
    static constexpr std::array<StringView, 1> ZEROTIER_PREFIXES { "zt" };
    static constexpr std::array<StringView, 1> MULLVAD_NEEDLES { "mullvad" };
    static constexpr std::array<StringView, 2> PROTON_NEEDLES { "protonvpn", "proton vpn" };
    static constexpr std::array<StringView, 2> NORDVPN_NEEDLES { "nordvpn", "nordlynx" };
    static constexpr std::array<StringView, 1> EXPRESSVPN_NEEDLES { "expressvpn" };
    static constexpr std::array<StringView, 1> SURFSHARK_NEEDLES { "surfshark" };
    static constexpr std::array<StringView, 2> PIA_NEEDLES { "pia", "private internet access" };
    static constexpr std::array<StringView, 1> VISCOSITY_NEEDLES { "viscosity" };
    static constexpr std::array<StringView, 1> ANYCONNECT_NEEDLES { "anyconnect" };
    static constexpr std::array<StringView, 2> GLOBALPROTECT_NEEDLES { "globalprotect", "palo alto" };
    static constexpr std::array<StringView, 2> FORTINET_NEEDLES { "fortinet", "forticlient" };
    static constexpr std::array<StringView, 1> PULSE_NEEDLES { "pulse" };
    static constexpr std::array<StringView, 1> WIREGUARD_NEEDLES { "wireguard" };
    static constexpr std::array<StringView, 1> WIREGUARD_PREFIXES { "wg" };
    static constexpr std::array<StringView, 1> OPENVPN_NEEDLES { "openvpn" };
    static constexpr std::array<StringView, 2> OPENVPN_PREFIXES { "tun", "tap" };
    static constexpr std::array<StringView, 1> UTUN_NEEDLES { "utun" };
    static constexpr std::array<StringView, 1> UTUN_PREFIXES { "utun" };
    static constexpr std::array<StringView, 3> PPP_NEEDLES { "ppp", "pptp", "l2tp" };
    static constexpr std::array<StringView, 1> PPP_PREFIXES { "ppp" };
    static constexpr std::array<StringView, 3> IPSEC_NEEDLES { "ipsec", "strongswan", "ikev2" };

    static constexpr std::array<VpnRule, 19> RULES {
      VpnRule {     .kind = "tailscale",     .needles = TAILSCALE_NEEDLES,        .prefixes = NO_PREFIXES },
      VpnRule {      .kind = "zerotier",      .needles = ZEROTIER_NEEDLES,  .prefixes = ZEROTIER_PREFIXES },
      VpnRule {       .kind = "mullvad",       .needles = MULLVAD_NEEDLES,        .prefixes = NO_PREFIXES },
      VpnRule {        .kind = "proton",        .needles = PROTON_NEEDLES,        .prefixes = NO_PREFIXES },
      VpnRule {       .kind = "nordvpn",       .needles = NORDVPN_NEEDLES,        .prefixes = NO_PREFIXES },
      VpnRule {    .kind = "expressvpn",    .needles = EXPRESSVPN_NEEDLES,        .prefixes = NO_PREFIXES },
      VpnRule {     .kind = "surfshark",     .needles = SURFSHARK_NEEDLES,        .prefixes = NO_PREFIXES },
      VpnRule {           .kind = "pia",           .needles = PIA_NEEDLES,        .prefixes = NO_PREFIXES },
      VpnRule {     .kind = "viscosity",     .needles = VISCOSITY_NEEDLES,        .prefixes = NO_PREFIXES },
      VpnRule {    .kind = "anyconnect",    .needles = ANYCONNECT_NEEDLES,        .prefixes = NO_PREFIXES },
      VpnRule { .kind = "globalprotect", .needles = GLOBALPROTECT_NEEDLES,        .prefixes = NO_PREFIXES },
      VpnRule {      .kind = "fortinet",      .needles = FORTINET_NEEDLES,        .prefixes = NO_PREFIXES },
      VpnRule {         .kind = "pulse",         .needles = PULSE_NEEDLES,        .prefixes = NO_PREFIXES },
      VpnRule {     .kind = "wireguard",     .needles = WIREGUARD_NEEDLES, .prefixes = WIREGUARD_PREFIXES },
      VpnRule {       .kind = "openvpn",       .needles = OPENVPN_NEEDLES,        .prefixes = NO_PREFIXES },
      VpnRule {          .kind = "utun",          .needles = UTUN_NEEDLES,      .prefixes = UTUN_PREFIXES },
      VpnRule {       .kind = "tun/tap",           .needles = NO_PREFIXES,   .prefixes = OPENVPN_PREFIXES },
      VpnRule {           .kind = "ppp",           .needles = PPP_NEEDLES,       .prefixes = PPP_PREFIXES },
      VpnRule {         .kind = "ipsec",         .needles = IPSEC_NEEDLES,        .prefixes = NO_PREFIXES },
    };

    for (const VpnRule& rule : RULES)
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

    // GetProcAddress exposes an untyped address by Win32 API design.
    auto* getAdaptersAddresses = reinterpret_cast<GetAdaptersAddressesFn>(GetProcAddress(ipHelper, "GetAdaptersAddresses")); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    if (getAdaptersAddresses == nullptr) {
      FreeLibrary(ipHelper);
      return {};
    }

    ULONG              bufferSize = 15 * 1024;
    Vec<unsigned char> buffer(bufferSize);

    // IP Helper expects its variable-length result in a caller-owned byte buffer.
    auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    ULONG result    = getAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST, nullptr, addresses, &bufferSize);

    if (result == ERROR_BUFFER_OVERFLOW) {
      buffer.resize(bufferSize);
      addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
      result    = getAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST, nullptr, addresses, &bufferSize);
    }

    Vec<VpnInterface> interfaces;
    if (result != NO_ERROR) {
      FreeLibrary(ipHelper);
      return interfaces;
    }

    for (IP_ADAPTER_ADDRESSES const* adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
      const String name        = adapter->AdapterName != nullptr ? String(adapter->AdapterName) : String {};
      const String description = WideToUtf8(adapter->Description);
      const bool   active      = adapter->OperStatus == IfOperStatusUp;

      Option<VpnClassification> classification = None;
      if (adapter->IfType == IF_TYPE_TUNNEL)
        classification = MakeClassification("tunnel");
      else if (adapter->IfType == IF_TYPE_PPP)
        classification = MakeClassification("ppp");
      else
        classification = ClassifyVpnInterface(name, description);

      if (classification)
        interfaces.push_back({
          .name        = name,
          .kind        = classification->kind,
          .displayName = classification->displayName,
          .active      = active,
        });
    }

    FreeLibrary(ipHelper);
    return interfaces;
  }
#elifdef __linux__
  auto ReadInterfaceFlags(const std::filesystem::path& interfacePath) -> unsigned int {
    std::ifstream input(interfacePath / "flags");
    if (!input)
      return 0;

    String value;
    input >> value;

    try {
      return static_cast<unsigned int>(std::stoul(value, nullptr, 0));
    } catch (...) {
      return 0;
    }
  }

  auto CollectVpnInterfaces() -> Vec<VpnInterface> {
    Vec<VpnInterface>           interfaces;
    const std::filesystem::path netDir { "/sys/class/net" };

    std::error_code errc;
    if (!std::filesystem::exists(netDir, errc))
      return interfaces;

    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(netDir, errc)) {
      if (errc)
        break;

      const String name = entry.path().filename().string();
      if (name.empty())
        continue;

      const unsigned int flags = ReadInterfaceFlags(entry.path());
      if ((flags & IFF_LOOPBACK) != 0)
        continue;

      if (auto classification = ClassifyVpnInterface(name))
        interfaces.push_back({
          .name        = name,
          .kind        = classification->kind,
          .displayName = classification->displayName,
          .active      = (flags & IFF_UP) != 0,
        });
    }

    return interfaces;
  }
#else
  auto CollectVpnInterfaces() -> Vec<VpnInterface> {
    ifaddrs*          addrs = nullptr;
    Vec<VpnInterface> interfaces;

    if (getifaddrs(&addrs) != 0 || addrs == nullptr)
      return interfaces;

    for (ifaddrs* iter = addrs; iter != nullptr; iter = iter->ifa_next) {
      if (iter->ifa_name == nullptr)
        continue;

      const unsigned int flags = iter->ifa_flags;
      if ((flags & IFF_LOOPBACK) != 0)
        continue;

      const String name(iter->ifa_name);
      if (std::ranges::any_of(interfaces, [&name](const VpnInterface& iface) { return iface.name == name; }))
        continue;

      if (auto classification = ClassifyVpnInterface(name))
        interfaces.push_back({
          .name        = name,
          .kind        = classification->kind,
          .displayName = classification->displayName,
          .active      = (flags & IFF_UP) != 0,
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
        .description  = "Detects VPN-like network interfaces",
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
      m_data.active     = std::ranges::any_of(m_data.interfaces, [](const VpnInterface& iface) -> bool { return iface.active; });
      m_lastError       = None;
      return {};
    }

    [[nodiscard]] auto getFields() const -> PluginFields override {
      const auto primary = std::ranges::find_if(m_data.interfaces, [](const VpnInterface& iface) -> bool { return iface.active; });

      PluginFieldObject interfaces;
      for (const VpnInterface& iface : m_data.interfaces) {
        interfaces.emplace(
          iface.name,
          PluginFieldObject {
            {       "active",                                                      iface.active },
            { "display_name",                                                 iface.displayName },
            {         "kind",                                                        iface.kind },
            {      "primary", primary != m_data.interfaces.end() && iface.name == primary->name },
        }
        );
      }

      return {
        {     "active",         m_data.active },
        { "interfaces", std::move(interfaces) },
      };
    }

    [[nodiscard]] auto getDisplayValue() const -> Result<String> override {
      if (!m_data.active)
        ERR(NotFound, "No active VPN interface found");

      auto primary = std::ranges::find_if(m_data.interfaces, [](const VpnInterface& iface) -> bool { return iface.active; });
      if (primary == m_data.interfaces.end())
        ERR(NotFound, "No active VPN interface found");

      return primary->displayName;
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
