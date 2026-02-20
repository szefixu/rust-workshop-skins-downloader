/*
 * Skin Installer
 *
 * Moves downloaded skins from the local staging folder into the real
 * Steam workshop content directory, skipping any that are already there.
 *
 * Source:  .\rust_workshop\steamapps\workshop\content\252490\
 * Default: C:\Program Files (x86)\Steam\steamapps\workshop\content\252490\
 *
 * Build (MSVC):  cl /std:c++17 /O2 install_skins.cpp /Fe:install_skins.exe
 * Build (MinGW): g++ -std=c++17 -O2 install_skins.cpp -o install_skins.exe
 */

#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

// =============================================================================
//  CONFIGURATION
// =============================================================================
const std::string APP_ID      = "252490";
const std::string SOURCE_PATH = "rust_workshop/steamapps/workshop/content/" + APP_ID;
const std::string DEFAULT_DST = "C:/Program Files (x86)/Steam/steamapps/workshop/content/" + APP_ID;
const std::string LOG_FILE    = "install_log.txt";

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
    const char* White   = "\033[97m";
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
//  LOGGING  (console + file)
// =============================================================================
static std::ofstream logFile;

static std::string ts() {
    auto t  = std::time(nullptr);
    auto tm = *std::localtime(&t);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

static void log(const std::string& msg,
                const char* col  = Col::Reset,
                bool        file = true) {
    std::cout << col << "[" << ts() << "] " << msg << Col::Reset << "\n";
    if (file && logFile.is_open())
        logFile << "[" << ts() << "] " << msg << "\n";
}

// =============================================================================
//  HELPERS
// =============================================================================
static bool folderHasFiles(const fs::path& p) {
    try {
        if (!fs::exists(p) || !fs::is_directory(p)) return false;
        for (auto& e : fs::directory_iterator(p))
            if (fs::is_regular_file(e) && fs::file_size(e) > 0) return true;
    } catch (...) {}
    return false;
}

// Count total regular files under a path (for size reporting)
static uintmax_t countFiles(const fs::path& p) {
    uintmax_t n = 0;
    try {
        for (auto& e : fs::recursive_directory_iterator(p))
            if (fs::is_regular_file(e)) n++;
    } catch (...) {}
    return n;
}

// Human-readable byte size
static std::string humanSize(uintmax_t bytes) {
    const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << v << " " << units[u];
    return ss.str();
}

// Progress bar printed on a single updating line
static void printProgress(int done, int total, int moved, int skipped, int failed) {
    float pct   = total > 0 ? (done * 100.f / total) : 0.f;
    const int W = 30;
    int filled  = static_cast<int>(W * pct / 100.f);

    std::cout << "\r\033[K";
    std::cout << Col::Bold << "[";
    for (int i = 0; i < W; ++i)
        std::cout << (i < filled ? '=' : (i == filled ? '>' : ' '));
    std::cout << "] " << std::fixed << std::setprecision(1) << pct << "%  ";
    std::cout << Col::Green  << "Copied:"  << moved   << Col::Reset << "  ";
    std::cout << Col::Yellow << "Skipped:" << skipped << Col::Reset << "  ";
    if (failed > 0)
        std::cout << Col::Red << "Failed:" << failed << Col::Reset;
    std::cout.flush();
}

// =============================================================================
//  VALIDATION
// =============================================================================

// Checks that the destination looks like a real Steam workshop content folder.
// Heuristic: parent chain should contain "Steam" and "steamapps".
static bool looksLikeSteamPath(const fs::path& p) {
    fs::path cur = p;
    bool foundSteamapps = false;
    bool foundSteam     = false;
    for (int i = 0; i < 6; ++i) {
        cur = cur.parent_path();
        if (cur == cur.parent_path()) break; // reached fs root
        std::string name = cur.filename().string();
        // case-insensitive compare
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower == "steamapps")  foundSteamapps = true;
        if (lower == "steam")      foundSteam     = true;
    }
    return foundSteamapps && foundSteam;
}

// Checks that Steam itself appears to be installed at the given destination.
static bool steamInstallPresent(const fs::path& contentDir) {
    // Walk up to find Steam root (should contain steam.exe or steam on Linux)
    fs::path cur = contentDir;
    for (int i = 0; i < 8; ++i) {
        cur = cur.parent_path();
        if (cur == cur.parent_path()) break;
        if (fs::exists(cur / "steam.exe") || fs::exists(cur / "steam"))
            return true;
    }
    return false;
}

// =============================================================================
//  COPY ONE SKIN
// =============================================================================
struct CopyResult { bool ok = false; std::string error; };

static CopyResult copySkin(const fs::path& src, const fs::path& dst) {
    CopyResult r;
    try {
        fs::create_directories(dst);
        fs::copy(src, dst,
                 fs::copy_options::recursive |
                 fs::copy_options::overwrite_existing);
        // Verify at least one file landed
        if (!folderHasFiles(dst)) {
            r.error = "destination empty after copy";
            return r;
        }
        r.ok = true;
    } catch (const std::exception& ex) {
        r.error = ex.what();
    }
    return r;
}

// =============================================================================
//  MAIN
// =============================================================================
int main() {
    enableAnsi();
    logFile.open(LOG_FILE, std::ios::out | std::ios::app);

    std::cout << Col::Bold << Col::Cyan
        << "+----------------------------------------------------------+\n"
        << "|              Rust Workshop Skin Installer                |\n"
        << "|   Copies skins from local cache to Steam workshop dir    |\n"
        << "+----------------------------------------------------------+\n"
        << Col::Reset << "\n";

    logFile << "\n========== Session start: " << ts() << " ==========\n";

    // -------------------------------------------------------------------------
    //  Validate source
    // -------------------------------------------------------------------------
    if (!fs::exists(SOURCE_PATH)) {
        log("ERROR: Source folder not found:", Col::Red);
        log("  " + SOURCE_PATH, Col::Red);
        log("Make sure you run this from the same folder as the downloader.", Col::Yellow);
        std::cout << "\nPress Enter to exit...";
        std::cin.get();
        return 1;
    }

    // Collect skin IDs from source
    std::vector<fs::path> skins;
    try {
        for (auto& entry : fs::directory_iterator(SOURCE_PATH)) {
            if (!entry.is_directory()) continue;
            std::string name = entry.path().filename().string();
            if (name.empty()) continue;
            if (!std::all_of(name.begin(), name.end(), ::isdigit)) continue;
            if (folderHasFiles(entry.path()))
                skins.push_back(entry.path());
        }
    } catch (const std::exception& ex) {
        log("ERROR reading source folder: " + std::string(ex.what()), Col::Red);
        std::cout << "\nPress Enter to exit...";
        std::cin.get();
        return 1;
    }

    std::sort(skins.begin(), skins.end());

    if (skins.empty()) {
        log("No downloaded skins found in source folder:", Col::Yellow);
        log("  " + SOURCE_PATH, Col::Yellow);
        std::cout << "\nPress Enter to exit...";
        std::cin.get();
        return 0;
    }

    log("Source:  " + SOURCE_PATH, Col::Cyan);
    log("Skins found in source: " + std::to_string(skins.size()), Col::Cyan);

    // -------------------------------------------------------------------------
    //  Destination: show default, let user confirm or override
    // -------------------------------------------------------------------------
    std::string dstStr = DEFAULT_DST;

    std::cout << "\n" << Col::Yellow
              << "Destination Steam workshop folder:\n"
              << "  " << Col::White << dstStr << Col::Reset << "\n\n"
              << Col::Yellow
              << "Press Enter to use this path, or type a custom path and press Enter:\n"
              << "> " << Col::Reset;

    std::string userInput;
    std::getline(std::cin, userInput);
    if (!userInput.empty())
        dstStr = userInput;

    // Normalise slashes
    std::replace(dstStr.begin(), dstStr.end(), '\\', '/');
    fs::path dstPath = fs::path(dstStr);

    // -------------------------------------------------------------------------
    //  Validate destination
    // -------------------------------------------------------------------------
    log("Validating destination path...", Col::Cyan);

    // Check path structure looks Steam-like
    if (!looksLikeSteamPath(dstPath)) {
        log("WARNING: The destination path does not look like a Steam workshop content folder.", Col::Yellow);
        log("  Expected a path containing 'Steam' and 'steamapps'.", Col::Yellow);
        log("  Path given: " + dstPath.string(), Col::Yellow);
        std::cout << Col::Yellow
                  << "\nContinue anyway? This could overwrite non-Steam files. (y/n): "
                  << Col::Reset;
        char c; std::cin >> c; std::cin.ignore();
        if (c != 'y' && c != 'Y') {
            log("Aborted by user.", Col::Red);
            return 1;
        }
    } else {
        log("Path structure OK (contains steamapps + Steam).", Col::Green);
    }

    // Check Steam executable is findable from this path
    if (!steamInstallPresent(dstPath)) {
        log("WARNING: Could not find steam.exe near the destination path.", Col::Yellow);
        log("  Steam may not be installed at that location, or the path is wrong.", Col::Yellow);
        std::cout << Col::Yellow
                  << "\nContinue anyway? (y/n): " << Col::Reset;
        char c; std::cin >> c; std::cin.ignore();
        if (c != 'y' && c != 'Y') {
            log("Aborted by user.", Col::Red);
            return 1;
        }
    } else {
        log("Steam installation detected.", Col::Green);
    }

    // Check destination path contains the correct App ID
    if (dstPath.filename().string() != APP_ID) {
        log("WARNING: Destination folder name is '" + dstPath.filename().string()
            + "' but expected '" + APP_ID + "' (Rust App ID).", Col::Yellow);
        std::cout << Col::Yellow
                  << "\nContinue anyway? (y/n): " << Col::Reset;
        char c; std::cin >> c; std::cin.ignore();
        if (c != 'y' && c != 'Y') {
            log("Aborted by user.", Col::Red);
            return 1;
        }
    } else {
        log("App ID folder name matches (" + APP_ID + ").", Col::Green);
    }

    // Create destination if it doesn't exist yet
    try {
        fs::create_directories(dstPath);
    } catch (const std::exception& ex) {
        log("ERROR: Could not create destination folder: " + std::string(ex.what()), Col::Red);
        log("  Check that you have write permission to: " + dstPath.string(), Col::Yellow);
        std::cout << "\nPress Enter to exit...";
        std::cin.get();
        return 1;
    }

    log("Destination: " + dstPath.string(), Col::Cyan);

    // -------------------------------------------------------------------------
    //  Pre-scan: how many skins need copying vs already present
    // -------------------------------------------------------------------------
    int needCopy    = 0;
    int alreadyDone = 0;
    for (auto& skin : skins) {
        fs::path dst = dstPath / skin.filename();
        if (folderHasFiles(dst)) alreadyDone++;
        else                     needCopy++;
    }

    log("Already in Steam folder (will skip): " + std::to_string(alreadyDone), Col::Yellow);
    log("Need to copy:                        " + std::to_string(needCopy),    Col::Cyan);

    if (needCopy == 0) {
        log("All skins are already present in the Steam folder. Nothing to do.", Col::Green);
        std::cout << "\nPress Enter to exit...";
        std::cin.get();
        return 0;
    }

    std::cout << "\n";
    log("Starting copy...", Col::Cyan);
    std::cout << "\n"; // space before progress bar

    // -------------------------------------------------------------------------
    //  Copy loop
    // -------------------------------------------------------------------------
    int total   = (int)skins.size();
    int done    = 0;
    int moved   = 0;
    int skipped = 0;
    int failed  = 0;

    std::vector<std::string> failedIds;

    for (auto& skinPath : skins) {
        std::string skinId = skinPath.filename().string();
        fs::path    dst    = dstPath / skinId;

        // Skip if already present
        if (folderHasFiles(dst)) {
            skipped++;
            done++;
            printProgress(done, total, moved, skipped, failed);
            logFile << "[" << ts() << "] SKIP    " << skinId << "\n";
            continue;
        }

        // Copy
        CopyResult r = copySkin(skinPath, dst);
        done++;

        if (r.ok) {
            moved++;
            logFile << "[" << ts() << "] OK      " << skinId << "\n";
        } else {
            failed++;
            failedIds.push_back(skinId);
            // Print error on its own line above the progress bar
            std::cout << "\n";
            log("ERROR copying " + skinId + ": " + r.error, Col::Red);
            std::cout << "\n";
        }

        printProgress(done, total, moved, skipped, failed);
    }

    // Clear the progress line before printing the summary
    std::cout << "\n\n";

    // -------------------------------------------------------------------------
    //  Summary
    // -------------------------------------------------------------------------
    log("-----------------------------------------------------------", Col::Bold);
    log("Copy complete.", Col::Bold);
    log("  Copied successfully:  " + std::to_string(moved),   Col::Green);
    log("  Skipped (present):    " + std::to_string(skipped), Col::Yellow);
    if (failed > 0) {
        log("  Failed:               " + std::to_string(failed), Col::Red);
        log("  Failed skin IDs:", Col::Red);
        for (auto& id : failedIds)
            log("    " + id, Col::Red);
    }
    log("  Full log saved to:    " + LOG_FILE, Col::Cyan);
    log("-----------------------------------------------------------", Col::Bold);

    logFile << "========== Session end: " << ts()
            << " | copied=" << moved
            << " skipped=" << skipped
            << " failed="  << failed << " ==========\n";

    std::cout << "\nPress Enter to exit...";
    std::cin.get();
    return failed > 0 ? 1 : 0;
}