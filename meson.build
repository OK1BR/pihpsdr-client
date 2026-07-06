project('pihpsdr-client', 'c',
  version : '0.0.1',
  license : 'GPL-3.0-or-later',
  default_options : [
    'c_std=c11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

add_project_arguments('-D_GNU_SOURCE', language : 'c')

# --- Dependencies -----------------------------------------------------------
# gtk4 and opus are pulled in transitively by the vendored client_server.h
# (the protocol modules include it headless — no gtk_init). openssl provides
# SHA-512 for the password KDF; zlib decompresses spectrum frames.
gtk_dep     = dependency('gtk4')
opus_dep    = dependency('opus')
openssl_dep = dependency('openssl')   # libssl + libcrypto (SHA512)
zlib_dep    = dependency('zlib')

# Vendored piHPSDR wire contract (see vendor/pihpsdr/VENDOR.md).
vendor_inc = include_directories('vendor/pihpsdr')
src_inc    = include_directories('src')

common_deps = [gtk_dep, opus_dep, openssl_dep, zlib_dep]

# --- Milestone 1: headless RX protocol spike --------------------------------
executable('pihpsdr-spike',
  [
    'src/spike.c',
    'src/protocol.c',
    'src/transport.c',
  ],
  include_directories : [vendor_inc, src_inc],
  dependencies : common_deps,
  install : false,
)

# --- Milestone 2: GTK4 panadapter GUI ---------------------------------------
executable('pihpsdr-client-gui',
  [
    'src/gui.c',
    'src/panadapter.c',
    'src/client.c',
    'src/protocol.c',
    'src/transport.c',
  ],
  include_directories : [vendor_inc, src_inc],
  dependencies : common_deps,
  install : false,
)

# Headless render check (writes a PNG of one real frame; no window).
executable('pihpsdr-render-test',
  [
    'src/render_test.c',
    'src/panadapter.c',
    'src/client.c',
    'src/protocol.c',
    'src/transport.c',
  ],
  include_directories : [vendor_inc, src_inc],
  dependencies : common_deps,
  install : false,
)
