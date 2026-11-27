# Pure-logic unit tests -- no game, no minhook, no linking against mgmp.dll.
# Each covers a piece of the mod that can only otherwise be exercised in-game
# (a live desync, an AstroZombie fight), so the branch logic gets tested here
# and the in-game run is left to prove only the parts that need a game. See
# the header comment in each .cpp for which hook/wire-format it pins.

add_executable(test_proto test_proto.cpp)
target_include_directories(test_proto PRIVATE ${CMAKE_SOURCE_DIR}/src/net)
add_test(NAME test_proto COMMAND test_proto)

add_executable(test_peerlog test_peerlog.cpp
    ${CMAKE_SOURCE_DIR}/src/session/mgmp_peerlog.cpp
    ${CMAKE_SOURCE_DIR}/src/core/mgmp_log.cpp)
target_include_directories(test_peerlog PRIVATE
    ${CMAKE_SOURCE_DIR}/src/session ${CMAKE_SOURCE_DIR}/src/core ${CMAKE_SOURCE_DIR}/src/net)
target_compile_definitions(test_peerlog PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
add_test(NAME test_peerlog COMMAND test_peerlog)

add_executable(test_invsync_house test_invsync_house.cpp ${CMAKE_SOURCE_DIR}/src/core/mgmp_mem.cpp)
target_include_directories(test_invsync_house PRIVATE
    ${CMAKE_SOURCE_DIR}/src/session ${CMAKE_SOURCE_DIR}/src/core ${CMAKE_SOURCE_DIR}/src/net)
target_compile_definitions(test_invsync_house PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
add_test(NAME test_invsync_house COMMAND test_invsync_house)

add_executable(test_hashring test_hashring.cpp)
target_include_directories(test_hashring PRIVATE ${CMAKE_SOURCE_DIR}/src/net)
add_test(NAME test_hashring COMMAND test_hashring)

add_executable(test_timedelay test_timedelay.cpp)
target_include_directories(test_timedelay PRIVATE ${CMAKE_SOURCE_DIR}/src/determinism)
add_test(NAME test_timedelay COMMAND test_timedelay)

add_executable(test_barrier test_barrier.cpp)
target_include_directories(test_barrier PRIVATE ${CMAKE_SOURCE_DIR}/src/session)
add_test(NAME test_barrier COMMAND test_barrier)

add_executable(test_split test_split.cpp)
target_include_directories(test_split PRIVATE ${CMAKE_SOURCE_DIR}/src/session)
add_test(NAME test_split COMMAND test_split)

add_executable(test_house_drag test_house_drag.cpp)
target_include_directories(test_house_drag PRIVATE ${CMAKE_SOURCE_DIR}/src/session)
add_test(NAME test_house_drag COMMAND test_house_drag)

add_executable(test_house_shop test_house_shop.cpp)
target_include_directories(test_house_shop PRIVATE ${CMAKE_SOURCE_DIR}/src/session)
add_test(NAME test_house_shop COMMAND test_house_shop)

add_executable(test_house_shop_live test_house_shop_live.cpp ${CMAKE_SOURCE_DIR}/src/core/mgmp_mem.cpp)
target_include_directories(test_house_shop_live PRIVATE
    ${CMAKE_SOURCE_DIR}/src/session ${CMAKE_SOURCE_DIR}/src/core ${CMAKE_SOURCE_DIR}/src/net)
target_compile_definitions(test_house_shop_live PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
add_test(NAME test_house_shop_live COMMAND test_house_shop_live)

add_executable(test_map_checkpoint test_map_checkpoint.cpp ${CMAKE_SOURCE_DIR}/src/core/mgmp_mem.cpp)
target_include_directories(test_map_checkpoint PRIVATE
    ${CMAKE_SOURCE_DIR}/src/session ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/net ${CMAKE_SOURCE_DIR}/src/determinism)
target_compile_definitions(test_map_checkpoint PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(test_map_checkpoint PRIVATE /Gy)
target_link_options(test_map_checkpoint PRIVATE /OPT:REF)
add_test(NAME test_map_checkpoint COMMAND test_map_checkpoint)

add_executable(test_rng_float test_rng_float.cpp rng_float_probe.asm
    ${CMAKE_SOURCE_DIR}/src/determinism/mgmp_rng_float.asm
    ${CMAKE_SOURCE_DIR}/src/determinism/mgmp_record.cpp
    ${CMAKE_SOURCE_DIR}/src/core/mgmp_mem.cpp
    ${CMAKE_SOURCE_DIR}/src/core/mgmp_rtti.cpp
    ${CMAKE_SOURCE_DIR}/src/core/mgmp_log.cpp)
target_include_directories(test_rng_float PRIVATE ${CMAKE_SOURCE_DIR}/src/core ${CMAKE_SOURCE_DIR}/src/determinism)
target_compile_definitions(test_rng_float PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
add_test(NAME test_rng_float COMMAND test_rng_float)

add_executable(test_night_playback test_night_playback.cpp)
target_include_directories(test_night_playback PRIVATE ${CMAKE_SOURCE_DIR}/src/session)
add_test(NAME test_night_playback COMMAND test_night_playback)

add_executable(test_music_codec test_music_codec.cpp)
target_include_directories(test_music_codec PRIVATE ${CMAKE_SOURCE_DIR}/src/session)
add_test(NAME test_music_codec COMMAND test_music_codec)

add_executable(test_sigscan test_sigscan.cpp ${CMAKE_SOURCE_DIR}/src/core/mgmp_sigscan.cpp)
target_include_directories(test_sigscan PRIVATE ${CMAKE_SOURCE_DIR}/src/core)
add_test(NAME test_sigscan COMMAND test_sigscan)

add_executable(test_resolve_live test_resolve_live.cpp ${CMAKE_SOURCE_DIR}/src/core/mgmp_sigscan.cpp)
target_include_directories(test_resolve_live PRIVATE ${CMAKE_SOURCE_DIR}/src/core)
add_test(NAME test_resolve_live COMMAND test_resolve_live)

add_executable(test_previewface test_previewface.cpp)
target_include_directories(test_previewface PRIVATE ${CMAKE_SOURCE_DIR}/src/core)
add_test(NAME test_previewface COMMAND test_previewface)

add_executable(test_spriteface test_spriteface.cpp
    ${CMAKE_SOURCE_DIR}/src/core/mgmp_spriteface.cpp
    ${CMAKE_SOURCE_DIR}/src/core/mgmp_mem.cpp)
target_include_directories(test_spriteface PRIVATE ${CMAKE_SOURCE_DIR}/src/core)
add_test(NAME test_spriteface COMMAND test_spriteface)

add_executable(test_battleid test_battleid.cpp)
target_include_directories(test_battleid PRIVATE ${CMAKE_SOURCE_DIR}/src/session)
add_test(NAME test_battleid COMMAND test_battleid)

# The one test that compiles a piece of the mod rather than a header: the config
# reader is a .cpp because it owns the Config singleton, and it is worth covering
# because it is the only parser in the project whose input a human types.
add_executable(test_config test_config.cpp ${CMAKE_SOURCE_DIR}/src/core/mgmp_config.cpp)
target_include_directories(test_config PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core ${CMAKE_SOURCE_DIR}/third_party)
target_compile_definitions(test_config PRIVATE
    WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS NOMINMAX)
add_test(NAME test_config COMMAND test_config)

# The relay tunnel, with a fake relay and real loopback sockets.
add_executable(test_tunnel test_tunnel.cpp ${CMAKE_SOURCE_DIR}/src/net/mgmp_tunnel.cpp)
target_include_directories(test_tunnel PRIVATE ${CMAKE_SOURCE_DIR}/src/net)
target_compile_definitions(test_tunnel PRIVATE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS NOMINMAX)
add_test(NAME test_tunnel COMMAND test_tunnel)

foreach(t test_proto test_hashring test_timedelay test_barrier test_split test_battleid test_config test_tunnel)
    target_compile_options(${t} PRIVATE /W3)
endforeach()
