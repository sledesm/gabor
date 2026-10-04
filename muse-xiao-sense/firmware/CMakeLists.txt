# Muse gadget firmware for the Seeed XIAO nRF52840 Sense (Zephyr).
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(muse_xiao_sense LANGUAGES C CXX)

target_sources(app PRIVATE
  src/main.c
  src/mem.c
  src/identity.c
  src/store.c
  src/b64url.c
  src/ble_framing.c
  src/pairing.c
  src/setup.c
  src/net.c
  src/https.c
  src/muse_api.c
  src/link.cpp
  src/service.c
  src/commands.c
  lib/cjson/cJSON.c
)
target_sources_ifdef(CONFIG_BT app PRIVATE src/ble_gatt.c)
target_sources_ifdef(CONFIG_MUSE_SETUP_OVER_TCP app PRIVATE src/setup_tcp.c)
target_sources_ifdef(CONFIG_MUSE_BOARD_XIAO app PRIVATE src/board_xiao.c)
target_sources_ifndef(CONFIG_MUSE_BOARD_XIAO app PRIVATE src/board_sim.c)

# The Muse Gadget SDK's own Noise client core, unchanged, on PSA Crypto.
target_sources(app PRIVATE
  lib/noise_core/src/ClientSession.cpp
  lib/noise_core/src/InitiatorHandshake.cpp
  lib/noise_core/src/PsaCryptoBackend.cpp
  lib/noise_core/src/ServiceCodec.cpp
  lib/noise_core/src/Status.cpp
  lib/noise_core/src/Transport.cpp
  lib/noise_core/src/TransportFrameCodec.cpp
)
target_include_directories(app PRIVATE src lib/noise_core/include lib/cjson)
zephyr_include_directories(src)

# The root CA the Muse endpoints chain to (DigiCert Global Root G2).
generate_inc_file_for_target(app ${CMAKE_CURRENT_SOURCE_DIR}/${CONFIG_MUSE_ROOT_CA_FILE}
  ${ZEPHYR_BINARY_DIR}/include/generated/muse_root_ca.der.inc)
