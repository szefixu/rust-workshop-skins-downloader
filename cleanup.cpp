/*
 * Workshop Instance Cleanup & Merge Tool
 *
 * Run this after stopping the downloader early (or any time) to:
 *   1. Move all successfully-downloaded skins from instances/rust_workshop_tN
 *      into the main rust_workshop content folder.
 *   2. Wipe steamcmd staging / partial download files from every instance dir.
 *   3. Remove leftover .patch and .lock files from the shared workshop dir.
 *   4. Delete each instance/rust_workshop_tN directory once it is empty.
 *   5. Remove the instances/ folder itself if it is fully empty.
 *   6. Clean up the temp_scripts folder.
 *
 * Build (MSVC):  cl /std:c++17 /O2 cleanup.cpp /Fe:cleanup.exe
 * Build (MinGW): g++ -std=c++17 -O2 cleanup.cpp -o cleanup.exe
 */

#include <iostream>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

// =============================================================================
//  CONFIGURATION  -- must match values in workshop_downloader.cpp
// =============================================================================
const std::string APP_ID          = "252490";
const std::string SHARED_DIR      = "rust_workshop";
const std::string CONTENT_PATH    = SHARED_DIR + "/steamapps/workshop/content/" + APP_ID;
const std::string INSTANCES_ROOT  = "instances";        // subfolder that holds all instance dirs
const std::string INST_DIR_PREFIX = "rust_workshop_t";  // matched inside INSTANCES_ROOT
const std::string TEMP_DIR        = "temp_scripts";

// Subdirs inside each instance dir that hold partial / staged downloads
const std::vector<std::string> STAGING_SUBDIRS = {
    "steamapps/workshop/downloads",
    "steamapps/workshop/temp",
    "steamapps/downloading",
};

// =============================================================================
//  ANSI COLOURS
// =============================================================================
namespace Col {
    const char* Reset   = "\033[0m";
    const char* Green   = "\033[32m";
    const char* Yellow  = "\033[33m";
    const char* Red     = "\033[31m";
    const char* Cyan    = "\033[36m";
    const char* Magenta = "\033[35m";
    const char* Bold    = "\033[1m";
}

#ifdef _WIN32
#include <windows.h>
static void enableAnsi() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    GetConsoleMode(h, &mode);
    SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}
#else
static void enableAnsi() {}
#endif

// =============================================================================
//  HELPERS
// =============================================================================
static std::string ts() {
    auto t  = std::time(nullptr);
    auto tm = *std::localtime(&t);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%H:%M:%S");
    return ss.str();
}

static void log(const std::string& msg, const char* col = Col::Reset) {
    std::cout << col << "[" << ts() << "] " << msg << Col::Reset << "\n";
}

static bool folderHasFiles(const fs::path& p) {
    try {
        if (!fs::exists(p) || !fs::is_directory(p)) return false;
        for (auto& e : fs::directory_iterator(p))
            if (fs::is_regular_file(e) && fs::file_size(e) > 0) return true;
    } catch (...) {}
    return false;
}

// True when the directory contains zero regular files at any depth.
static bool dirIsEmpty(const fs::path& p) {
    try {
        if (!fs::exists(p) || !fs::is_directory(p)) return true;
        for (auto& e : fs::recursive_directory_iterator(p))
            if (fs::is_regular_file(e)) return false;
        return true;
    } catch (...) {}
    return false;
}

// =============================================================================
//  STEP 1 -- Discover all instance directories inside INSTANCES_ROOT
// =============================================================================
static std::vector<fs::path> findInstanceDirs() {
    std::vector<fs::path> found;

    if (!fs::exists(INSTANCES_ROOT)) {
        log("No '" + INSTANCES_ROOT + "/' folder found -- nothing to process.", Col::Yellow);
        return found;
    }

    try {
        for (auto& entry : fs::directory_iterator(INSTANCES_ROOT)) {
            if (!entry.is_directory()) continue;
            std::string name = entry.path().filename().string();
            // Match rust_workshop_tN where N is one or more digits
            if (name.rfind(INST_DIR_PREFIX, 0) != 0) continue;
            std::string suffix = name.substr(INST_DIR_PREFIX.size());
            if (!suffix.empty() && std::all_of(suffix.begin(), suffix.end(), ::isdigit))
                found.push_back(entry.path());
        }
    } catch (const std::exception& ex) {
        log("ERROR scanning '" + INSTANCES_ROOT + "/': " + ex.what(), Col::Red);
    }

    std::sort(found.begin(), found.end());
    return found;
}

// =============================================================================
//  STEP 2 -- Wipe staging folders inside one instance dir
// =============================================================================
static int cleanStaging(const fs::path& instanceDir) {
    int removed = 0;
    for (const auto& sub : STAGING_SUBDIRS) {
        fs::path p = instanceDir / sub;
        if (!fs::exists(p)) continue;
        try {
            for (auto& entry : fs::directory_iterator(p))
                removed += (int)fs::remove_all(entry);
        } catch (const std::exception& ex) {
            log("  WARN: could not clean " + p.string() + ": " + ex.what(), Col::Yellow);
        }
    }
    return removed;
}

// =============================================================================
//  STEP 3 -- Move skins from one instance dir into the shared content path
// =============================================================================
struct MoveResult {
    int moved   = 0;   // skins successfully moved
    int already = 0;   // skins already present in shared dir (skipped)
    int failed  = 0;   // skins that could not be moved
};

static MoveResult moveSkinsFromInstance(const fs::path& instanceDir) {
    MoveResult r;
    fs::path srcContent = instanceDir / "steamapps" / "workshop" / "content" / APP_ID;

    if (!fs::exists(srcContent)) return r;

    try {
        for (auto& entry : fs::directory_iterator(srcContent)) {
            if (!entry.is_directory()) continue;
            std::string skinId = entry.path().filename().string();

            // Skin IDs are purely numeric
            if (!std::all_of(skinId.begin(), skinId.end(), ::isdigit)) continue;

            fs::path dst = fs::path(CONTENT_PATH) / skinId;

            // Already in shared dir -- remove duplicate and skip
            if (folderHasFiles(dst)) {
                r.already++;
                try { fs::remove_all(entry.path()); } catch (...) {}
                continue;
            }

            // Attempt fast rename (same filesystem)
            try {
                fs::create_directories(dst.parent_path());
                fs::rename(entry.path(), dst);
                if (folderHasFiles(dst)) {
                    r.moved++;
                } else {
                    r.failed++;
                    log("  WARN: rename succeeded but dst is empty: " + skinId, Col::Yellow);
                }
            } catch (...) {
                // Cross-device fallback: recursive copy then remove source
                try {
                    fs::copy(entry.path(), dst,
                             fs::copy_options::recursive |
                             fs::copy_options::overwrite_existing);
                    fs::remove_all(entry.path());
                    if (folderHasFiles(dst))
                        r.moved++;
                    else
                        r.failed++;
                } catch (const std::exception& ex) {
                    log("  ERROR: could not move skin " + skinId + ": " + ex.what(), Col::Red);
                    r.failed++;
                }
            }
        }
    } catch (const std::exception& ex) {
        log("  ERROR iterating " + srcContent.string() + ": " + ex.what(), Col::Red);
    }
    return r;
}

// =============================================================================
//  STEP 4 -- Remove stale .patch / .lock files from the shared workshop dir
// =============================================================================
static int cleanSharedLocks() {
    int removed = 0;
    fs::path dl = fs::path(SHARED_DIR) / "steamapps" / "workshop" / "downloads";
    if (!fs::exists(dl)) return 0;
    try {
        for (auto& entry : fs::directory_iterator(dl)) {
            std::string ext = entry.path().extension().string();
            if (ext == ".patch" || ext == ".lock") {
                try { fs::remove(entry); removed++; } catch (...) {}
            }
        }
    } catch (...) {}
    return removed;
}

// =============================================================================
//  STEP 5 -- Remove a directory if it is empty
// =============================================================================
static bool tryRemoveDir(const fs::path& dir, bool verbose = true) {
    if (!dirIsEmpty(dir)) return false;
    try {
        fs::remove_all(dir);
        if (verbose)
            log("  Removed " + dir.string() + "/", Col::Cyan);
        return true;
    } catch (const std::exception& ex) {
        log("  WARN: could not remove " + dir.string() + ": " + ex.what(), Col::Yellow);
        return false;
    }
}

// =============================================================================
//  STEP 6 -- Clean temp_scripts folder
// =============================================================================
static void cleanTempDir() {
    if (!fs::exists(TEMP_DIR)) return;
    try {
        fs::remove_all(TEMP_DIR);
        log("Removed " + TEMP_DIR + "/", Col::Cyan);
    } catch (const std::exception& ex) {
        log("WARN: could not remove " + TEMP_DIR + ": " + ex.what(), Col::Yellow);
    }
}

// =============================================================================
//  MAIN
// =============================================================================
int main() {
    enableAnsi();

    std::cout << Col::Bold << Col::Cyan
        << "+------------------------------------------------------+\n"
        << "|     Workshop Cleanup & Merge Tool                    |\n"
        << "|  instances/rust_workshop_tN  -->  rust_workshop      |\n"
        << "+------------------------------------------------------+\n"
        << Col::Reset << "\n";

    // Ensure shared content destination exists
    try { fs::create_directories(CONTENT_PATH); } catch (...) {}

    // -- Discover instance dirs -------------------------------------------
    auto instances = findInstanceDirs();
    if (instances.empty()) {
        // findInstanceDirs already printed a message if the root was missing
        if (fs::exists(INSTANCES_ROOT))
            log("No matching instance directories found inside '" + INSTANCES_ROOT + "/'.",
                Col::Yellow);
    } else {
        log("Found " + std::to_string(instances.size()) + " instance director"
            + (instances.size() == 1 ? "y" : "ies") + " in '" + INSTANCES_ROOT + "/':",
            Col::Cyan);
        for (auto& d : instances)
            std::cout << "  " << d.filename().string() << "\n";
        std::cout << "\n";
    }

    // -- Counters ---------------------------------------------------------
    int totalMoved       = 0;
    int totalAlready     = 0;
    int totalFailed      = 0;
    int totalDirsRemoved = 0;
    int totalStaging     = 0;

    // -- Process each instance dir ----------------------------------------
    for (auto& instDir : instances) {
        std::string name = instDir.filename().string();
        log("-- Processing " + name + " --", Col::Bold);

        // 1. Wipe staging files (partial downloads)
        int stagingRemoved = cleanStaging(instDir);
        if (stagingRemoved > 0) {
            log("  Removed " + std::to_string(stagingRemoved) + " staging file(s).", Col::Magenta);
            totalStaging += stagingRemoved;
        }

        // 2. Move skins to shared rust_workshop
        MoveResult mr = moveSkinsFromInstance(instDir);
        totalMoved   += mr.moved;
        totalAlready += mr.already;
        totalFailed  += mr.failed;

        std::string summary = "  Skins moved: " + std::to_string(mr.moved);
        if (mr.already > 0) summary += "  |  already present (skipped): " + std::to_string(mr.already);
        if (mr.failed  > 0) summary += "  |  FAILED: " + std::to_string(mr.failed);
        log(summary, mr.failed > 0 ? Col::Red : Col::Green);

        // 3. Remove instance dir if now empty
        if (tryRemoveDir(instDir)) {
            log("  Removed instances/" + name + "/", Col::Cyan);
            totalDirsRemoved++;
        } else {
            log("  Kept instances/" + name + "/ (not empty -- manual check recommended)",
                Col::Yellow);
            // List remaining files so the user knows what is still there
            try {
                for (auto& e : fs::recursive_directory_iterator(instDir))
                    if (fs::is_regular_file(e))
                        std::cout << "    " << fs::relative(e.path(), instDir).string() << "\n";
            } catch (...) {}
        }

        std::cout << "\n";
    }

    // -- Try to remove the instances/ root if it is now empty -------------
    if (fs::exists(INSTANCES_ROOT) && dirIsEmpty(INSTANCES_ROOT)) {
        if (tryRemoveDir(INSTANCES_ROOT, false))
            log("Removed empty '" + INSTANCES_ROOT + "/' folder.", Col::Cyan);
    }

    // -- Clean shared .patch / .lock files --------------------------------
    int locksRemoved = cleanSharedLocks();
    if (locksRemoved > 0)
        log("Removed " + std::to_string(locksRemoved)
            + " stale .patch/.lock file(s) from shared workshop dir.", Col::Magenta);

    // -- Clean temp_scripts -----------------------------------------------
    cleanTempDir();

    // -- Final summary ----------------------------------------------------
    std::cout << Col::Bold
        << "-------------------- Summary ------------------------\n" << Col::Reset;
    std::cout << Col::Green  << "  Skins moved to rust_workshop:  " << totalMoved       << "\n" << Col::Reset;
    std::cout << Col::Yellow << "  Already present (skipped):     " << totalAlready     << "\n" << Col::Reset;
    if (totalFailed > 0)
        std::cout << Col::Red << "  Failed to move:                " << totalFailed     << "\n" << Col::Reset;
    std::cout << Col::Cyan   << "  Instance dirs removed:         " << totalDirsRemoved
              << " / " << instances.size()                                               << "\n" << Col::Reset;
    if (locksRemoved > 0)
        std::cout << Col::Magenta << "  Stale lock files removed:      " << locksRemoved << "\n" << Col::Reset;
    if (totalStaging > 0)
        std::cout << Col::Magenta << "  Staging files removed:         " << totalStaging  << "\n" << Col::Reset;
    std::cout << Col::Bold
        << "-----------------------------------------------------\n" << Col::Reset;

    if (totalFailed > 0) {
        std::cout << Col::Yellow
            << "\nSome skins could not be moved. Instance directories that still\n"
            << "contain files were kept so you can inspect them manually.\n"
            << Col::Reset;
    }

    std::cout << "\nPress Enter to exit...";
    std::cin.get();
    return totalFailed > 0 ? 1 : 0;
}