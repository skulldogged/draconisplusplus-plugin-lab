# Draconis++ Plugin Lab

A personal repository for external Draconis++ plugins that do not belong in core.

This repo keeps plugin development separate from Draconis++ core while still
building cleanly through the core plugin system. It includes:

- a self-contained plugin directory with `plugin.json`
- a simple `IInfoProviderPlugin`
- a Nix flake that exposes plugin-root packages
- optional per-plugin precompiled config via `example_status/config.hpp`

## Layout

```text
example_status/
  example_status.cpp
  plugin.json
vpn_info/
  vpn_info.cpp
  plugin.json
flake.nix
```

Draconis++ discovers plugins from directories that contain direct children with
a `plugin.json` manifest. This repository root is therefore a plugin root.

## Build With Draconis++

From a Draconis++ core checkout:

```bash
meson setup build -Dplugin_dirs=/path/to/draconisplusplus-plugin-lab
meson compile -C build
```

To compile the plugin statically into Draconis++:

```bash
meson setup build -Dplugin_dirs=/path/to/draconisplusplus-plugin-lab -Dstatic_plugins=vpn_info
meson compile -C build
```

## Nix

The flake exposes plugin-root packages:

- `packages.${system}.default`
- `packages.${system}.all`
- `packages.${system}.example_status`
- `packages.${system}.vpn_info`

For precompiled plugin config, use `lib.${system}.mkPluginRoot` to generate a
configured copy of this plugin root. The generated header is written to
`example_status/config.hpp`, where the plugin includes it in
`DRAC_PRECOMPILED_CONFIG` builds.

```nix
programs.draconisplusplus = {
  enable = true;
  configFormat = "hpp";
  pluginPackages = [
    (inputs.my-draconis-plugin.lib.${pkgs.system}.mkPluginRoot {
      exampleStatus = {
        message = "Built into Draconis++";
      };
    })
  ];
  staticPlugins = ["example_status"];
};
```

For a non-configured plugin root:

```nix
programs.draconisplusplus = {
  enable = true;
  pluginPackages = [
    inputs.my-draconis-plugin.packages.${pkgs.system}.all
  ];
  staticPlugins = ["vpn_info"];
};
```

## Customizing

Rename `example_status` to your plugin name, then update:

- `example_status/plugin.json`
- the C++ class and `DRAC_PLUGIN(...)` registration
- `pluginNames` in `flake.nix`
- any generated config header logic in `flake.nix`

Manifest notes:

- `name` should match the plugin directory and the implicit source file name.
- `class` must match the C++ class passed to `DRAC_PLUGIN(...)`.
- `platforms` uses Meson `host_machine.system()` names such as `linux`,
  `darwin`, and `windows`; use `["all"]` for portable plugins.
- `deps` lists external dependencies resolved by Meson, for example
  `{ "name": "libcurl", "include_type": "system", "static": true }`.

Keep plugin-specific Nix config in this repository. The core Draconis++ module
should only receive plugin roots through `pluginPackages` or `pluginDirs`.

## Included Plugins

### `vpn_info`

Detects active VPN-like interfaces on Windows, macOS, Linux, FreeBSD, OpenBSD,
NetBSD, and DragonFly BSD.

It reports these fields:

- `active`: boolean connection state
- `count`: number of detected VPN-like interfaces
- `primary`: first detected interface
- `primary_kind`: detected VPN family for the primary interface
- `primary_display`: display label for the primary interface
- `interfaces`: array of interface names
- `display_names`: array of display labels

Example layout row:

```nix
programs.draconisplusplus.layout = [
  {
    name = "network";
    rows = [
      { key = "plugin.vpn_info"; }
    ];
  }
];
```
