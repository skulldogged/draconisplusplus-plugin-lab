{
  description = "Personal external plugins for Draconis++";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    utils.url = "github:numtide/flake-utils";
  };

  outputs = {
    self,
    nixpkgs,
    utils,
    ...
  }: let
    # Keep this list in sync with the plugin directories in the repository.
    # Each name should be a direct child directory with a plugin.json manifest.
    pluginNames = [
      "container_info"
      "vpn_info"
    ];
  in
    {
      lib = {
        inherit pluginNames;
      };
    }
    // utils.lib.eachDefaultSystem (
      system: let
        pkgs = import nixpkgs {
          inherit system;
        };

        pluginBuildInputs = with pkgs.pkgsStatic; [
          curl
          grpc
          protobuf
        ];

        escapeCppString =
          value:
            builtins.replaceStrings
            ["\\" "\""]
            ["\\\\" "\\\""]
            (toString value);

        containerInfoConfigHeader = containerInfo: let
          backends = containerInfo.backends or ["all"];
          backendCount = builtins.length backends;
          backendValues = builtins.concatStringsSep ", " (map (backend: "\"${escapeCppString backend}\"") backends);
        in ''
          #pragma once

          #include <array>

          namespace draconis::config {
            inline constexpr std::array<const char*, ${toString backendCount}> CONTAINER_INFO_BACKENDS = {
              ${backendValues}
            };
          } // namespace draconis::config
        '';

        mkPluginRoot = {
          # By default, package every plugin in this repository. Pass a smaller
          # list when you want a package containing only selected plugins.
          names ? pluginNames,
          containerInfo ? null,
        }:
          pkgs.stdenvNoCC.mkDerivation {
            pname = "draconisplusplus-plugin-lab";
            version = "0.1.0";
            src = self;

            dontConfigure = true;
            dontBuild = true;
            passthru = {
              inherit pluginBuildInputs;
            };

            # A plugin package is just a plugin root: a directory whose direct
            # children are plugin directories. Draconis++ receives this path via
            # -Dplugin_dirs= and compiles/loads the plugins itself.
            installPhase = ''
              runHook preInstall
              mkdir -p "$out"
            ''
            + builtins.concatStringsSep "\n" (map (name: ''
              cp -R "${name}" "$out/${name}"
            '') names)
            + nixpkgs.lib.optionalString (containerInfo != null && builtins.elem "container_info" names) ''
              cp ${pkgs.writeText "container-info-config.hpp" (containerInfoConfigHeader containerInfo)} "$out/container_info/config.hpp"
            ''
            + ''
              runHook postInstall
            '';
          };

        allPlugins = mkPluginRoot {};
      in {
        lib = {
          inherit mkPluginRoot pluginBuildInputs;
        };

        packages =
          {
            all = allPlugins;
            default = allPlugins;
          }
          // builtins.listToAttrs (map (name: {
            inherit name;
            value = mkPluginRoot {names = [name];};
          }) pluginNames);

        checks = self.packages.${system};
      }
    );
}
