#pragma once
#include <cstddef>
#include <cstdint>

bool SafeRead(uintptr_t address, void* out, size_t size);
bool SafeReadU32(uintptr_t address, uint32_t& out);
// Guarded write of game memory (UI state only, such as a list's scroll position).
bool SafeWriteI32(uintptr_t address, int32_t value);
// Reads a zero-terminated string of at most capacity-1 chars.
bool SafeReadCString(uintptr_t address, char* out, size_t capacity);
bool IsReadable(uintptr_t address);
