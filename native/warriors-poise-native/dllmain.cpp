// warriors-poise-native 0.6.0: armour-based poise. Every worn armour piece and shield adds
// points to a pool, every Light, Medium or Heavy hit removes some. While points remain the hit
// only makes the character twitch; when they run out it lands as usual. The pool refills after
// a few seconds without a poise hit. The data mod assigns the points per item and exports them
// with the rules to <game>/shroudforge/exports/warriors-poise/poise.txt.
//
// Where: the code that fills a keen::ecs::HitEvent, at the store of its hitReaction. That spot is
// inside a function fragment with no entry of its own, so it is patched in place. The idea of
// patching that store comes from Combat Rebalanced by TakeyaSaito (MIT).
// Registers there: r13 = source event, r12w = hit type, rbp = frame holding the target entity.
// The dedicated server has the same code, so one DLL serves both. Equipment exists only where a
// character is simulated: the server, or the client when it hosts a local world.
//
// Nothing is tied to one game build:
//   - code: every place is found by rule (autodetect.h): the hit site, the game's component
//     getter (whose instructions give the entity layout) and the frame slot of the target entity;
//   - data: Equipment, ItemImpact and HitEvent offsets, the hit-type values and the slot of the
//     main hand and the shield come from ShroudForge's type dump of the running executable
//     (typedump.h), used only when its SHA-256 identity matches. Without one, built-in values are
//     used only on the executables they were tested on.
// Anything missing, ambiguous or unexpected leaves the game untouched (vanilla hits) and the log
// says why. At run time every read of game memory is guarded; repeated faults switch poise off
// for the session, and a hit whose target does not check out is left alone.
//
// Log: <game>/shroudforge/logs/warriors-poise-native.log, one session. Nothing is written into the mod folder.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "autodetect.h"
#include "typedump.h"

namespace {

constexpr char kVersion[] = "0.6.0";

// The first two instructions of the hit site (mov [rbx+X],esi / mov [rbx+R],r12w): exactly the
// size of the absolute jump written over them. The pattern fixes their encoding, so their length.
constexpr size_t kPatchSize = 14;

// Entity record: the id is not read by the getter, so it stays a constant (ShroudForge's
// ecsReadLayout.entityId, the same in all its 13 profiles of 2026). Each hit checks it against
// the event's target, so a wrong value means "left alone", never a wrong pool.
constexpr size_t kEntityId = 0x10;
constexpr uint32_t kMaxComponentBits = 1024;

// One blow can reach a target as several events within a tick or two.
constexpr uint64_t kSameBlowMs = 50;
constexpr int kMaxPlayers = 16;
constexpr LONG kMaxPoiseLines = 400, kMaxSkipLines = 20, kMaxOddLines = 10;
// Memory faults while reading a target before poise switches itself off for the session.
constexpr LONG kMaxFaults = 10;

HANDLE g_log = INVALID_HANDLE_VALUE;
volatile LONG g_poise_lines = 0, g_skip_lines = 0, g_odd_lines = 0, g_faults = 0;
volatile LONG g_off = 0;  // set once poise gives up for this session

// Where things are in this build (filled once before the hook is installed, read-only after).
typedump::Layout g_layout;
autodetect::Found g_code;
uint32_t g_max_components = 0;

struct Rules {
    bool enabled = false;
    int light = 0, medium = 0, heavy = 0;
    uint64_t refill_ms = 0;
};
Rules g_rules;
// Poise per armour piece or shield; negative marks a two-handed weapon. Read-only once the game runs.
std::unordered_map<uint32_t, int> g_points;

struct Pool {
    uint32_t target = 0;
    int points = 0;
    uint64_t last_hit = 0;
    uint64_t last_reaction = 0;
    bool last_absorbed = false;
};
Pool g_pools[kMaxPlayers];
SRWLOCK g_pools_lock = SRWLOCK_INIT;
SRWLOCK g_log_lock = SRWLOCK_INIT;

void log_line(const char* text) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[640];
    int n = snprintf(line, sizeof line, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour,
                     t.wMinute, t.wSecond, t.wMilliseconds, text);
    if (n <= 0) return;
    DWORD written;
    AcquireSRWLockExclusive(&g_log_lock);
    WriteFile(g_log, line, static_cast<DWORD>(n < static_cast<int>(sizeof line) ? n : sizeof line - 1), &written, nullptr);
    ReleaseSRWLockExclusive(&g_log_lock);
}

const char* reaction_name(uint64_t reaction) {
    if (reaction < g_layout.reactions.size() && !g_layout.reactions[reaction].empty()) return g_layout.reactions[reaction].c_str();
    return "unknown";
}

constexpr int kMaxSlots = 32;

struct Hit {
    uint32_t root_target = 0;
    bool has_equipment = false;
    const char* skipped = nullptr;  // why there is no equipment to read
    uint32_t items[kMaxSlots] = {};
};

// Reads the game's memory; a bad guess must end in a log line, never in a crash.
bool read_hit(const uint8_t* event, const uint8_t* frame, Hit& hit) {
    const typedump::Layout& l = g_layout;
    const autodetect::Found& c = g_code;
    __try {
        hit.root_target = *reinterpret_cast<const uint32_t*>(event + l.event_root_target);
        const uint8_t* entity = *reinterpret_cast<const uint8_t* const*>(frame + c.frame_target);
        hit.skipped = "no target entity";
        if (!entity) return true;
        hit.skipped = "another entity in the frame";
        if (*reinterpret_cast<const uint32_t*>(entity + kEntityId) != hit.root_target) return true;
        const uint8_t* layout = *reinterpret_cast<const uint8_t* const*>(entity + c.entity_layout);
        const uint8_t* storage = *reinterpret_cast<const uint8_t* const*>(entity + c.entity_storage);
        uint32_t row = *reinterpret_cast<const uint32_t*>(entity + c.entity_row);
        hit.skipped = "no layout";
        if (!layout || !storage) return true;

        int found = -1, candidates = 0;
        for (uint32_t i = 0; i < g_max_components; ++i) {
            uint64_t bits = *reinterpret_cast<const uint64_t*>(layout + (i >> 6) * 8);
            if (!((bits >> (i & 63)) & 1)) continue;
            uint16_t stride = *reinterpret_cast<const uint16_t*>(layout + c.layout_strides + i * 2);
            if (stride != l.equipment_size) continue;
            if (found < 0) found = static_cast<int>(i);
            ++candidates;
        }
        hit.skipped = "no Equipment on the target";
        if (found < 0) return true;
        hit.skipped = "more than one component of Equipment's size (ambiguous)";
        if (candidates != 1) return true;
        uint16_t offset = *reinterpret_cast<const uint16_t*>(layout + c.layout_offsets + found * 2);
        const uint8_t* equipment = storage + offset + static_cast<size_t>(row) * l.equipment_size;
        for (uint32_t slot = 0; slot < l.impact_slots; ++slot) {
            hit.items[slot] = *reinterpret_cast<const uint32_t*>(equipment + l.impacts_at + slot * l.impact_size + l.item_id_at);
        }
        hit.has_equipment = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int cost_of(uint64_t reaction) {
    if (reaction == g_layout.light) return g_rules.light;
    if (reaction == g_layout.medium) return g_rules.medium;
    if (reaction == g_layout.heavy) return g_rules.heavy;
    return -1;  // nothing to absorb, or a hit poise never stops (Boss, stuns, knockdowns)
}

// A server runs for days and a rejoining player is a new entity, so the longest idle pool is reused.
Pool* pool_of(uint32_t target) {
    Pool* oldest = &g_pools[0];
    for (Pool& pool : g_pools) {
        if (pool.target == target) return &pool;
        if (pool.last_hit < oldest->last_hit) oldest = &pool;
    }
    *oldest = Pool{};
    oldest->target = target;
    return oldest;
}

// Called by the stub for every hit. Returns the hit type to store.
uint64_t on_hit(const uint8_t* event, const uint8_t* frame, uint64_t reaction) {
    if (g_off) return reaction;
    if (reaction >= g_layout.reactions.size()) {
        // Not a value of HitReactionType: the code around the site is not what it was. Leave it alone.
        if (InterlockedIncrement(&g_odd_lines) <= kMaxOddLines) {
            char text[96];
            snprintf(text, sizeof text, "hit type %llu is not a known HitReactionType: left alone", static_cast<unsigned long long>(reaction));
            log_line(text);
        }
        return reaction;
    }
    int cost = cost_of(reaction);
    if (!g_rules.enabled || cost < 0) return reaction;
    Hit hit;
    if (!read_hit(event, frame, hit)) {
        LONG faults = InterlockedIncrement(&g_faults);
        if (faults <= 5) log_line("hit: memory fault while reading the target, left alone");
        if (faults == kMaxFaults) {
            InterlockedExchange(&g_off, 1);
            log_line("poise OFF for this session: repeated memory faults (game changed?); hits stay vanilla");
        }
        return reaction;
    }
    if (!hit.has_equipment) {
        // On a client connected to a server this answers whether the hit code runs here at all.
        if (InterlockedIncrement(&g_skip_lines) <= kMaxSkipLines) {
            char text[200];
            snprintf(text, sizeof text, "%s hit, target %u: left alone, %s", reaction_name(reaction), hit.root_target, hit.skipped);
            log_line(text);
        }
        return reaction;
    }

    const typedump::Layout& l = g_layout;
    auto weapon = g_points.find(hit.items[l.weapon_slot]);
    bool two_handed = weapon != g_points.end() && weapon->second < 0;
    int full = 0;
    for (uint32_t slot = 0; slot < l.impact_slots; ++slot) {
        auto found = g_points.find(hit.items[slot]);
        if (found == g_points.end() || found->second <= 0) continue;
        if (slot == l.shield_slot && two_handed) continue;  // most two-handed weapons already empty this slot
        full += found->second;
    }
    if (full <= 0) return reaction;  // creatures, and players with nothing that gives poise
    uint64_t now = GetTickCount64();
    int before = 0, after = 0;
    bool absorbed = false, repeat = false;
    AcquireSRWLockExclusive(&g_pools_lock);
    Pool* pool = pool_of(hit.root_target);
    if (pool->last_hit && pool->last_reaction == reaction && now - pool->last_hit < kSameBlowMs) {
        repeat = true;
        absorbed = pool->last_absorbed;
    } else {
        if (!pool->last_hit || now - pool->last_hit >= g_rules.refill_ms || pool->points > full) pool->points = full;
        before = pool->points;
        pool->points = before > cost ? before - cost : 0;
        after = pool->points;
        absorbed = after > 0;
        pool->last_hit = now;
        pool->last_reaction = reaction;
        pool->last_absorbed = absorbed;
    }
    ReleaseSRWLockExclusive(&g_pools_lock);

    if (!repeat && InterlockedIncrement(&g_poise_lines) <= kMaxPoiseLines) {
        char text[200];
        snprintf(text, sizeof text, "%s hit, target %u: poise %d -> %d of %d, %s (weapon %u%s, shield %u)", reaction_name(reaction),
                 hit.root_target, before, after, full, absorbed ? "absorbed" : "flinch", hit.items[l.weapon_slot],
                 two_handed ? " two-handed" : "", hit.items[l.shield_slot]);
        log_line(text);
    }
    return absorbed ? l.very_light : reaction;
}

// Saves what the game code may still need, calls on_hit(r13, rbp, r12w), stores the result in
// r12w and restores everything. Flags matter: a conditional jump right after the site tests them.
const unsigned char kStubEntry[] = {
    0x9C,                                      // pushfq
    0x50, 0x51, 0x52,                          // push rax / rcx / rdx
    0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,  // push r8 / r9 / r10 / r11
    0x49, 0x89, 0xE3,                          // mov r11,rsp
    0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00,  // sub rsp,90h
    0x48, 0x83, 0xE4, 0xF0,                    // and rsp,-16
    0x4C, 0x89, 0x5C, 0x24, 0x28,              // mov [rsp+28h],r11
    0x0F, 0x11, 0x44, 0x24, 0x30,              // movups [rsp+30h],xmm0
    0x0F, 0x11, 0x4C, 0x24, 0x40,              // movups [rsp+40h],xmm1
    0x0F, 0x11, 0x54, 0x24, 0x50,              // movups [rsp+50h],xmm2
    0x0F, 0x11, 0x5C, 0x24, 0x60,              // movups [rsp+60h],xmm3
    0x0F, 0x11, 0x64, 0x24, 0x70,              // movups [rsp+70h],xmm4
    0x0F, 0x11, 0xAC, 0x24, 0x80, 0x00, 0x00, 0x00,  // movups [rsp+80h],xmm5
    0x4C, 0x89, 0xE9,                          // mov rcx,r13
    0x48, 0x89, 0xEA,                          // mov rdx,rbp
    0x4D, 0x0F, 0xB7, 0xC4,                    // movzx r8,r12w
    0x48, 0xB8,                                // mov rax,<on_hit>
};
const unsigned char kStubExit[] = {
    0xFF, 0xD0,                                // call rax
    0x66, 0x41, 0x89, 0xC4,                    // mov r12w,ax
    0x0F, 0x10, 0x44, 0x24, 0x30,              // movups xmm0,[rsp+30h]
    0x0F, 0x10, 0x4C, 0x24, 0x40,              // movups xmm1,[rsp+40h]
    0x0F, 0x10, 0x54, 0x24, 0x50,              // movups xmm2,[rsp+50h]
    0x0F, 0x10, 0x5C, 0x24, 0x60,              // movups xmm3,[rsp+60h]
    0x0F, 0x10, 0x64, 0x24, 0x70,              // movups xmm4,[rsp+70h]
    0x0F, 0x10, 0xAC, 0x24, 0x80, 0x00, 0x00, 0x00,  // movups xmm5,[rsp+80h]
    0x4C, 0x8B, 0x5C, 0x24, 0x28,              // mov r11,[rsp+28h]
    0x4C, 0x89, 0xDC,                          // mov rsp,r11
    0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58,  // pop r11 / r10 / r9 / r8
    0x5A, 0x59, 0x58,                          // pop rdx / rcx / rax
    0x9D,                                      // popfq
};
const unsigned char kAbsoluteJump[] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};  // jmp [rip+0], then the address

bool install(unsigned char* site) {
    constexpr size_t kStubSize = sizeof kStubEntry + 8 + sizeof kStubExit + kPatchSize + sizeof kAbsoluteJump + 8;
    auto stub = static_cast<unsigned char*>(VirtualAlloc(nullptr, kStubSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub) return false;
    unsigned char* p = stub;
    auto put = [&p](const void* bytes, size_t size) {
        memcpy(p, bytes, size);
        p += size;
    };
    uint64_t callback = reinterpret_cast<uint64_t>(&on_hit);
    uint64_t back = reinterpret_cast<uint64_t>(site) + kPatchSize;
    put(kStubEntry, sizeof kStubEntry);
    put(&callback, 8);
    put(kStubExit, sizeof kStubExit);
    put(site, kPatchSize);  // the two moved stores have no rip-relative operand
    put(kAbsoluteJump, sizeof kAbsoluteJump);
    put(&back, 8);
    FlushInstructionCache(GetCurrentProcess(), stub, kStubSize);

    DWORD old;
    if (!VirtualProtect(site, kPatchSize, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        return false;
    }
    uint64_t target = reinterpret_cast<uint64_t>(stub);
    memcpy(site, kAbsoluteJump, sizeof kAbsoluteJump);
    memcpy(site + sizeof kAbsoluteJump, &target, 8);
    VirtualProtect(site, kPatchSize, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, kPatchSize);
    return true;
}

// <game folder>\<relative>, or false.
bool game_path(const wchar_t* relative, wchar_t* out) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    *slash = 0;
    return swprintf(out, MAX_PATH, L"%s\\%s", path, relative) > 0;
}

void open_log() {
    wchar_t file[MAX_PATH];
    if (!game_path(L"shroudforge\\logs\\warriors-poise-native.log", file)) return;
    DeleteFileW(file);  // one session only, so the log cannot grow without end
    g_log = CreateFileW(file, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// The data mod's export: a rules line, then one "<itemId> <points>" line per armour piece or shield.
void load_rules() {
    wchar_t file[MAX_PATH];
    if (!game_path(L"shroudforge\\exports\\warriors-poise\\poise.txt", file)) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, file, L"r") != 0 || !f) {
        log_line("poise off: no poise.txt export");
        return;
    }
    char line[128];
    int enabled = 0, refill = 0;
    if (fgets(line, sizeof line, f) && sscanf_s(line, "poise %d light=%d medium=%d heavy=%d refill=%d", &enabled, &g_rules.light,
                                                &g_rules.medium, &g_rules.heavy, &refill) == 5) {
        while (fgets(line, sizeof line, f)) {
            unsigned long id = 0;
            int points = 0;
            if (sscanf_s(line, "%lu %d", &id, &points) == 2) g_points[static_cast<uint32_t>(id)] = points;
        }
        g_rules.refill_ms = refill > 0 ? static_cast<uint64_t>(refill) : 0;
        g_rules.enabled = enabled == 1 && !g_points.empty();
    }
    fclose(f);
    char text[160];
    snprintf(text, sizeof text, "poise %s: %zu items, costs light %d medium %d heavy %d, refill %llu ms", g_rules.enabled ? "on" : "off",
             g_points.size(), g_rules.light, g_rules.medium, g_rules.heavy, static_cast<unsigned long long>(g_rules.refill_ms));
    log_line(text);
}

// The data layout: ShroudForge's dump of this executable, else the built-in values if this is a
// tested executable, else nothing.
bool find_layout(const wchar_t* exe) {
    char text[512];
    std::string sha = typedump::file_sha256(exe);
    bool tested = false;
    for (const char* t : typedump::kTestedExecutables) tested = tested || sha == t;
    snprintf(text, sizeof text, "executable SHA-256 %s (%s)", sha.empty() ? "unreadable" : sha.c_str(),
             tested ? "a build this version was tested on" : "not a build this version was tested on");
    log_line(text);
    std::string why;
    if (!sha.empty() && typedump::load(exe, sha, g_layout, why)) return true;
    snprintf(text, sizeof text, "type dump not used: %s", why.c_str());
    log_line(text);
    if (!tested) return false;
    g_layout = typedump::tested_layout();
    return true;
}

void start() {
    auto base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    wchar_t exe[MAX_PATH] = L"";
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) exe[0] = 0;
    const wchar_t* name = wcsrchr(exe, L'\\');
    char text[600];
    snprintf(text, sizeof text, "warriors-poise-native %s loaded in %ls, PE TimeDateStamp 0x%08X, image size %lu", kVersion,
             name ? name + 1 : exe, nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
    log_line(text);
    load_rules();
    if (!g_rules.enabled) return;  // nothing to do: the game's code is not even searched

    // Data layout.
    if (!exe[0] || !find_layout(exe)) {
        log_line("inactive: no data layout for this game build (no matching ShroudForge type dump, not a tested build), nothing patched");
        return;
    }
    snprintf(text, sizeof text, "layout (%s): Equipment %u B, impacts +%u, %u slots of %u B, item id +%u, main hand slot %u, shield slot %u; "
                                "HitEvent target +0x%X, hit type +0x%X; Light %u Medium %u Heavy %u -> VeryLight %u",
             g_layout.source.c_str(), g_layout.equipment_size, g_layout.impacts_at, g_layout.impact_slots, g_layout.impact_size,
             g_layout.item_id_at, g_layout.weapon_slot, g_layout.shield_slot, g_layout.event_root_target, g_layout.event_hit_reaction,
             g_layout.light, g_layout.medium, g_layout.heavy, g_layout.very_light);
    log_line(text);
    if (g_layout.impact_slots > kMaxSlots || g_layout.reactions.empty()) {
        log_line("inactive: the layout has more equipment slots than this version handles, nothing patched");
        return;
    }

    // Code.
    autodetect::Image im;
    if (!autodetect::open(base, im)) {
        log_line("inactive: the game executable could not be read, nothing patched");
        return;
    }
    std::vector<std::string> notes;
    autodetect::detect(im, g_layout.event_hit_reaction, g_code, notes);
    for (const std::string& line : notes) log_line(("rule " + line).c_str());
    if (!g_code.ok) {
        log_line("inactive: a rule found no single target (game changed?), nothing patched; hits stay vanilla");
        return;
    }
    // Presence bits sit before the offset table; never read past either table.
    uint32_t by_bits = (g_code.layout_offsets / 8) * 64, by_table = (g_code.layout_strides - g_code.layout_offsets) / 2;
    g_max_components = by_bits < by_table ? by_bits : by_table;
    if (g_max_components > kMaxComponentBits) g_max_components = kMaxComponentBits;

    snprintf(text, sizeof text, "%s at +0x%X", install(base + g_code.hit_site) ? "poise hook installed" : "poise hook FAILED to install",
             g_code.hit_site);
    log_line(text);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        open_log();
        start();
    }
    return TRUE;
}
