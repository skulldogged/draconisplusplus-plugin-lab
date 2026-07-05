# Draconis++ Plugin Lab

A personal repository for external Draconis++ plugins that do not belong in core.

This repo keeps plugin development separate from Draconis++ core while still
building cleanly through the core plugin system. It includes:

- a self-contained plugin directory with `plugin.json`
- local `IInfoProviderPlugin` implementations
- a Nix flake that exposes plugin-root packages

## Layout

```text
container_info/
  container_info.cpp
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
- `packages.${system}.container_info`
- `packages.${system}.vpn_info`

Example plugin-root usage:

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

To add another plugin, create a new plugin directory, then update:

- `<plugin_name>/plugin.json`
- the C++ class and `DRAC_PLUGIN(...)` registration
- `pluginNames` in `flake.nix`

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

### `container_info`

Reports local container runtime availability and counts without invoking
container CLI tools. It checks Docker, Podman, containerd, CRI-compatible
runtimes such as CRI-O, and LXD/LXC exposed through LXD local APIs.

It reports these fields:

- `active`: true when any supported runtime has running containers
- `total_running`: total running containers across detected runtimes
- `total_containers`: total containers across detected runtimes
- `runtimes`: array of runtime objects with `id`, `display_name`, `kind`,
  `available`, `active`, `running`, `total`, `version`, `endpoint`, and
  optional `error`

Display output uses the first active runtime in priority order:
Docker, Podman, containerd, CRI, then LXD.

Example layout row:

```nix
programs.draconisplusplus.layout = [
  {
    name = "containers";
    rows = [
      { key = "plugin.container_info"; }
    ];
  }
];
```

### `vpn_info`

Detects VPN-like interfaces on Windows, macOS, Linux, FreeBSD, OpenBSD,
NetBSD, and DragonFly BSD.

It reports these fields:

- `active`: true when any detected VPN-like interface is up
- `interfaces`: object keyed by interface ID. Each value contains `display_name`,
  `kind`, `active`, and `primary`.

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
