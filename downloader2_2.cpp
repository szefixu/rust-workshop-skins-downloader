/*
 * Rust Workshop Skin Downloader
 *
 * Fixes in this version:
 *  [1] LOCKING FAILED   – each steamcmd instance gets its own isolated install
 *      directory (rust_workshop_tN) so patch state files never collide. After
 *      a successful download the skin folder is moved to the shared content path.
 *  [2] STAGED FILE VALIDATION / MISSING UPDATE FILES – stale partial downloads
 *      in the steamcmd "downloads/" staging folder are wiped before every run
 *      and before every retry pass, so corrupted stage files can't block items.
 *  [3] New result categories: LockFailed, ValidationFailed (both auto-retried).
 *  [4] Smarter log parsing: detects all result lines steamcmd actually writes.
 *
 * Build (MSVC):  cl /std:c++17 /O2 workshop_downloader.cpp /Fe:downloader.exe
 * Build (MinGW): g++ -std=c++17 -O2 workshop_downloader.cpp -o downloader.exe
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <regex>
#include <thread>
#include <mutex>
#include <atomic>
#include <filesystem>
#include <chrono>
#include <cstdlib>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <iomanip>
#include <ctime>

namespace fs = std::filesystem;
using Clock  = std::chrono::steady_clock;

// ─────────────────────────────────────────────────────────────────────────────
//  CONFIGURATION
// ─────────────────────────────────────────────────────────────────────────────
const std::string APP_ID          = "252490";
// Shared content destination – where skins end up after a successful download.
const std::string SHARED_DIR      = "rust_workshop";
const std::string CONTENT_PATH    = SHARED_DIR + "/steamapps/workshop/content/" + APP_ID;
// Per-instance install dir template – threadId is appended at runtime.
const std::string INSTANCES_ROOT  = "instances";
const std::string INST_DIR_PREFIX = INSTANCES_ROOT + "/rust_workshop_t";
const std::string LOG_DIR         = "logs";
const std::string TEMP_DIR        = "temp_scripts";
const std::string FAILED_IDS_FILE = "failed_ids.txt";
const std::string REPORT_FILE     = "download_report.txt";

const int BASE_TIMEOUT_SEC        = 90;   // per-item; instance timeout = BASE * chunk.size()
const int STATUS_POLL_MS          = 500;
const int MAX_RETRY_PASSES        = 3;    // extra passes (LockFailed/Validation get extra chance)
const int RATELIMIT_BACKOFF_SEC   = 30;

// ─────────────────────────────────────────────────────────────────────────────
//  ANSI COLOURS
// ─────────────────────────────────────────────────────────────────────────────
namespace Col {
    const char* Reset   = "\033[0m";
    const char* Green   = "\033[32m";
    const char* Yellow  = "\033[33m";
    const char* Red     = "\033[31m";
    const char* Cyan    = "\033[36m";
    const char* Magenta = "\033[35m";
    const char* White   = "\033[97m";
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

// ─────────────────────────────────────────────────────────────────────────────
//  RESULT CATEGORIES
// ─────────────────────────────────────────────────────────────────────────────
enum class SkinResult {
    Success,
    Skipped,
    Timeout,
    RateLimit,
    LockFailed,       // "result : Locking Failed" – file locked by parallel instance
    ValidationFailed, // "Staged file validation failed" – stale/corrupt staging files
    Error,
    Unknown
};

static std::string resultName(SkinResult r) {
    switch (r) {
        case SkinResult::Success:          return "Success";
        case SkinResult::Skipped:          return "Skipped";
        case SkinResult::Timeout:          return "Timeout";
        case SkinResult::RateLimit:        return "RateLimit";
        case SkinResult::LockFailed:       return "LockFailed";
        case SkinResult::ValidationFailed: return "ValidationFailed";
        case SkinResult::Error:            return "Error";
        default:                           return "Unknown";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  SHARED STATE
// ─────────────────────────────────────────────────────────────────────────────
std::mutex coutMtx;
std::mutex resultMtx;

std::atomic<int> successCount(0);
std::atomic<int> failedCount(0);
std::atomic<int> skippedCount(0);
std::atomic<int> timeoutCount(0);
std::atomic<int> errorCount(0);
std::atomic<int> ratelimitCount(0);
std::atomic<int> lockFailCount(0);
std::atomic<int> validationFailCount(0);
std::atomic<int> totalProcessed(0);
std::atomic<bool> anyRateLimitDetected(false);

std::unordered_map<std::string, SkinResult> skinResults;

// ─────────────────────────────────────────────────────────────────────────────
//  LOGGING
// ─────────────────────────────────────────────────────────────────────────────
static std::string timestamp() {
    auto t  = std::time(nullptr);
    auto tm = *std::localtime(&t);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%H:%M:%S");
    return ss.str();
}

static std::mutex fileMtx;
static std::ofstream mainLogFile;

static void fileLog(const std::string& msg) {
    std::lock_guard<std::mutex> lk(fileMtx);
    if (mainLogFile.is_open())
        mainLogFile << "[" << timestamp() << "] " << msg << "\n";
}

static void logMain(const std::string& msg, const char* colour = Col::White) {
    std::lock_guard<std::mutex> lock(coutMtx);
    std::cout << "\n" << colour << "[" << timestamp() << "] " << msg << Col::Reset;
    std::cout.flush();
    fileLog(msg);
}

// ─────────────────────────────────────────────────────────────────────────────
//  PROGRESS BAR
// ─────────────────────────────────────────────────────────────────────────────
static void printProgress(int total, int pass, int maxPass) {
    int done  = totalProcessed.load();
    int succ  = successCount.load();
    int skip  = skippedCount.load();
    int fail  = failedCount.load();
    int tmt   = timeoutCount.load();
    int err   = errorCount.load();
    int rl    = ratelimitCount.load();
    int lk    = lockFailCount.load();
    int vf    = validationFailCount.load();
    int rem   = std::max(0, total - done);

    float pct   = total > 0 ? (done * 100.f / total) : 0.f;
    const int W = 28;
    int filled  = static_cast<int>(W * pct / 100.f);

    std::lock_guard<std::mutex> lock(coutMtx);
    std::cout << "\r\033[K";
    std::cout << Col::Cyan  << "[Pass " << pass << "/" << maxPass << "] " << Col::Reset;
    std::cout << Col::Bold  << "[";
    for (int i = 0; i < W; ++i)
        std::cout << (i < filled ? '=' : (i == filled ? '>' : ' '));
    std::cout << "] " << std::fixed << std::setprecision(1) << pct << "% ";
    std::cout << Col::Green   << "OK:"   << succ                    << Col::Reset << " ";
    std::cout << Col::Yellow  << "Skip:" << skip                    << Col::Reset << " ";
    std::cout << Col::Red     << "Fail:" << fail;
    std::cout << "(T:"  << tmt;
    std::cout << " E:"  << err;
    std::cout << " RL:" << rl;
    std::cout << " LK:" << lk;
    std::cout << " VF:" << vf << ")"                                << Col::Reset << " ";
    std::cout << "Rem:" << rem << Col::Reset;
    std::cout.flush();
}

// ─────────────────────────────────────────────────────────────────────────────
//  FILESYSTEM HELPERS
// ─────────────────────────────────────────────────────────────────────────────
static bool folderHasFiles(const fs::path& p) {
    try {
        if (!fs::exists(p) || !fs::is_directory(p)) return false;
        for (auto& e : fs::directory_iterator(p))
            if (fs::is_regular_file(e) && fs::file_size(e) > 0) return true;
    } catch (...) {}
    return false;
}

// Wipe the steamcmd staging / downloads folder inside an instance dir.
// This removes stale .patch and partial download files that cause
// "Staged file validation failed (N missing)" errors on repeated runs.
static void cleanStagingFolder(const std::string& instanceDir) {
    // steamcmd writes partial downloads to these subdirs:
    static const std::vector<std::string> stagingSubdirs = {
        "/steamapps/workshop/downloads",
        "/steamapps/workshop/temp",
        "/steamapps/downloading",
    };
    for (const auto& sub : stagingSubdirs) {
        fs::path p = fs::path(instanceDir) / sub;
        if (fs::exists(p)) {
            try {
                for (auto& entry : fs::directory_iterator(p))
                    fs::remove_all(entry);
            } catch (const std::exception& ex) {
                fileLog("WARN: Could not clean staging dir " + p.string() + ": " + ex.what());
            }
        }
    }
}

// Wipe stale .patch and .lock files from the shared workshop downloads dir.
// These are leftover locks that block parallel instances from acquiring access.
static void cleanSharedPatchFiles() {
    fs::path downloadsDir = fs::path(SHARED_DIR) / "steamapps" / "workshop" / "downloads";
    if (!fs::exists(downloadsDir)) return;
    try {
        for (auto& entry : fs::directory_iterator(downloadsDir)) {
            std::string ext = entry.path().extension().string();
            if (ext == ".patch" || ext == ".lock") {
                try { fs::remove(entry); }
                catch (...) {}
            }
        }
    } catch (...) {}
}

static void prepareDirs() {
    fs::create_directories(LOG_DIR);
    fs::create_directories(CONTENT_PATH);
    if (fs::exists(TEMP_DIR)) {
        try { fs::remove_all(TEMP_DIR); } catch (...) {}
    }
    fs::create_directories(TEMP_DIR);
}

// Move a downloaded skin from the instance's content dir to the shared one.
// Returns true if skin is confirmed present in shared dir after the operation.
static bool moveSkinToShared(const std::string& instanceDir, const std::string& skinId) {
    fs::path src = fs::path(instanceDir) / "steamapps" / "workshop" / "content" / APP_ID / skinId;
    fs::path dst = fs::path(CONTENT_PATH) / skinId;

    if (folderHasFiles(dst)) return true; // already present from a previous pass

    if (!folderHasFiles(src)) return false;

    try {
        fs::create_directories(dst.parent_path());
        fs::rename(src, dst); // atomic on same filesystem
        return folderHasFiles(dst);
    } catch (...) {
        // Cross-device: fall back to recursive copy then remove source
        try {
            fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            fs::remove_all(src);
            return folderHasFiles(dst);
        } catch (const std::exception& ex) {
            fileLog("ERROR moving skin " + skinId + ": " + ex.what());
            return false;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  STEAMCMD LOG PARSER
//
//  Handles all result line formats seen in practice:
//    [AppID 252490] Download item 3511955902 result : Locking Failed
//    [AppID 252490] Download item 492051023  result : Failure
//    [AppID 252490] Update canceled: Staged file validation failed (13 missing...)
//    [AppID 252490] Update canceled: Failed to write patch state file (File locked)
//    Success. Downloaded item 1234567 to ...
//    ERROR! Download item 1234567 failed (Timeout).
//    Timeout downloading item 1234567
// ─────────────────────────────────────────────────────────────────────────────
struct ParsedLog {
    std::unordered_map<std::string, SkinResult> perItem;
    bool globalRateLimit      = false;
    bool globalTimeout        = false;
    bool globalLockFailed     = false;
    bool globalValidationFail = false;
    int  successCount         = 0;
    int  failureCount         = 0;
};

static ParsedLog parseSteamCmdLog(const std::string& logPath,
                                  const std::vector<std::string>& chunk) {
    ParsedLog result;
    std::ifstream file(logPath);
    if (!file.is_open()) {
        fileLog("WARN: Could not open log for parsing: " + logPath);
        return result;
    }

    for (const auto& id : chunk)
        result.perItem[id] = SkinResult::Unknown;

    // Workshop log "result :" line
    std::regex reResult    (R"(\[AppID \d+\] Download item (\d+) result : (.+))");
    // steamcmd console "Success." line
    std::regex reSuccess   (R"(Success\. Downloaded item (\d+))");
    // steamcmd console "ERROR!" line
    std::regex reError     (R"(ERROR! Download item (\d+) failed \(([^)]+)\))");
    // steamcmd "Timeout" standalone line
    std::regex reTimeout   (R"(Timeout downloading item (\d+))");
    // Staged validation failure with an item ID embedded
    std::regex reValidation(R"(Staged file validation failed.*?item (\d+))", std::regex::icase);
    // Patch-state file lock (no item ID in line)
    std::regex rePatchLock (R"(Failed to write patch state file \(File locked\))", std::regex::icase);
    // Rate limit
    std::regex reRateLimit (R"(rate.?limit|too many requests|throttled)", std::regex::icase);

    std::string line;
    std::string lastId; // context for lines that have no embedded item ID

    while (std::getline(file, line)) {
        std::smatch m;

        // ── Workshop log result line ─────────────────────────────────────
        if (std::regex_search(line, m, reResult)) {
            std::string id     = m[1].str();
            std::string reason = m[2].str();
            lastId = id;

            SkinResult sr = SkinResult::Error;
            if (reason == "OK" || reason.find("Success") != std::string::npos) {
                sr = SkinResult::Success;
                result.successCount++;
            } else if (reason.find("Locking Failed") != std::string::npos ||
                       reason.find("locked")         != std::string::npos) {
                sr = SkinResult::LockFailed;
                result.globalLockFailed = true;
                result.failureCount++;
            } else if (reason.find("Timeout") != std::string::npos) {
                sr = SkinResult::Timeout;
                result.globalTimeout = true;
                result.failureCount++;
            } else if (reason.find("rate") != std::string::npos ||
                       reason.find("Rate") != std::string::npos) {
                sr = SkinResult::RateLimit;
                result.globalRateLimit = true;
                result.failureCount++;
            } else {
                // Generic "Failure" – may be refined by earlier/later context lines
                sr = SkinResult::Error;
                result.failureCount++;
            }
            if (result.perItem.count(id))
                result.perItem[id] = sr;
            continue;
        }

        // ── Staged file validation failure (with item ID) ────────────────
        if (std::regex_search(line, m, reValidation)) {
            std::string id = m[1].str();
            if (result.perItem.count(id))
                result.perItem[id] = SkinResult::ValidationFailed;
            result.globalValidationFail = true;
            continue;
        }
        // Staged file validation failure (no item ID – use lastId context)
        if (line.find("Staged file validation failed") != std::string::npos ||
            line.find("Missing update files")          != std::string::npos) {
            result.globalValidationFail = true;
            if (!lastId.empty() && result.perItem.count(lastId) &&
                (result.perItem[lastId] == SkinResult::Error ||
                 result.perItem[lastId] == SkinResult::Unknown))
                result.perItem[lastId] = SkinResult::ValidationFailed;
            continue;
        }

        // ── Patch-state lock (no item ID – use lastId context) ───────────
        if (std::regex_search(line, rePatchLock)) {
            result.globalLockFailed = true;
            if (!lastId.empty() && result.perItem.count(lastId) &&
                (result.perItem[lastId] == SkinResult::Error ||
                 result.perItem[lastId] == SkinResult::Unknown))
                result.perItem[lastId] = SkinResult::LockFailed;
            continue;
        }

        // ── steamcmd "Success." console line ────────────────────────────
        if (std::regex_search(line, m, reSuccess)) {
            std::string id = m[1].str();
            if (result.perItem.count(id)) {
                result.perItem[id] = SkinResult::Success;
                result.successCount++;
            }
            lastId = id;
            continue;
        }

        // ── steamcmd "ERROR!" console line ──────────────────────────────
        if (std::regex_search(line, m, reError)) {
            std::string id     = m[1].str();
            std::string reason = m[2].str();
            lastId = id;
            SkinResult sr = SkinResult::Error;
            if (reason.find("Timeout") != std::string::npos) {
                sr = SkinResult::Timeout;
                result.globalTimeout = true;
            } else if (reason.find("rate") != std::string::npos ||
                       reason.find("Rate") != std::string::npos) {
                sr = SkinResult::RateLimit;
                result.globalRateLimit = true;
            }
            if (result.perItem.count(id)) result.perItem[id] = sr;
            result.failureCount++;
            continue;
        }

        // ── steamcmd "Timeout" standalone console line ───────────────────
        if (std::regex_search(line, m, reTimeout)) {
            std::string id = m[1].str();
            if (result.perItem.count(id)) result.perItem[id] = SkinResult::Timeout;
            result.globalTimeout = true;
            result.failureCount++;
            lastId = id;
            continue;
        }

        // ── Global rate-limit marker ─────────────────────────────────────
        if (std::regex_search(line, reRateLimit))
            result.globalRateLimit = true;
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  WORKER – one steamcmd instance in its own isolated install directory
// ─────────────────────────────────────────────────────────────────────────────
static void workerInstance(const std::vector<std::string>& chunk,
                            int  threadId,
                            int  total,
                            int  pass) {
    if (chunk.empty()) return;

    std::string instanceDir = INST_DIR_PREFIX + std::to_string(threadId);
    std::string threadTemp  = TEMP_DIR + "/t" + std::to_string(threadId);
    try {
        fs::create_directories(threadTemp);
        fs::create_directories(instanceDir);
    } catch (...) {}

    std::string scriptPath = threadTemp + "/script.txt";
    std::string logPath    = LOG_DIR + "/instance_p" + std::to_string(pass)
                           + "_t" + std::to_string(threadId) + ".log";

    // Clean stale staging files in THIS instance's dir before starting
    cleanStagingFolder(instanceDir);

    // ── Write steamcmd script ─────────────────────────────────────────────
    {
        std::ofstream sc(scriptPath);
        if (!sc.is_open()) {
            logMain("ERROR: Could not create script: " + scriptPath, Col::Red);
            return;
        }
        sc << "login anonymous\n";
        // Isolated install dir → no shared patch-state-file collisions
        sc << "force_install_dir ./" << instanceDir << "\n";
        for (const auto& id : chunk)
            sc << "workshop_download_item " << APP_ID << " " << id << "\n";
        sc << "quit\n";
    }

    fileLog("[T" + std::to_string(threadId) + "][P" + std::to_string(pass) + "] "
            "Starting | dir=" + instanceDir + " | items=" + std::to_string(chunk.size()));

    // ── Run steamcmd ──────────────────────────────────────────────────────
    std::string cmd = "steamcmd.exe +runscript \"" + scriptPath
                    + "\" > \"" + logPath + "\" 2>&1";

    std::atomic<bool> procDone(false);
    auto tStart = Clock::now();

    std::thread procThread([&]() {
        std::system(cmd.c_str());
        procDone.store(true, std::memory_order_release);
    });

    long long instanceTimeout = (long long)BASE_TIMEOUT_SEC * (long long)chunk.size();
    bool timedOut = false;

    while (!procDone.load(std::memory_order_acquire)) {
        long long elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                Clock::now() - tStart).count();
        if (elapsed > instanceTimeout) {
            timedOut = true;
            fileLog("[T" + std::to_string(threadId) + "] Hard timeout (" +
                    std::to_string(elapsed) + "s). Killing steamcmd.");
#ifdef _WIN32
            std::system("taskkill /F /IM steamcmd.exe >NUL 2>&1");
#else
            std::system("pkill -f steamcmd");
#endif
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(STATUS_POLL_MS));
    }

    procThread.join();
    long long dur = std::chrono::duration_cast<std::chrono::seconds>(
                        Clock::now() - tStart).count();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    try { fs::remove(scriptPath); } catch (...) {}

    // ── Parse log ─────────────────────────────────────────────────────────
    ParsedLog parsed = parseSteamCmdLog(logPath, chunk);

    fileLog("[T" + std::to_string(threadId) + "] Finished in " + std::to_string(dur) + "s"
            + " | OK="     + std::to_string(parsed.successCount)
            + " Fail="     + std::to_string(parsed.failureCount)
            + " RL="       + std::to_string(parsed.globalRateLimit)
            + " TM="       + std::to_string(parsed.globalTimeout)
            + " LK="       + std::to_string(parsed.globalLockFailed)
            + " VF="       + std::to_string(parsed.globalValidationFail));

    if (parsed.globalRateLimit) {
        anyRateLimitDetected.store(true);
        logMain("[T" + std::to_string(threadId) + "] Rate limit – backing off "
                + std::to_string(RATELIMIT_BACKOFF_SEC) + "s", Col::Yellow);
        std::this_thread::sleep_for(std::chrono::seconds(RATELIMIT_BACKOFF_SEC));
    }

    // ── Reconcile: move from instance dir → shared, then classify ─────────
    for (const auto& id : chunk) {
        SkinResult sr = parsed.perItem.count(id) ? parsed.perItem.at(id)
                                                 : SkinResult::Unknown;

        bool moved    = moveSkinToShared(instanceDir, id);
        bool inShared = folderHasFiles(fs::path(CONTENT_PATH) / id);

        if (moved || inShared) {
            sr = SkinResult::Success;
        } else if (sr == SkinResult::Success) {
            // steamcmd reported success but no files materialised
            sr = SkinResult::ValidationFailed;
            fileLog("WARN: steamcmd said Success for " + id + " but no files found – "
                    "treating as ValidationFailed (will retry).");
        }

        // Hard-timeout overrides anything that isn't already a success
        if (timedOut && sr != SkinResult::Success)
            sr = SkinResult::Timeout;

        switch (sr) {
            case SkinResult::Success:
                successCount++;
                break;
            case SkinResult::Timeout:
                timeoutCount++;        failedCount++; break;
            case SkinResult::RateLimit:
                ratelimitCount++;      failedCount++; break;
            case SkinResult::LockFailed:
                lockFailCount++;       failedCount++; break;
            case SkinResult::ValidationFailed:
                validationFailCount++; failedCount++; break;
            default:
                errorCount++;          failedCount++; sr = SkinResult::Error; break;
        }
        totalProcessed++;

        {
            std::lock_guard<std::mutex> lk(resultMtx);
            skinResults[id] = sr;
        }
    }

    // Clean staging again so the next pass on this instance dir starts fresh
    cleanStagingFolder(instanceDir);
}

// ─────────────────────────────────────────────────────────────────────────────
//  JSON ID PARSER
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<std::string> parseIds(const std::string& jsonFile) {
    std::ifstream file(jsonFile);
    std::vector<std::string> result;
    std::string line;
    std::regex idRe(R"re("(\d{6,12})")re");

    while (std::getline(file, line)) {
        std::smatch m;
        std::string s = line;
        while (std::regex_search(s, m, idRe)) {
            result.push_back(m[1]);
            s = m.suffix().str();
        }
    }
    std::unordered_set<std::string> seen;
    result.erase(std::remove_if(result.begin(), result.end(),
        [&](const std::string& id){ return !seen.insert(id).second; }), result.end());
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  PARTITIONER
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<std::vector<std::string>> partition(const std::vector<std::string>& ids,
                                                        int n) {
    std::vector<std::vector<std::string>> chunks(n);
    int base = (int)ids.size() / n;
    int rem  = (int)ids.size() % n;
    int idx  = 0;
    for (int i = 0; i < n; ++i) {
        int sz = base + (i < rem ? 1 : 0);
        for (int j = 0; j < sz && idx < (int)ids.size(); ++j)
            chunks[i].push_back(ids[idx++]);
    }
    return chunks;
}

// ─────────────────────────────────────────────────────────────────────────────
//  RUN ONE PASS
// ─────────────────────────────────────────────────────────────────────────────
static void runPass(const std::vector<std::string>& toDownload,
                    int instances, int pass, int grandTotal) {
    if (toDownload.empty()) return;

    int n = std::min(instances, (int)toDownload.size());
    auto chunks = partition(toDownload, n);

    logMain("Pass " + std::to_string(pass) + "/" + std::to_string(MAX_RETRY_PASSES + 1)
            + ": " + std::to_string(toDownload.size()) + " skins → "
            + std::to_string(n) + " isolated steamcmd instance(s).", Col::Cyan);

    cleanSharedPatchFiles(); // remove leftover shared locks before spawning

    std::vector<std::thread> pool;
    pool.reserve(n);
    for (int i = 0; i < n; ++i)
        pool.emplace_back(workerInstance, std::cref(chunks[i]), i, grandTotal, pass);

    std::atomic<bool> allDone(false);
    std::thread statusThread([&]() {
        while (!allDone.load(std::memory_order_acquire)) {
            printProgress(grandTotal, pass, MAX_RETRY_PASSES + 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(STATUS_POLL_MS));
        }
        printProgress(grandTotal, pass, MAX_RETRY_PASSES + 1);
    });

    for (auto& t : pool) t.join();
    allDone.store(true);
    if (statusThread.joinable()) statusThread.join();
}

// ─────────────────────────────────────────────────────────────────────────────
//  HELPERS FOR RETRY LOGIC
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<std::string> collectFailed(const std::vector<std::string>& ids) {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lk(resultMtx);
    for (const auto& id : ids) {
        auto it = skinResults.find(id);
        if (it != skinResults.end() &&
            it->second != SkinResult::Success &&
            it->second != SkinResult::Skipped)
            out.push_back(id);
    }
    return out;
}

static void resetCountersForRetry(const std::vector<std::string>& ids) {
    std::lock_guard<std::mutex> lk(resultMtx);
    for (const auto& id : ids) {
        auto it = skinResults.find(id);
        if (it == skinResults.end()) continue;
        switch (it->second) {
            case SkinResult::Timeout:          timeoutCount--;          break;
            case SkinResult::RateLimit:        ratelimitCount--;        break;
            case SkinResult::LockFailed:       lockFailCount--;         break;
            case SkinResult::ValidationFailed: validationFailCount--;   break;
            default:                           errorCount--;            break;
        }
        failedCount--;
        totalProcessed--;
        it->second = SkinResult::Unknown;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  REPORT WRITER
// ─────────────────────────────────────────────────────────────────────────────
static void writeReport(const std::vector<std::string>& allIds) {
    std::ofstream rep(REPORT_FILE);
    std::ofstream failFile(FAILED_IDS_FILE);

    rep << "=== Workshop Skin Download Report ===\n"
        << "Date:                " << timestamp()                 << "\n\n"
        << "Total IDs:           " << allIds.size()               << "\n"
        << "Skipped:             " << skippedCount.load()         << "\n"
        << "Success:             " << successCount.load()         << "\n"
        << "Failed (total):      " << failedCount.load()          << "\n"
        << "  Timeouts:          " << timeoutCount.load()         << "\n"
        << "  Errors:            " << errorCount.load()           << "\n"
        << "  RateLimit:         " << ratelimitCount.load()       << "\n"
        << "  LockFailed:        " << lockFailCount.load()        << "\n"
        << "  ValidationFailed:  " << validationFailCount.load()  << "\n\n"
        << "--- Failed skin IDs ---\n";

    std::lock_guard<std::mutex> lk(resultMtx);
    for (const auto& id : allIds) {
        auto it = skinResults.find(id);
        if (it == skinResults.end()) continue;
        if (it->second != SkinResult::Success && it->second != SkinResult::Skipped) {
            rep << id << "  [" << resultName(it->second) << "]\n";
            failFile << id << "\n";
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  MAIN
// ─────────────────────────────────────────────────────────────────────────────
int main() {
    enableAnsi();
    prepareDirs();
    mainLogFile.open(LOG_DIR + "/main.log", std::ios::out | std::ios::app);

    std::cout << Col::Bold << Col::Cyan
        << "--------------------------------------------------------\n"
        << "-     Rust Workshop Skin Downloader  (steamcmd)        -\n"
        << "-  Fix: isolated dirs · staging cleanup · lock detect  -\n"
        << "--------------------------------------------------------\n"
        << Col::Reset;

    // ── Pre-flight ────────────────────────────────────────────────────────
    if (!fs::exists("steamcmd.exe")) {
        logMain("ERROR: steamcmd.exe not found.", Col::Red); return 1;
    }
    if (!fs::exists("ImportedSkins.json")) {
        logMain("ERROR: ImportedSkins.json not found.", Col::Red); return 1;
    }

    auto allIds = parseIds("ImportedSkins.json");
    if (allIds.empty()) {
        logMain("ERROR: No skin IDs found in ImportedSkins.json.", Col::Red); return 1;
    }
    logMain("Loaded " + std::to_string(allIds.size()) + " unique skin IDs.", Col::Green);

    // ── User input ────────────────────────────────────────────────────────
    int  maxInstances;
    char skipExistingCh;
    char prevFailedCh = 'n';

    std::cout << "\n" << Col::Yellow
              << "NOTE: Each instance downloads to its own rust_workshop_tN directory\n"
              << "      to prevent 'Locking Failed' collisions. Recommended: 1-3.\n"
              << Col::Reset;
    std::cout << Col::Yellow << "Max parallel SteamCMD instances: " << Col::Reset;
    std::cin >> maxInstances;
    if (maxInstances < 1) maxInstances = 1;

    std::cout << Col::Yellow << "Skip already-downloaded skins? (y/n): " << Col::Reset;
    std::cin >> skipExistingCh;
    bool skipExisting = (skipExistingCh == 'y' || skipExistingCh == 'Y');

    std::unordered_set<std::string> prevFailed;
    if (fs::exists(FAILED_IDS_FILE)) {
        std::ifstream ff(FAILED_IDS_FILE);
        std::string ln;
        while (std::getline(ff, ln))
            if (!ln.empty()) prevFailed.insert(ln);
        std::cout << Col::Yellow << "Found " << prevFailed.size()
                  << " previously-failed IDs. Retry only those? (y/n): " << Col::Reset;
        std::cin >> prevFailedCh;
    }
    bool onlyPrevFailed = !prevFailed.empty() &&
                          (prevFailedCh == 'y' || prevFailedCh == 'Y');

    // ── Build work list ───────────────────────────────────────────────────
    std::vector<std::string> toProcess;
    for (const auto& id : allIds) {
        if (onlyPrevFailed && !prevFailed.count(id)) {
            std::lock_guard<std::mutex> lk(resultMtx);
            skinResults[id] = SkinResult::Skipped;
            skippedCount++;
            continue;
        }
        if (skipExisting && folderHasFiles(fs::path(CONTENT_PATH) / id)) {
            std::lock_guard<std::mutex> lk(resultMtx);
            skinResults[id] = SkinResult::Skipped;
            skippedCount++;
            continue;
        }
        toProcess.push_back(id);
    }

    int grandTotal = (int)toProcess.size();
    if (grandTotal == 0) {
        logMain("Nothing to download.", Col::Green);
        std::cout << "Skipped: " << skippedCount << "\n";
        return 0;
    }

    logMain("Skins to download: " + std::to_string(grandTotal)
            + "  |  Already present (skipped): " + std::to_string(skippedCount.load()), Col::Cyan);
    fileLog("=== Session start | total=" + std::to_string(grandTotal)
            + " instances=" + std::to_string(maxInstances) + " ===");

    // ── Initial pass ──────────────────────────────────────────────────────
    auto tSessionStart = Clock::now();
    runPass(toProcess, maxInstances, 1, grandTotal);

    // ── Retry passes ──────────────────────────────────────────────────────
    for (int retry = 1; retry <= MAX_RETRY_PASSES; ++retry) {
        auto failed = collectFailed(toProcess);
        if (failed.empty()) {
            logMain("All items succeeded – no retries needed.", Col::Green);
            break;
        }

        // Diagnostic breakdown
        int vfCount = 0, lkCount = 0;
        {
            std::lock_guard<std::mutex> lk(resultMtx);
            for (const auto& id : failed) {
                auto it = skinResults.find(id);
                if (it == skinResults.end()) continue;
                if (it->second == SkinResult::ValidationFailed) vfCount++;
                if (it->second == SkinResult::LockFailed)       lkCount++;
            }
        }

        logMain("Retry pass " + std::to_string(retry) + "/" + std::to_string(MAX_RETRY_PASSES)
                + ": " + std::to_string(failed.size()) + " item(s)"
                + "  [VF=" + std::to_string(vfCount)
                + " LK=" + std::to_string(lkCount) + "]", Col::Yellow);

        // Wipe ALL instance staging dirs + shared locks before retry
        for (int i = 0; i < maxInstances; ++i)
            cleanStagingFolder(INST_DIR_PREFIX + std::to_string(i));
        cleanSharedPatchFiles();

        if (anyRateLimitDetected.load()) {
            int backoff = RATELIMIT_BACKOFF_SEC * 2;
            logMain("Rate-limit detected; sleeping " + std::to_string(backoff) + "s...", Col::Yellow);
            std::this_thread::sleep_for(std::chrono::seconds(backoff));
            anyRateLimitDetected.store(false);
        }

        resetCountersForRetry(failed);

        // Fewer instances on retry to lower rate-limit and lock pressure
        int retryInst = std::max(1, maxInstances / 2);
        runPass(failed, retryInst, retry + 1, grandTotal);
    }

    // ── Final summary ─────────────────────────────────────────────────────
    long long totalSec = std::chrono::duration_cast<std::chrono::seconds>(
                             Clock::now() - tSessionStart).count();

    std::cout << "\n\n"
        << Col::Bold    << "──────────────── Download Complete ────────────────\n" << Col::Reset
        << Col::Green   << "  Success:             " << successCount          << "\n" << Col::Reset
        << Col::Yellow  << "  Skipped:             " << skippedCount          << "\n" << Col::Reset
        << Col::Red     << "  Failed (total):      " << failedCount           << "\n"
                        << "    Timeouts:            " << timeoutCount        << "\n"
                        << "    Errors:               " << errorCount         << "\n"
                        << "    RateLimit:            " << ratelimitCount     << "\n"
        << Col::Magenta << "    LockFailed:           " << lockFailCount      << "\n"
                        << "    ValidationFailed:     " << validationFailCount<< "\n" << Col::Reset
        << "  Total time: " << totalSec / 60 << "m " << totalSec % 60 << "s\n"
        << "────────────────────────────────────────────────────\n";
    if (failedCount > 0)
        std::cout << Col::Yellow << "  Failed IDs → " << FAILED_IDS_FILE << "\n" << Col::Reset;
    std::cout << "  Report     → " << REPORT_FILE  << "\n"
              << "  Logs       → " << LOG_DIR       << "/\n\n";

    writeReport(allIds);
    fileLog("=== Session end | success=" + std::to_string(successCount.load())
            + " failed=" + std::to_string(failedCount.load())
            + " time=" + std::to_string(totalSec) + "s ===");

    if (mainLogFile.is_open()) mainLogFile.close();
    return 0;
}