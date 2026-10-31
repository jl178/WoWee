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
