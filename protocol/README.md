# Layer-shell protocol source

`wlr-layer-shell-unstable-v1.xml` is vendored so target builds are
reproducible and do not download a protocol description during a firmware
build.

It was obtained from the wlroots repository at commit
`0855cdacb2eeeff35849e2e9c4db0aa996d78d10`; its SHA-256 is
`996b8c66e1c2276c443336d4cb174634e8e5948823779042f244930525b79cc2`.
The protocol's permissive copyright notice is retained verbatim in the XML.

At build time, `wayland-scanner` generates the client header and private C
source into the build directory. Generated files are deliberately not checked
into this repository.

