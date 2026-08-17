#include "filebrowser.h"

#include <nds.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <dirent.h>
#include <strings.h> // strcasecmp(); POSIX, so there's no <c...> spelling of it
#include <sys/stat.h>

#include "ui.h"
#include "menu.h"
#include "nds_platform.h"

namespace {

struct FileEntry {
	char name[256];
	bool isDir;
	uint64_t sizeBytes;
};

bool HasExtensionCI(const char* name, const char* ext) {
	size_t nameLen = strlen(name);
	size_t extLen = strlen(ext);
	if (extLen > nameLen) { return false; }
	return strcasecmp(name + (nameLen - extLen), ext) == 0;
}

void PathJoin(char* dest, size_t destSize, const char* base, const char* name) {
	if (strcmp(base, "/") == 0) {
		snprintf(dest, destSize, "/%s", name);
	} else {
		snprintf(dest, destSize, "%s/%s", base, name);
	}
}

void PathUp(char* path) {
	if (strcmp(path, "/") == 0) { return; }
	char* lastSlash = strrchr(path, '/');
	if (lastSlash == path) {
		path[1] = '\0';
	} else {
		*lastSlash = '\0';
	}
}

void ListDirectory(const char* path, const char* ext, std::vector<FileEntry>& outEntries) {
	outEntries.clear();

	std::vector<FileEntry> real;
	DIR* dir = opendir(path);
	if (dir) {
		struct dirent* ent;
		while ((ent = readdir(dir)) != nullptr) {
			if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) { continue; }

			char fullPath[512];
			PathJoin(fullPath, sizeof(fullPath), path, ent->d_name);

			struct stat st;
			if (stat(fullPath, &st) != 0) { continue; }
			bool isDir = S_ISDIR(st.st_mode);
			if (!isDir && !HasExtensionCI(ent->d_name, ext)) { continue; }

			FileEntry fe;
			strncpy(fe.name, ent->d_name, sizeof(fe.name) - 1);
			fe.name[sizeof(fe.name) - 1] = '\0';
			fe.isDir = isDir;
			fe.sizeBytes = isDir ? 0 : static_cast<uint64_t>(st.st_size);
			real.push_back(fe);
		}
		closedir(dir);
	}

	std::sort(real.begin(), real.end(), [](const FileEntry& a, const FileEntry& b) {
		if (a.isDir != b.isDir) { return a.isDir; }
		return strcasecmp(a.name, b.name) < 0;
	});

	if (strcmp(path, "/") != 0) {
		FileEntry up;
		strcpy(up.name, "..");
		up.isDir = true;
		up.sizeBytes = 0;
		outEntries.push_back(up);
	}
	outEntries.insert(outEntries.end(), real.begin(), real.end());
}

void BuildBrowserEntries(const std::vector<FileEntry>& entries,
	std::vector<fb_file_entry_t>& outEntries) {
	outEntries.clear();
	outEntries.reserve(entries.size());
	for (const FileEntry& entry : entries) {
		fb_file_entry_t browserEntry;
		browserEntry.name = entry.name;
		browserEntry.kind = strcmp(entry.name, "..") == 0 ? FB_FILE_ENTRY_PARENT :
			entry.isDir ? FB_FILE_ENTRY_DIRECTORY : FB_FILE_ENTRY_FILE;
		browserEntry.size_bytes = entry.sizeBytes;
		outEntries.push_back(browserEntry);
	}
}

void RenderSelectionContext(const char* currentPath, const fb_file_browser_t& browser,
	size_t expectedSize) {
	DrawHeader(BOTTOM_SCREEN, "Write image");
	const fb_cell_rect_t &content = UiPageRegions().content;
	const int detailRow = content.row + 1;

	const fb_file_entry_t *selected = fb_file_browser_selected(&browser);
	if (!selected) {
		fb_draw_status(BOTTOM_SCREEN, detailRow, FB_STATUS_INFO, fb_theme()->info,
			fb_theme()->bg, "No image selected");
		return;
	}

	if (selected->kind == FB_FILE_ENTRY_PARENT) {
		fb_draw_status(BOTTOM_SCREEN, detailRow, FB_STATUS_INFO, fb_theme()->info,
			fb_theme()->bg, "Return to the parent folder");
		fb_draw_path_bar(BOTTOM_SCREEN, detailRow + 2, fb_theme()->secondary,
			fb_theme()->bg, currentPath);
		return;
	}

	char fullPath[512];
	PathJoin(fullPath, sizeof(fullPath), currentPath, selected->name);
	if (selected->kind == FB_FILE_ENTRY_DIRECTORY) {
		fb_draw_status(BOTTOM_SCREEN, detailRow, FB_STATUS_INFO, fb_theme()->info,
			fb_theme()->bg, "Open this folder");
		fb_draw_path_bar(BOTTOM_SCREEN, detailRow + 2, fb_theme()->secondary,
			fb_theme()->bg, fullPath);
		return;
	}

	char sizeLine[80];
	snprintf(sizeLine, sizeof(sizeLine), "Image: %llu bytes",
		static_cast<unsigned long long>(selected->size_bytes));
	const fb_status_t compatibility =
		selected->size_bytes < expectedSize ? FB_STATUS_ERROR :
		selected->size_bytes == expectedSize ? FB_STATUS_SUCCESS : FB_STATUS_WARNING;
	const uint16_t compatibilityColor =
		selected->size_bytes < expectedSize ? fb_theme()->danger :
		selected->size_bytes == expectedSize ? fb_theme()->good : fb_theme()->warn;
	const char *const compatibilityMessage =
		selected->size_bytes < expectedSize ? "Image is too small for this cart" :
		selected->size_bytes == expectedSize ? "Image size matches this cart" :
		"Image is larger; trailing data is ignored";
	fb_draw_path_bar(BOTTOM_SCREEN, detailRow, fb_theme()->secondary,
		fb_theme()->bg, fullPath);
	fb_draw_status(BOTTOM_SCREEN, detailRow + 2, compatibility, compatibilityColor,
		fb_theme()->bg, sizeLine);
	fb_draw_status(BOTTOM_SCREEN, detailRow + 4, compatibility, compatibilityColor,
		fb_theme()->bg, compatibilityMessage);
}

void RenderList(const char* currentPath, const fb_file_browser_t& browser,
	size_t expectedSize) {
	DrawHeader(TOP_SCREEN, "Pick a file to write");

	const fb_cell_rect_t &content = UiPageRegions().content;
	// The one-cell margin consumes one of the content region's cells.
	char pathDisplay[64];
	const int maxPathChars = std::min(content.cols - 1,
		static_cast<int>(sizeof(pathDisplay) - 1));
	int pathLen = strlen(currentPath);
	if (pathLen > maxPathChars) {
		snprintf(pathDisplay, sizeof(pathDisplay), "...%s", currentPath + pathLen - (maxPathChars - 3));
	} else {
		snprintf(pathDisplay, sizeof(pathDisplay), "%.*s", maxPathChars, currentPath);
	}
	fb_draw_wrapped_chars(TOP_SCREEN, UiContentX(1), UiContentY(0), fb_theme()->secondary, pathDisplay);

	for (unsigned row = 0; row < browser.list.rows; row++) {
		const unsigned index = browser.list.first + row;
		if (index >= browser.list.focus.count) { break; }
		fb_draw_file_browser_row(TOP_SCREEN, UiPageRegions().content.row + 1 + row,
			fb_theme()->text, fb_theme()->bg, fb_theme()->warn, fb_theme()->bg,
			&browser.entries[index], index == browser.list.focus.selected,
			FB_LIST_STYLE_CURSOR);
	}
	fb_list_scrollbar(TOP_SCREEN, FB_WIDTH - 1,
		(UiPageRegions().content.row + 1) * FB_GLYPH_H, 1,
		browser.list.rows * FB_GLYPH_H, &browser.list,
		fb_theme()->secondary, fb_theme()->accent);

	const bool hasParentEntry = browser.list.focus.count > 0 &&
		browser.entries[0].kind == FB_FILE_ENTRY_PARENT;
	const bool hasRealEntries = browser.list.focus.count > (hasParentEntry ? 1u : 0u);
	if (!hasRealEntries) {
		fb_draw_wrapped_chars(TOP_SCREEN, UiContentX(1), UiContentY(1 + (hasParentEntry ? 1 : 0)),
			fb_theme()->secondary, "No .bin files in this folder yet.");
	}

	static const fb_action_t actions[] = {
		{ FB_INPUT_A, "Select", nullptr, 0 },
		{ FB_INPUT_B, "Back", nullptr, 0 },
	};
	fb_draw_action_bar(TOP_SCREEN, UiPageRegions().footer.row, fb_theme()->text, fb_theme()->select, actions, sizeof(actions) / sizeof(actions[0]));
	RenderSelectionContext(currentPath, browser, expectedSize);
}

} // namespace

bool BrowseForFile(const char* startPath, const char* ext, size_t expectedSize,
	char* outPath, size_t outPathSize) {
	if (mount_fat() != ALL_OK) {
		fb_draw_wrapped_chars(TOP_SCREEN, UiContentX(1), UiContentY(14), fb_theme()->danger,
			"Couldn't access the SD card.\nMake sure it's inserted.");
		fb_draw_wrapped_chars(TOP_SCREEN, UiContentX(1), UiContentY(17), fb_theme()->warn,
			"Press <B> to go back.");
		WaitPress(KEY_B);
		return false;
	}

	char currentPath[512];
	strncpy(currentPath, startPath, sizeof(currentPath) - 1);
	currentPath[sizeof(currentPath) - 1] = '\0';

	std::vector<FileEntry> entries;
	ListDirectory(currentPath, ext, entries);

	std::vector<fb_file_entry_t> browserEntries;
	BuildBrowserEntries(entries, browserEntries);
	const int visibleCount = UiContentRows() - 1;
	fb_file_browser_t browser;
	fb_file_browser_init(&browser, browserEntries.data(), browserEntries.size(),
		visibleCount, 0);
	fb_input_repeat_t browserRepeat = {};
	bool dirty = true;
	bool result = false;

	while (true) {
		swiWaitForVBlank();
		if (dirty) {
			RenderList(currentPath, browser, expectedSize);
			dirty = false;
		}

		scanKeys();
		u32 keys = keysDown();
		const u32 heldKeys = keysHeld();
		const fb_input_t heldDirection = heldKeys & KEY_DOWN ? FB_INPUT_DOWN :
			heldKeys & KEY_UP ? FB_INPUT_UP : 0;

		if (fb_input_repeat_update(&browserRepeat, heldDirection, 12, 3)) {
			if (heldDirection == FB_INPUT_DOWN) {
				dirty |= fb_file_browser_move(&browser, 1);
			} else if (heldDirection == FB_INPUT_UP) {
				dirty |= fb_file_browser_move(&browser, -1);
			}
		}
		if (keys & KEY_B) {
			if (strcmp(currentPath, "/") == 0) {
				result = false;
				break;
			}
			PathUp(currentPath);
			ListDirectory(currentPath, ext, entries);
			BuildBrowserEntries(entries, browserEntries);
			fb_file_browser_init(&browser, browserEntries.data(), browserEntries.size(),
				visibleCount, 0);
			dirty = true;
		}
		if (keys & KEY_A && browser.list.focus.count != 0) {
			const fb_file_entry_t *selected = fb_file_browser_selected(&browser);
			switch (fb_file_browser_activate(&browser)) {
			case FB_FILE_BROWSER_GO_PARENT: {
				PathUp(currentPath);
				ListDirectory(currentPath, ext, entries);
				BuildBrowserEntries(entries, browserEntries);
				fb_file_browser_init(&browser, browserEntries.data(), browserEntries.size(),
					visibleCount, 0);
				dirty = true;
				break;
			}

			case FB_FILE_BROWSER_ENTER_DIRECTORY: {
				char newPath[512];
				PathJoin(newPath, sizeof(newPath), currentPath, selected->name);
				strncpy(currentPath, newPath, sizeof(currentPath) - 1);
				currentPath[sizeof(currentPath) - 1] = '\0';
				ListDirectory(currentPath, ext, entries);
				BuildBrowserEntries(entries, browserEntries);
				fb_file_browser_init(&browser, browserEntries.data(), browserEntries.size(),
					visibleCount, 0);
				dirty = true;
				break;
			}

			case FB_FILE_BROWSER_OPEN_FILE: {
				char fullPath[512];
				PathJoin(fullPath, sizeof(fullPath), currentPath, selected->name);
				snprintf(outPath, outPathSize, "%s", fullPath);
				result = true;
				break;
			}

			case FB_FILE_BROWSER_NONE:
				break;
			}
			if (result) { break; }
		}
	}

	unmount_fat();
	return result;
}
