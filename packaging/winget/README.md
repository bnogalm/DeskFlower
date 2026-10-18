# winget manifests

Manifests for the Windows Package Manager community repository
([microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs)), in the same folder layout.

For a new version: copy the last version folder, change `PackageVersion`, the installer URL, its
`InstallerSha256` and the `ProductCode` (both places; read it from the MSI's Property table), then
`winget validate --manifest <folder>` and open a pull request adding the folder to winget-pkgs
(or run `wingetcreate update bnogalm.DeskFlower --version <v> --urls <msi url> --submit`).
