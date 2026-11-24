#!/usr/bin/env bash
# Link the browser host against the engine archive.
#   ./tools/build_browser.sh            -> build/browser/swbf2.{js,wasm}
#   ./tools/build_browser.sh node       -> build/browser/swbf2_node.js (headless checks)
set -euo pipefail
cd "$(dirname "$0")/.."
TARGET="${1:-browser}"

export PATH="$PWD/vendor/emsdk:$PWD/vendor/emsdk/upstream/emscripten:$PWD/vendor/emsdk/node/24.19.0_64bit/bin:$PATH"
mkdir -p build/browser

# The page's status line must state what this module actually contains, so the numbers come
# from the build itself: objects in the archive, and how many TUs were attempted.
# `|| true` matters: this script runs under set -e, and a number that cannot be counted must
# degrade to 0 rather than abort the link. AR is resolved from the toolchain because emsdk does
# not put llvm-ar on PATH.
AR="$PWD/vendor/emsdk/upstream/bin/llvm-ar"
[ -x "$AR" ] || AR=$(command -v llvm-ar || true)

# Vendored Bink1 decoder for wasm32 (build/binkdec.a) - see tools/build_binkdec_wasm.sh.
bash tools/build_binkdec_wasm.sh

OBJS=$({ "$AR" t build/core.a || true; } 2>/dev/null | wc -l | tr -d ' ')
SCOPE=$(wc -l < build/results.tsv 2>/dev/null | tr -d ' ') || SCOPE=0
OBJS=${OBJS:-0}; SCOPE=${SCOPE:-0}
echo "linking against build/core.a: $OBJS objects, $SCOPE TUs attempted"
BUILD_DEFS=(-DSWBF2_ENGINE_OBJECTS="${OBJS:-0}" -DSWBF2_ENGINE_SCOPE="${SCOPE:-0}")

EXPORTS='_main,_malloc,_free,_swbf2_init,_swbf2_frame,_swbf2_start_loop,_swbf2_probe_file,_swbf2_show_texture,_swbf2_last_report,_swbf2_load_texture,_swbf2_tex_upload,_swbf2_tex_scratch_len,_swbf2_tex_scratch_into,_swbf2_run_scripts_from,_swbf2_run_script,_swbf2_log_line,_swbf2_sim_time,_swbf2_sim_delta,_swbf2_sim_ticks,_swbf2_load_level,_swbf2_collision_meshes,_swbf2_ground_probe,_swbf2_ground_probe_result,_swbf2_terrain_height,_swbf2_frames,_swbf2_lua_calls,_swbf2_texture_size,_swbf2_texture_packed,_swbf2_gpu_luma,_swbf2_gpu_lit,_swbf2_gpu_distinct,_swbf2_gpu_center_packed,_swbf2_set_probe,_swbf2_texture_format_name,_swbf2_decode_texture,_swbf2_engine_build,_swbf2_stage_texture,_swbf2_montage,_swbf2_montage_count,_swbf2_clear_cache,_swbf2_version,_swbf2_set_3d,_swbf2_is_3d,_swbf2_scene_orbit,_swbf2_scene_fov,_swbf2_scene_drawn,_swbf2_scene_culled,_swbf2_depth_order_test,_swbf2_fbo_probe,_swbf2_build_models,_swbf2_append_trooper_model,_swbf2_append_combat_models,_swbf2_trooper_drawn,_swbf2_combat_setup,_swbf2_combat_turn_live,_swbf2_combat_active,_swbf2_combat_report,_swbf2_combat_unit_count,_swbf2_combat_unit_model_hash,_swbf2_combat_unit_matrix,_swbf2_combat_unit_alive,_swbf2_combat_unit_health,_swbf2_combat_unit_ammo,_swbf2_combat_select_weapon,_swbf2_combat_respawn_player,_swbf2_combat_respawn_targets,_swbf2_combat_set_ai_fire,_swbf2_model_selfcheck,_swbf2_set_geometry,_swbf2_geometry_on,_swbf2_model_models,_swbf2_model_segments,_swbf2_model_instances,_swbf2_model_drawn,_swbf2_model_culled,_swbf2_model_visited,_swbf2_model_placed_segments,_swbf2_model_placed_prim_total,_swbf2_model_placed_index_total,_swbf2_model_vertices,_swbf2_model_indices,_swbf2_model_indices_drawn,_swbf2_model_odd_prim_types,_swbf2_model_truncated,_swbf2_model_textures_resolved,_swbf2_model_unresolved_instances,_swbf2_model_draw_declared,_swbf2_load_class,_swbf2_spawn_soldier,_swbf2_set_viewport,_swbf2_viewport_state,_swbf2_fov_inputs,_swbf2_control_attach,_swbf2_control_ready,_swbf2_control_update,_swbf2_set_input,_swbf2_get_input,_swbf2_update,_swbf2_camera_update,_swbf2_camera_matrix,_swbf2_use_engine_camera,_swbf2_engine_camera_on,_swbf2_trooper_state,_swbf2_movement_script_length,_swbf2_movement_script,_audio_browser_load_sample,_audio_browser_load_sample_by_name,_audio_browser_last_load_status,_audio_browser_sample_hash,_audio_browser_sample_rate,_audio_browser_sample_bytes,_audio_browser_sample_channels,_audio_browser_sample_frames,_audio_browser_push_pcm,_audio_browser_drain_pcm,_audio_browser_pending_frames,_audio_browser_clear,_audio_browser_set_sink_ready,_audio_browser_sink_ready,_swbf2_vfs_write_save,_swbf2_vfs_read_save'

WGPU_EXPORTS=',_swbf2_wgpu_init,_swbf2_wgpu_is_ready,_swbf2_wgpu_has_bc,_swbf2_wgpu_error_count,_swbf2_wgpu_last_error,_swbf2_wgpu_resize,_swbf2_wgpu_debug_set_clear_colour,_swbf2_wgpu_debug_set_ps_mode,_swbf2_wgpu_debug_skip_draws,_swbf2_wgpu_debug_record_draws,_swbf2_wgpu_debug_draw_record_json,_swbf2_wgpu_debug_frame_texture_summary,_swbf2_wgpu_readback_frame,_swbf2_wgpu_draw_calls,_swbf2_wgpu_triangles,_swbf2_wgpu_skinned_draws,_swbf2_wgpu_skinned_triangles,_swbf2_wgpu_validate_all_shaders,_swbf2_engine_render_init,_swbf2_engine_render_load_tat2,_swbf2_engine_render_set_camera,_swbf2_engine_render_set_trooper_visible,_swbf2_engine_render_frame,_swbf2_engine_render_stats,_swbf2_gamestate_boot,_swbf2_gamestate_set_resolution,_swbf2_gamestate_tick,_swbf2_gamestate_launch_mission,_swbf2_gamestate_return_to_shell,_swbf2_gamestate_lua_exec,_swbf2_gamestate_status,_swbf2_gamestate_gui_probe,_swbf2_input_set_key,_swbf2_input_set_mouse_delta,_swbf2_input_set_cursor,_swbf2_input_set_gamepad,_swbf2_input_sync_state,_swbf2_input_clear_all,_swbf2_audio_set_enabled,_swbf2_audio_set_music_streaming,_swbf2_audio_set_sample_rate,_swbf2_audio_set_next_mix_frames,_swbf2_audio_pump_and_mix,_swbf2_audio_drain_stereo,_swbf2_audio_mix_pending_frames,_swbf2_audio_reset_stats,_swbf2_audio_stats,_swbf2_net_set_local_ip,_swbf2_net_push_in,_swbf2_net_host_lan,_swbf2_net_host_lan_config,_swbf2_net_set_password,_swbf2_net_find_lan,_swbf2_net_join_found,_swbf2_net_launch_match,_swbf2_net_disconnect,_swbf2_net_status'

OPT_LEVEL=(-O1)
LINK_DEBUG=(-sASSERTIONS=1 --profiling-funcs)
if [ "${SWBF2_RELEASE:-0}" = "1" ]; then
  OPT_LEVEL=(-O2 -ffp-contract=off -msimd128)
  LINK_DEBUG=(-sASSERTIONS=0)
  echo "build mode: RELEASE (-O2 -ffp-contract=off -msimd128 -sASSERTIONS=0)"
fi

COMMON=(
  "${OPT_LEVEL[@]}"
  -std=gnu++17
  # These flags are copied from tools/compile_core.sh and must stay in step with it. The reason
  # is not stylistic: spike/shim/swbf2_prefix.h contains `#define __declspec(x)`, which neutralises
  # the decompiler's `class __declspec(align(16)) PblMatrix`. Without the prefix header, -fms-extensions
  # honours that alignment instead, every class containing a PblMatrix gets different padding, and
  # the host ends up reading engine members at the wrong offsets - which is exactly how a correct
  # camera matrix came back permuted. Same headers plus different flags equals different ABI.
  -target wasm32-unknown-emscripten
  -fms-extensions
  -Wno-ignored-attributes -Wno-deprecated-non-prototype -Wno-invalid-offsetof
  -Wno-writable-strings -Wno-implicit-exception-spec-mismatch -Wno-narrowing
  -Wno-address-of-temporary -Wno-c23-extensions -Wno-unused-variable -Wno-unused-function
  -include spike/shim/swbf2_prefix.h
  -I spike/shim
  -I build/src_fixed
  -I build/src_fixed/LuaFL/include
  -I build/src_fixed/LuaFL/src
  -I browser/platform
  browser/main.cpp
  # The shared level harness: the engine's own start-up and level load, in one place, so that the
  # browser and the native oracle build run the same code against the same data.
  spike/level_harness.cpp
  # The engine-native fight: real Characters, weapons, ordnance and damage, shared with the
  # native combat oracle.
  spike/combat_harness.cpp
  browser/platform/pblfile_browser.cpp
  browser/platform/savedevice_browser.cpp
  browser/platform/sndfile_browser.cpp
  # Portable RedMi heap backend matching vendor/openswbf2's RedMemoryMi.h contract without Win32
  # HeapWalk/VirtualQuery calls.
  browser/platform/redmemory_mi_browser.cpp
  browser/platform/pccrash_browser.cpp
  # The small browser audio slice: PblFile/SoundFL bank reader + PCM queue.  The WebAudio sink
  # itself lives in browser/webaudio.mjs and is loaded by the browser host when it is wired up.
  browser/platform/audio_browser.cpp
  browser/platform/startup_browser.cpp browser/platform/pc_globals.cpp browser/platform/redambientlight_browser.cpp
  browser/platform/pcRedRenderer_browser.cpp
  # The platform seams the engine's own game loop and entity code call into: the DirectSound
  # device layer, COM, DirectInput, Steam, Bink, Galaxy, the D3DX helpers, and the symbols that
  # lived in pcMain.cpp (the Win32 entry point this build does not include).
  browser/platform/dsound_browser.cpp browser/platform/com_browser.cpp browser/platform/dinput_browser.cpp
  browser/platform/steam_browser.cpp browser/platform/bink_browser.cpp browser/platform/galaxy_browser.cpp
  browser/platform/d3dx_browser.cpp browser/platform/pcseam_browser.cpp
  browser/platform/redwarning_browser.cpp
  # The socket layer the engine's own networking code calls into. The multiplayer stack is
  # bypassed in this build, but the game loop still pulls the engine's NetSetup/NetGame objects in,
  # and those call Winsock.
  browser/platform/winsock_browser.cpp browser/platform/winsock_c_browser.cpp
)

LINK=(
  -sMODULARIZE=1
  -sEXPORT_NAME=SWBF2
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,callMain,FS,FS_createDataFile,getValue,setValue,HEAPU8,UTF8ToString
  -sALLOW_MEMORY_GROWTH=1
  -sINITIAL_MEMORY=268435456
  -sSTACK_SIZE=8388608
  "${LINK_DEBUG[@]}"
  -sINVOKE_RUN=0
  -sEXIT_RUNTIME=0
  -sENVIRONMENT=web,worker,node
  # Emscripten does not have setjmp/longjmp in libc: they are lowered by the compile step to
  # __wasm_setjmp/emscripten_longjmp and supplied by the JS runtime. LuaFL's ldo.cpp is the one
  # TU in the tree that uses them (the decompilation even names MSVC's _setjmp3), so it is
  # compiled through emcc for exactly that lowering - see tools/compile_core.sh. The link has to
  # be told the same model or those symbols are unresolvable.
  -sSUPPORT_LONGJMP=1
  # The decompiled engine is built -fno-exceptions -fno-rtti, and its objects still reference
  # function-local-static guards (__cxa_guard_acquire) and sized operator delete. Emscripten
  # does not add a C++ runtime here because nothing in the link asks for exceptions, so the
  # no-exception libc++abi is named explicitly - the same contract the engine compiles against.
  -lc++abi-noexcept
  # libc++ itself is needed as well: the engine's std::string and std::hash_map calls are
  # inline in the headers, but the parts that allocate and the hash helpers live in the library
  # (basic_string::__assign_external, __hash_memory, __next_prime, this_thread::sleep_for).
  # Without it the linker reports them missing, and they are the engine's own calls.
  -lc++
)

if [ "$TARGET" = "browser" ]; then
  COMPILER=em++
  COMMON+=(
    --use-port=emdawnwebgpu
    browser/gpu/d3d9_resources_wgpu.cpp
    browser/gpu/d3d9_ffp_wgsl.cpp
    browser/gpu/dx9_sm1_wgsl.cpp
    browser/gpu/d3d9_device_wgpu.cpp
    browser/gpu/wgpu_engine_bridge.cpp
    browser/gpu/wgpu_gamestate_bridge.cpp
  )
  LINK+=(
    --use-port=emdawnwebgpu
    -sUSE_WEBGL2=1 -sFULL_ES3=1 -sMAX_WEBGL_VERSION=2 -sMIN_WEBGL_VERSION=2
    -sEXPORTED_FUNCTIONS="${EXPORTS}${WGPU_EXPORTS}"
  )
  OUT=build/browser/swbf2.js
else
  COMPILER=em++
  LINK+=(-sEXPORTED_FUNCTIONS="$EXPORTS")
  OUT=build/browser/swbf2_node.js
fi

# The archive is linked whole, and that is not a linker convenience: the engine keeps state in
# file-scope objects with dynamic initialisers (FrontlineSceneManager's global at
# Battlefront2SceneManager.cpp:46, the memory pools, the class registries). A linker that pulls
# archive members only to satisfy undefined symbols never links those members in, their
# constructors never run, and the engine then walks a null singleton - which is how the level
# loader ended up adding entities to a scene manager at address zero. The whole-archive link is
# what makes the engine's own start-up happen.
"$COMPILER" "${COMMON[@]}" "${BUILD_DEFS[@]}" "${LINK[@]}" \
  -Wl,--whole-archive build/core.a -Wl,--no-whole-archive \
  build/binkdec.a \
  "${EXTRA_LINK[@]+"${EXTRA_LINK[@]}"}" \
  -o "$OUT"

ls -la build/browser/
