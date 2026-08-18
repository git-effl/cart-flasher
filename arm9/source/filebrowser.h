#pragma once

#include <cstddef>

// Lets the user navigate the SD card starting at startPath and pick a file
// whose name ends in ext (case-insensitive). The bottom screen compares each
// selected file's size to expectedSize. cartName is the selected cart family
// rendered as the top-screen H2. Returns true and fills outPath with the full
// selected path, or false if the user backed out at the root.
bool BrowseForFile(const char* startPath, const char* ext, const char* cartName,
	size_t expectedSize, char* outPath, size_t outPathSize);
