/*
 * appworkshop_252490.acf Patcher
 *
 * Scans the Steam Rust workshop content folder, reads each skin's manifest.txt
 * to extract real metadata, then inserts missing entries into both
 * "WorkshopItemsInstalled" and "WorkshopItemDetails" sections of the .acf file.
 *
 * Values written per skin:
 *   size          -- real total byte size of all files in the skin folder
 *   timeupdated   -- parsed from manifest.txt "PublishDate" (Unix timestamp)
 *                    falls back to newest file mtime if manifest.txt absent
 *   timetouched   -- current time (Steam updates this on next launch anyway)
 *   manifest      -- "0"  Steam fetches the real hash on next launch without
 *                         re-downloading files that are already on disk.
 *
 * A timestamped backup is always written before any modification.
 * Run this while Steam is CLOSED (Steam holds a write lock on .acf).
 *
 * Build (MSVC):  cl /std:c++17 /O2 patch_acf.cpp /Fe:patch_acf.exe
 * Build (MinGW): g++ -std=c++17 -O2 patch_acf.cpp -o patch_acf.exe
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <regex>

namespace fs = std::filesystem;

// =============================================================================
//  CONFIGURATION
// =============================================================================
const std::string APP_ID = "252490";

const std::string DEFAULT_CONTENT_DIR =
    "C:/Program Files (x86)/Steam/steamapps/workshop/content/" + APP_ID;

const std::string DEFAULT_ACF_PATH =
    "C:/Program Files (x86)/Steam/steamapps/workshop/appworkshop_" + APP_ID + ".acf";

const std::string LOG_FILE = "patch_acf_log.txt";

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
//  LOGGING
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
                const char* col = Col::Reset,
                bool toFile     = true) {
    std::cout << col << "[" << ts() << "] " << msg << Col::Reset << "\n";
    if (toFile && logFile.is_open())
        logFile << "[" << ts() << "] " << msg << "\n";
}

// =============================================================================
//  STRING HELPERS
// =============================================================================

// Strip leading/trailing whitespace including \r
static std::string trimStr(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Extract the content of the FIRST quoted token on a line.
// e.g.  \t"WorkshopItemsInstalled"  ->  WorkshopItemsInstalled
// e.g.  \t\t"size"\t\t"2615900"     ->  size
static std::string firstQuotedToken(const std::string& line) {
    size_t q1 = line.find('"');
    if (q1 == std::string::npos) return "";
    size_t q2 = line.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return line.substr(q1 + 1, q2 - q1 - 1);
}

static bool isAllDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), ::isdigit);
}

// =============================================================================
//  SKIN INFO  (read from disk)
// =============================================================================
struct SkinInfo {
    std::string id;
    uintmax_t   size        = 0;
    std::time_t timeupdated = 0;
    std::time_t timetouched = 0;
};

// Parse ISO-8601 string like "2025-02-04T12:09:39.8009705Z" -> time_t (UTC)
static std::time_t parseIso8601(const std::string& s) {
    std::regex re(R"((\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2}))");
    std::smatch m;
    if (!std::regex_search(s, m, re)) return 0;
    std::tm tm{};
    tm.tm_year  = std::stoi(m[1]) - 1900;
    tm.tm_mon   = std::stoi(m[2]) - 1;
    tm.tm_mday  = std::stoi(m[3]);
    tm.tm_hour  = std::stoi(m[4]);
    tm.tm_min   = std::stoi(m[5]);
    tm.tm_sec   = std::stoi(m[6]);
    tm.tm_isdst = 0;
#ifdef _WIN32
    return _mkgmtime(&tm);
#else
    return timegm(&tm);
#endif
}

// Read the PublishDate from a skin's manifest.txt
static std::time_t readManifestDate(const fs::path& skinDir) {
    fs::path manifestPath = skinDir / "manifest.txt";
    if (!fs::exists(manifestPath)) return 0;
    std::ifstream f(manifestPath);
    std::string line;
    // Pattern:  "PublishDate": "2025-02-04T12:09:39.8009705Z"
    // The )"  inside would break a raw string so we use a named delimiter.
    std::regex re(R"re("PublishDate"\s*:\s*"([^"]+)")re");
    while (std::getline(f, line)) {
        std::smatch m;
        if (std::regex_search(line, m, re))
            return parseIso8601(m[1].str());
    }
    return 0;
}

// Total byte size of all regular files in a folder (recursive)
static uintmax_t folderSize(const fs::path& p) {
    uintmax_t total = 0;
    try {
        for (auto& e : fs::recursive_directory_iterator(p))
            if (fs::is_regular_file(e))
                total += fs::file_size(e);
    } catch (...) {}
    return total;
}

// Newest file mtime in a folder (fallback when manifest.txt is absent)
static std::time_t folderNewestMtime(const fs::path& p) {
    std::time_t newest = 0;
    try {
        for (auto& e : fs::recursive_directory_iterator(p)) {
            if (!fs::is_regular_file(e)) continue;
            auto ftime = fs::last_write_time(e);
            auto sys   = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                             ftime - fs::file_time_type::clock::now()
                             + std::chrono::system_clock::now());
            std::time_t t = std::chrono::system_clock::to_time_t(sys);
            if (t > newest) newest = t;
        }
    } catch (...) {}
    return newest;
}

static bool folderHasFiles(const fs::path& p) {
    try {
        if (!fs::exists(p) || !fs::is_directory(p)) return false;
        for (auto& e : fs::directory_iterator(p))
            if (fs::is_regular_file(e) && fs::file_size(e) > 0) return true;
    } catch (...) {}
    return false;
}

static SkinInfo readSkinInfo(const fs::path& skinDir) {
    SkinInfo si;
    si.id          = skinDir.filename().string();
    si.size        = folderSize(skinDir);
    si.timetouched = std::time(nullptr);
    std::time_t mdate = readManifestDate(skinDir);
    si.timeupdated = (mdate > 0) ? mdate : folderNewestMtime(skinDir);
    return si;
}

// =============================================================================
//  ACF PARSER
//
//  The .acf (VDF) structure for AppWorkshop looks like this:
//
//  "AppWorkshop"              <- depth 0 key
//  {                          <- depth 1 opens
//      "appid"  "252490"      <- depth 1 kv  (ignored)
//      "WorkshopItemsInstalled"   <- depth 1 key  (SECTION HEADER)
//      {                      <- depth 2 opens
//          "490678544"        <- depth 2 key  (SKIN ID)
//          {                  <- depth 3 opens
//              "size" "..."   <- depth 3 kv  (ignored)
//          }                  <- depth 3 closes -> back to 2
//      }                      <- depth 2 closes -> back to 1  <-- INSERT POINT
//      "WorkshopItemDetails"  <- depth 1 key  (SECTION HEADER)
//      {                      <- depth 2 opens
//          ...
//      }                      <- depth 2 closes -> back to 1  <-- INSERT POINT
//  }                          <- depth 1 closes -> back to 0
//
//  The old parser used a relative depth that reset to 0 per section, which
//  caused it to miss the section headers because the outer AppWorkshop block
//  had already incremented the counter. This version uses absolute depth.
// =============================================================================
struct AcfInfo {
    std::unordered_set<std::string> installedIds;
    std::unordered_set<std::string> detailsIds;
    int installedCloseLineIdx = -1;
    int detailsCloseLineIdx   = -1;
};

static AcfInfo parseAcf(const std::vector<std::string>& lines) {
    AcfInfo info;

    enum class Sec { None, Installed, Details, Other };
    Sec cur   = Sec::None;
    int depth = 0;   // absolute brace depth

    for (int i = 0; i < (int)lines.size(); ++i) {
        std::string t = trimStr(lines[i]);

        if (t == "{") {
            depth++;
            continue;
        }

        if (t == "}") {
            // A closing brace at depth 2 ends a section block.
            // Record the line index so we know where to insert new entries.
            if (depth == 2) {
                if (cur == Sec::Installed) info.installedCloseLineIdx = i;
                if (cur == Sec::Details)   info.detailsCloseLineIdx   = i;
                cur = Sec::None;
            }
            if (depth > 0) depth--;
            continue;
        }

        if (t.empty() || t.front() != '"') continue;

        std::string key = firstQuotedToken(t);
        if (key.empty()) continue;

        if (depth == 1) {
            // Inside the root AppWorkshop block: section name keys
            if      (key == "WorkshopItemsInstalled") cur = Sec::Installed;
            else if (key == "WorkshopItemDetails")    cur = Sec::Details;
            else                                      cur = Sec::Other;
            continue;
        }

        if (depth == 2) {
            // Inside a section: item ID lines (pure numeric)
            if (isAllDigits(key)) {
                if (cur == Sec::Installed) info.installedIds.insert(key);
                if (cur == Sec::Details)   info.detailsIds.insert(key);
            }
            continue;
        }
        // depth >= 3: key-value pairs inside item blocks -- not needed
    }

    return info;
}

// =============================================================================
//  ACF ENTRY BUILDERS
// =============================================================================
static std::string buildInstalledEntry(const SkinInfo& s) {
    std::ostringstream o;
    o << "\t\t\"" << s.id << "\"\n"
      << "\t\t{\n"
      << "\t\t\t\"size\"\t\t\""         << s.size        << "\"\n"
      << "\t\t\t\"timeupdated\"\t\t\""  << s.timeupdated << "\"\n"
      << "\t\t\t\"manifest\"\t\t\"0\"\n"
      << "\t\t}\n";
    return o.str();
}

static std::string buildDetailsEntry(const SkinInfo& s) {
    std::ostringstream o;
    o << "\t\t\"" << s.id << "\"\n"
      << "\t\t{\n"
      << "\t\t\t\"manifest\"\t\t\"0\"\n"
      << "\t\t\t\"timeupdated\"\t\t\""        << s.timeupdated << "\"\n"
      << "\t\t\t\"timetouched\"\t\t\""        << s.timetouched << "\"\n"
      << "\t\t\t\"latest_timeupdated\"\t\t\"" << s.timeupdated << "\"\n"
      << "\t\t\t\"latest_manifest\"\t\t\"0\"\n"
      << "\t\t}\n";
    return o.str();
}

// =============================================================================
//  BACKUP
// =============================================================================
static bool backupAcf(const fs::path& acfPath) {
    auto t  = std::time(nullptr);
    auto tm = *std::localtime(&t);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y%m%d_%H%M%S");
    fs::path backup = acfPath.parent_path()
                    / (acfPath.stem().string() + "_backup_" + ss.str() + ".acf");
    try {
        fs::copy_file(acfPath, backup, fs::copy_options::overwrite_existing);
        log("Backup created: " + backup.string(), Col::Cyan);
        return true;
    } catch (const std::exception& ex) {
        log("ERROR creating backup: " + std::string(ex.what()), Col::Red);
        return false;
    }
}

// =============================================================================
//  VALIDATION HELPERS
// =============================================================================
static bool looksLikeSteamPath(const fs::path& p) {
    fs::path cur = p;
    bool hasSteamapps = false, hasSteam = false;
    for (int i = 0; i < 8; ++i) {
        cur = cur.parent_path();
        if (cur == cur.parent_path()) break;
        std::string lower = cur.filename().string();
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower == "steamapps") hasSteamapps = true;
        if (lower == "steam")     hasSteam     = true;
    }
    return hasSteamapps && hasSteam;
}

static bool confirmContinue(const std::string& prompt) {
    std::cout << Col::Yellow << prompt << " (y/n): " << Col::Reset;
    char c; std::cin >> c; std::cin.ignore(1024, '\n');
    return c == 'y' || c == 'Y';
}

// =============================================================================
//  MAIN
// =============================================================================
int main() {
    enableAnsi();
    logFile.open(LOG_FILE, std::ios::out | std::ios::app);
    logFile << "\n========== Session start: " << ts() << " ==========\n";

    std::cout << Col::Bold << Col::Cyan
        << "+----------------------------------------------------------+\n"
        << "|         appworkshop_252490.acf Patcher                   |\n"
        << "|  Reads manifest.txt per skin, inserts missing ACF entries |\n"
        << "+----------------------------------------------------------+\n"
        << Col::Reset << "\n";

    // -------------------------------------------------------------------------
    //  Path input
    // -------------------------------------------------------------------------
    auto promptPath = [](const std::string& label,
                         const std::string& def) -> std::string {
        std::cout << Col::Yellow << label << ":\n  "
                  << Col::White << def << Col::Reset << "\n"
                  << Col::Yellow
                  << "Press Enter to use this, or type a custom path: "
                  << Col::Reset;
        std::string in;
        std::getline(std::cin, in);
        if (!in.empty()) {
            std::replace(in.begin(), in.end(), '\\', '/');
            return in;
        }
        return def;
    };

    std::string contentDirStr = promptPath(
        "Steam workshop content folder (252490)", DEFAULT_CONTENT_DIR);
    std::cout << "\n";
    std::string acfPathStr = promptPath(
        "appworkshop_252490.acf path", DEFAULT_ACF_PATH);
    std::cout << "\n";

    fs::path contentDir = fs::path(contentDirStr);
    fs::path acfPath    = fs::path(acfPathStr);

    // -------------------------------------------------------------------------
    //  Validate content dir
    // -------------------------------------------------------------------------
    if (!fs::exists(contentDir)) {
        log("ERROR: Content folder not found: " + contentDir.string(), Col::Red);
        std::cout << "\nPress Enter to exit..."; std::cin.get(); return 1;
    }
    if (!looksLikeSteamPath(contentDir)) {
        log("WARNING: Path does not look like a Steam workshop folder.", Col::Yellow);
        if (!confirmContinue("Continue anyway?")) {
            log("Aborted.", Col::Red); return 1;
        }
    }
    if (contentDir.filename().string() != APP_ID) {
        log("WARNING: Folder name '" + contentDir.filename().string()
            + "' does not match App ID '" + APP_ID + "'.", Col::Yellow);
        if (!confirmContinue("Continue anyway?")) {
            log("Aborted.", Col::Red); return 1;
        }
    }

    // -------------------------------------------------------------------------
    //  Validate .acf path
    // -------------------------------------------------------------------------
    if (!fs::exists(acfPath)) {
        log("ERROR: .acf file not found: " + acfPath.string(), Col::Red);
        std::cout << "\nPress Enter to exit..."; std::cin.get(); return 1;
    }
    if (acfPath.extension() != ".acf") {
        log("WARNING: File does not have .acf extension.", Col::Yellow);
        if (!confirmContinue("Continue anyway?")) {
            log("Aborted.", Col::Red); return 1;
        }
    }

    log("Content folder : " + contentDir.string(), Col::Cyan);
    log("ACF file       : " + acfPath.string(),    Col::Cyan);

    // -------------------------------------------------------------------------
    //  Read .acf into memory (preserve original lines exactly)
    // -------------------------------------------------------------------------
    std::vector<std::string> lines;
    {
        std::ifstream f(acfPath, std::ios::binary); // binary to avoid \r mangling
        if (!f.is_open()) {
            log("ERROR: Cannot open .acf for reading.", Col::Red);
            std::cout << "\nPress Enter to exit..."; std::cin.get(); return 1;
        }
        std::string ln;
        while (std::getline(f, ln)) {
            // Strip trailing \r so trimStr comparisons work cleanly
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            lines.push_back(ln);
        }
    }
    log("ACF loaded: " + std::to_string(lines.size()) + " lines.", Col::Cyan);

    // -------------------------------------------------------------------------
    //  Parse ACF
    // -------------------------------------------------------------------------
    AcfInfo acf = parseAcf(lines);

    // Debug: report what the parser found
    log("Parser found WorkshopItemsInstalled close at line: "
        + std::to_string(acf.installedCloseLineIdx), Col::Cyan);
    log("Parser found WorkshopItemDetails close at line   : "
        + std::to_string(acf.detailsCloseLineIdx), Col::Cyan);

    if (acf.installedCloseLineIdx < 0 || acf.detailsCloseLineIdx < 0) {
        log("ERROR: Could not locate WorkshopItemsInstalled or WorkshopItemDetails "
            "sections in the .acf file.", Col::Red);
        log("Dumping first 30 lines of the file for inspection:", Col::Yellow);
        for (int i = 0; i < std::min(30, (int)lines.size()); ++i)
            log("  L" + std::to_string(i) + ": " + lines[i], Col::Yellow, false);
        std::cout << "\nPress Enter to exit..."; std::cin.get(); return 1;
    }

    log("Existing entries in WorkshopItemsInstalled : "
        + std::to_string(acf.installedIds.size()), Col::Cyan);
    log("Existing entries in WorkshopItemDetails    : "
        + std::to_string(acf.detailsIds.size()), Col::Cyan);

    // -------------------------------------------------------------------------
    //  Scan content folder for skin IDs present on disk
    // -------------------------------------------------------------------------
    std::vector<SkinInfo> toAdd;
    int skippedCount = 0;
    int emptyCount   = 0;

    try {
        std::vector<fs::directory_entry> entries;
        for (auto& e : fs::directory_iterator(contentDir))
            if (e.is_directory()) entries.push_back(e);

        std::sort(entries.begin(), entries.end(),
            [](const fs::directory_entry& a, const fs::directory_entry& b) {
                return a.path().filename().string() < b.path().filename().string();
            });

        for (auto& entry : entries) {
            std::string name = entry.path().filename().string();
            if (!isAllDigits(name)) continue;

            if (!folderHasFiles(entry.path())) {
                emptyCount++;
                log("SKIP empty : " + name, Col::Yellow);
                continue;
            }

            bool inInstalled = acf.installedIds.count(name) > 0;
            bool inDetails   = acf.detailsIds.count(name)   > 0;

            if (inInstalled && inDetails) {
                skippedCount++;
                logFile << "[" << ts() << "] PRESENT " << name << "\n";
                continue;
            }

            SkinInfo si = readSkinInfo(entry.path());

            bool hasManifest = fs::exists(entry.path() / "manifest.txt") && si.timeupdated > 0;
            logFile << "[" << ts() << "] QUEUE " << name
                    << " size=" << si.size
                    << " timeupdated=" << si.timeupdated
                    << (hasManifest ? " (from manifest.txt)" : " (from mtime)") << "\n";

            toAdd.push_back(si);
        }
    } catch (const std::exception& ex) {
        log("ERROR scanning content folder: " + std::string(ex.what()), Col::Red);
        std::cout << "\nPress Enter to exit..."; std::cin.get(); return 1;
    }

    // -------------------------------------------------------------------------
    //  Report
    // -------------------------------------------------------------------------
    log("Already in ACF (skipping) : " + std::to_string(skippedCount), Col::Yellow);
    log("Empty folders (skipping)  : " + std::to_string(emptyCount),   Col::Yellow);
    log("Missing -- will add       : " + std::to_string(toAdd.size()),
        toAdd.empty() ? Col::Green : Col::Magenta);

    if (toAdd.empty()) {
        log("ACF is already up to date. Nothing to write.", Col::Green);
        logFile << "========== Session end (no changes): " << ts() << " ==========\n";
        std::cout << "\nPress Enter to exit..."; std::cin.get(); return 0;
    }

    // Preview
    std::cout << "\n" << Col::Cyan << "First up to 5 skins to be added:\n" << Col::Reset;
    for (int i = 0; i < std::min((int)toAdd.size(), 5); ++i)
        std::cout << "  " << toAdd[i].id
                  << "  size=" << toAdd[i].size
                  << "  timeupdated=" << toAdd[i].timeupdated << "\n";
    if ((int)toAdd.size() > 5)
        std::cout << "  ... and " << (toAdd.size() - 5) << " more.\n";
    std::cout << "\n";

    if (!confirmContinue("Proceed with patching the .acf file?")) {
        log("Aborted by user.", Col::Yellow); return 0;
    }

    // -------------------------------------------------------------------------
    //  Backup
    // -------------------------------------------------------------------------
    if (!backupAcf(acfPath)) {
        if (!confirmContinue("Backup failed. Continue without backup?")) {
            log("Aborted.", Col::Red); return 1;
        }
    }

    // -------------------------------------------------------------------------
    //  Build insertion strings, then splice into lines[].
    //
    //  Strategy: insert into the section with the HIGHER line index first so
    //  that the lower index is not shifted when we do the second insertion.
    // -------------------------------------------------------------------------
    std::string installedInsert;
    std::string detailsInsert;

    for (auto& si : toAdd) {
        if (!acf.installedIds.count(si.id)) installedInsert += buildInstalledEntry(si);
        if (!acf.detailsIds.count(si.id))   detailsInsert   += buildDetailsEntry(si);
    }

    // Split a multi-line string into individual strings for vector insertion
    auto splitLines = [](const std::string& block) -> std::vector<std::string> {
        std::vector<std::string> out;
        std::istringstream ss(block);
        std::string ln;
        while (std::getline(ss, ln)) {
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            out.push_back(ln);
        }
        return out;
    };

    int instIdx = acf.installedCloseLineIdx;
    int detIdx  = acf.detailsCloseLineIdx;

    if (detIdx > instIdx) {
        // Insert details first (higher index), then installed
        if (!detailsInsert.empty()) {
            auto dLines = splitLines(detailsInsert);
            lines.insert(lines.begin() + detIdx, dLines.begin(), dLines.end());
        }
        if (!installedInsert.empty()) {
            auto iLines = splitLines(installedInsert);
            lines.insert(lines.begin() + instIdx, iLines.begin(), iLines.end());
        }
    } else {
        // Insert installed first (higher index), then details
        if (!installedInsert.empty()) {
            auto iLines = splitLines(installedInsert);
            lines.insert(lines.begin() + instIdx, iLines.begin(), iLines.end());
            detIdx += (int)iLines.size(); // adjust for the shift
        }
        if (!detailsInsert.empty()) {
            auto dLines = splitLines(detailsInsert);
            lines.insert(lines.begin() + detIdx, dLines.begin(), dLines.end());
        }
    }

    // -------------------------------------------------------------------------
    //  Write patched ACF back
    // -------------------------------------------------------------------------
    {
        std::ofstream out(acfPath, std::ios::out | std::ios::trunc);
        if (!out.is_open()) {
            log("ERROR: Cannot open .acf for writing.", Col::Red);
            log("       Is Steam running? Close it before patching.", Col::Red);
            std::cout << "\nPress Enter to exit..."; std::cin.get(); return 1;
        }
        for (auto& ln : lines)
            out << ln << "\n";
    }

    // -------------------------------------------------------------------------
    //  Done
    // -------------------------------------------------------------------------
    log("ACF patched successfully.", Col::Green);
    log("Skins added   : " + std::to_string(toAdd.size()),   Col::Green);
    log("Skins skipped : " + std::to_string(skippedCount),   Col::Yellow);
    log("Log saved to  : " + LOG_FILE,                        Col::Cyan);
    log("IMPORTANT: Steam was closed during patching, right?", Col::Yellow);
    log("           On next Steam launch it will verify entries and fetch", Col::Yellow);
    log("           real manifest hashes -- no re-download of skin files.", Col::Yellow);

    logFile << "========== Session end: " << ts()
            << " | added=" << toAdd.size()
            << " skipped=" << skippedCount << " ==========\n";

    std::cout << "\nPress Enter to exit...";
    std::cin.get();
    return 0;
}