{
  description = "Wowee development shell: native build on NixOS and other Nix systems";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [
            cmake
            ninja
            pkg-config
            python3
            # Required
            sdl3
            vulkan-loader
            vulkan-headers
            shaderc          # glslc, compiles the shaders CMake does not find prebuilt
            glm
            openssl
            zlib
            ffmpeg           # cinematics
            libx11           # the ungrab path in main.cpp links X11 on Linux
            # Optional, enable Warden execution and the asset tools
            unicorn
            stormlib
          ];

          # tools/clang_warning_check.py (the clang_warning_check ctest) parses
          # the sources with clang, which the Windows/macOS/Android builds use
          # and which warns where GCC cannot. A path, not a package: clang in
          # `packages` would set CC/CXX and switch this build off GCC.
          WOWEE_CLANGXX = "${pkgs.clang}/bin/clang++";

          # Vulkan on NixOS: the loader finds ICDs through the system driver path.
          VK_ICD_FILENAMES = "";
          shellHook = ''
            export VULKAN_SDK="${pkgs.vulkan-loader}"
            unset VK_ICD_FILENAMES
          '';
        };
      });
    };
}
