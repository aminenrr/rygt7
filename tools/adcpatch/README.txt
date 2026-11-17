adcpatch - rewrites GT Sport (v1.69) AdHoc scripts for ryGT's offline features

Patched scripts go in <game folder>\patches\scripts\ (same file name as the game's); the renderer
(GTS-005) serves them in place of the originals. Do not distribute the patched .ADC files: they are
built from Polyphony's scripts. adcpatch does not read or decrypt the game's files: you give it
the original script files yourself.

  <mode> <original .ADC> <patched .ADC> [.ad source]

  uikit      PROJECTS\GT7SP\UI_KIT\UI_KIT.ADC      openGrim.ad   offline livery "upload" (apply)
  livery2    PROJECTS\GT7SP\LIVERY2\LIVERY2.ADC                  Save takes the upload path; user
                                                                 decals allowed offline
  thumbnail  PROJECTS\GT7SP\THUMBNAIL\THUMBNAIL.ADC              no 10/32 KB thumbnail size floor
  gallery    PROJECTS\GT7SP\GALLERY\GALLERY.ADC                  "My Decals" listed offline
  grim       SCRIPTS\GT7SP\GRIM\GRIM.ADC           grim.ad       decal list from <game>\decals\*.svg

Build: clone https://github.com/Nenkai/GTAdhocToolchain (3fecc2e) next to this folder as
..\GTAdhocToolchain, apply GTAdhocToolchain-exact-roundtrip.patch (symbol ids by name, so an
unchanged script round-trips byte for byte), then
`dotnet publish -c Release -r win-x64 --self-contained -p:PublishSingleFile=true`.
