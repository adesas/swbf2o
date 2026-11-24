#pragma once
/* Platform shim: replaces the engine's MemoryPool allocator with the WASM heap.
 * The decompiled tree only needs Lua_Red_realloc / Lua_Red_Free + LuaMemory::CreatePools. */
#include <stddef.h>
namespace LuaMemory { void CreatePools(bool bMission); void Reset(); }
void Lua_Red_Free(void* pBlock, unsigned int uiSize);
void* Lua_Red_realloc(void* pBlock, unsigned int uiOldSize, unsigned int uiNewSize);
void LuaScript_CallWARNINGF(const char* msg);
