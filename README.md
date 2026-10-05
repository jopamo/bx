<div style="background-color:#1e1e1e; padding:1em; display:block; border-radius:8px; margin:0; text-align:center;">
  <img src="assets/bx.png" alt="logo" style="display:block; margin:0 auto;">
</div>

`bx` is a native multicall utility.

One binary, many commands.

Mira browser compatibility identities are build inputs, not browser
dependencies:

```sh
meson setup build -Dmira_chrome_version=154.0.8037.92.1 -Dmira_firefox_version=157.0
```

Only the major version is advertised. Empty values (the default) disable
the corresponding profile. Automatic selection starts with the runtime
libcurl identity, then tries enabled Chrome and Firefox profiles on eligible
403/406 responses. `-U STRING` preserves an explicit identity.

## Licensing

`bx` is a mixed-license tree. bx-owned code is GPL-2.0-or-later; imported code
keeps its upstream license. See `LICENSE`, `COPYING.GPL-2`, and per-subtree
license files for the authoritative carve-outs.
