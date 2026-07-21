{
  description = "Personal external plugins for Draconis++";

  inputs = {
    draconisplusplus.url = "github:skulldogged/draconisplusplus-monorepo";
    draconisplusplus.inputs.nixpkgs.follows = "nixpkgs";
    draconisplusplus.inputs.utils.follows = "utils";
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    utils.url = "github:numtide/flake-utils";
  };

  outputs = {
    self,
    draconisplusplus,
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

        pluginBuildInputsByName = {
          container_info = [pkgs.pkgsStatic.curl];
          vpn_info = [];
        };
        pluginBuildInputs = nixpkgs.lib.unique (nixpkgs.lib.concatMap (name: pluginBuildInputsByName.${name}) pluginNames);

        escapeCppString = value:
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
          plugins ? null,
          containerInfo ? null,
        }: let
          normalizePlugin = value:
            if builtins.isBool value
            then {
              enable = value;
              settings = null;
            }
            else {
              enable = value.enable or true;
              settings = value.settings or null;
            };
          requestedPlugins =
            if plugins == null
            then
              builtins.listToAttrs (map (name: {
                  inherit name;
                  value = {
                    enable = true;
                    settings = null;
                  };
                })
                names)
            else nixpkgs.lib.mapAttrs (_: normalizePlugin) plugins;
          selectedPlugins = nixpkgs.lib.filterAttrs (_: value: value.enable) requestedPlugins;
          selectedNames = builtins.attrNames selectedPlugins;
          unknownNames = builtins.filter (name: !(builtins.elem name pluginNames)) (builtins.attrNames requestedPlugins);
          containerInfoSettings =
            if containerInfo != null
            then containerInfo
            else selectedPlugins.container_info.settings or null;
          selectedBuildInputs = nixpkgs.lib.unique (nixpkgs.lib.concatMap (name: pluginBuildInputsByName.${name} or []) selectedNames);
        in
          assert nixpkgs.lib.assertMsg (unknownNames == []) "Unknown personal Draconis++ plugins: ${nixpkgs.lib.concatStringsSep ", " unknownNames}";
            pkgs.stdenvNoCC.mkDerivation {
              pname = "draconisplusplus-plugin-lab";
              version = "0.1.0";
              src = self;

              dontConfigure = true;
              dontBuild = true;
              passthru = {
                pluginNames = selectedNames;
                pluginBuildInputs = selectedBuildInputs;
                inherit pluginBuildInputsByName;
              };

              # A plugin package is just a plugin root: a directory whose direct
              # children are plugin directories. Draconis++ receives this path via
              # -Dplugin_dirs= and compiles/loads the plugins itself.
              installPhase =
                ''
                  runHook preInstall
                  mkdir -p "$out"
                ''
                + builtins.concatStringsSep "\n" (map (name: ''
                    cp -R "${name}" "$out/${name}"
                  '')
                  selectedNames)
                + nixpkgs.lib.optionalString (containerInfoSettings != null && builtins.elem "container_info" selectedNames) ''
                  cp ${pkgs.writeText "container-info-config.hpp" (containerInfoConfigHeader containerInfoSettings)} "$out/container_info/config.hpp"
                ''
                + ''
                  runHook postInstall
                '';
            };

        allPlugins = mkPluginRoot {};
        integrationCheck = name:
          draconisplusplus.lib.${system}.withPlugins {
            package = draconisplusplus.packages.${system}.generic;
            pluginPackages = [self.packages.${system}.${name}];
            staticPlugins = ["all"];
          };
        integrationChecks = builtins.listToAttrs (map (name: {
            name = "integration-${name}";
            value = integrationCheck name;
          })
          pluginNames);
      in {
        lib = {
          inherit mkPluginRoot pluginBuildInputs pluginBuildInputsByName;
        };

        packages =
          {
            all = allPlugins;
            default = allPlugins;
          }
          // builtins.listToAttrs (map (name: {
              inherit name;
              value = mkPluginRoot {names = [name];};
            })
            pluginNames);

        checks =
          self.packages.${system}
          // integrationChecks
          // {
            integration-all = draconisplusplus.lib.${system}.withPlugins {
              package = draconisplusplus.packages.${system}.generic;
              pluginPackages = [allPlugins];
              staticPlugins = ["all"];
            };
          };
      }
    );
}
