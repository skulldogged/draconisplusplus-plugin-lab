{
  description = "Template for external Draconis++ plugins";

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
      "example_status"
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

        escapeCppString =
          value:
            builtins.replaceStrings
            ["\\" "\""]
            ["\\\\" "\\\""]
            (toString value);

        # This function returns the C++ header that will be copied to
        # example_status/config.hpp when users pass exampleStatus = { ... } to
        # mkPluginRoot. Add your own plugin-specific generated headers in the
        # same style.
        exampleStatusConfigHeader = exampleStatus: ''
          #pragma once

          namespace draconis::config {
            inline constexpr const char* EXAMPLE_STATUS_MESSAGE = "${escapeCppString (exampleStatus.message or "Hello from a configured external plugin")}";
          } // namespace draconis::config
        '';

        mkPluginRoot = {
          # By default, package every plugin in this repository. Pass a smaller
          # list when you want a package containing only selected plugins.
          names ? pluginNames,

          # Optional precompiled config for the example plugin. If null, no
          # config.hpp is generated and the plugin uses its runtime defaults.
          exampleStatus ? null,
        }:
          pkgs.stdenvNoCC.mkDerivation {
            pname = "draconisplusplus-plugin-template";
            version = "0.1.0";
            src = self;

            dontConfigure = true;
            dontBuild = true;

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
            + nixpkgs.lib.optionalString (exampleStatus != null) ''
              cp ${pkgs.writeText "example-status-config.hpp" (exampleStatusConfigHeader exampleStatus)} "$out/example_status/config.hpp"
            ''
            + ''
              runHook postInstall
            '';
          };

        allPlugins = mkPluginRoot {};
      in {
        lib = {
          inherit mkPluginRoot;
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
