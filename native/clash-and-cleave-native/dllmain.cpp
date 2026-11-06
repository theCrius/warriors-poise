// clash-and-cleave-native 0.4.0: armour-based poise. Every worn armour piece and shield adds
// points to a pool, every Light, Medium or Heavy hit removes some. While points remain the hit
// only makes the character twitch; when they run out it lands as usual. The pool refills after
// a few seconds without a poise hit. The data mod assigns the points per item and exports them
// with the rules to <game>/shroudforge/exports/clash-and-cleave/poise.txt.
//
// Where: the code that fills a keen::ecs::HitEvent, at the store of its hitReaction. That spot is
// inside a function fragment with no entry of its own, so it is patched in place. The byte
// pattern comes from Combat Rebalanced by TakeyaSaito (MIT), which patches the same store.
// Registers there: r13 = source event, r12w = hit type, rbp = frame holding the target entity.
// The dedicated server has the same code (game 1287369: client +0x3F065F, server +0x1918FF, all 664
// instructions of the fragment equal), so one DLL serves both. Equipment exists only where a
// character is simulated: the server, or the client when it hosts a local world.
//
// Worn items: the entity's keen::ecs::Equipment component lists them. Entity layout as in the
// game's own getter (game 1287369: client +0x9BF460, server +0x65B2F0) and ShroudForge's ecsReadLayout. The component's
// index differs per build, so it is found by its size instead.
//
// Log: <game>/shroudforge/logs/clash-and-cleave-native.log. Nothing is written into the mod folder.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace {

constexpr char kVersion[] = "0.4.0";

// mov [rbx+0BCh],esi / mov [rbx+0E0h],r12w / mov eax,[r13+0B8h]
const unsigned char kHitSite[] = {0x89, 0xB3, 0xBC, 0x00, 0x00, 0x00, 0x66, 0x44, 0x89, 0xA3, 0xE0,
                                  0x00, 0x00, 0x00, 0x41, 0x8B, 0x85, 0xB8, 0x00, 0x00, 0x00};
// The first two instructions: exactly the size of the absolute jump written over them.
constexpr size_t kPatchSize = 14;

// keen::ecs::HitEvent
constexpr size_t kEventRootTarget = 0xA0;
// Target entity pointer in the hit code's frame (rbp-relative).
constexpr size_t kFrameTargetEntity = 0xC8;
// Entity record.
constexpr size_t kEntityId = 0x10, kEntityLayout = 0x18, kEntityStorage = 0x20, kEntityRow = 0x30;
// Entity layout: presence bits at +0, then per-component u16 offsets and strides.
constexpr size_t kLayoutOffsets = 0x84, kLayoutStrides = 0xA84;
constexpr uint32_t kMaxComponents = 1024;
// keen::ecs::Equipment: an array of ItemImpact, each starting with an ItemInstanceInfo.
constexpr uint32_t kEquipmentSize = 1380;
constexpr size_t kImpactsAt = 8, kImpactSize = 72, kItemIdAt = 8;
constexpr int kImpactSlots = 11;
// Slots seen in game: 0 weapon, 1 shield, 2-6 armour, 7-8 rings, 9 ammo.
constexpr int kWeaponSlot = 0, kShieldSlot = 1;

// keen::impact::HitReactionType
constexpr uint64_t kVeryLight = 1, kLight = 2, kMedium = 3, kHeavy = 4;
// One blow can reach a target as several events within a tick or two.
constexpr uint64_t kSameBlowMs = 50;
constexpr int kMaxPlayers = 16;
constexpr LONG kMaxPoiseLines = 400, kMaxSkipLines = 20;

const char* const kReactions[] = {"None", "VeryLight", "Light", "Medium", "Heavy", "Boss", "LongStun",
                                  "TrapStun", "ShortStun_BeGoneSkill", "ThrowIntoAir", "ThrowIntoAir_OnlyWhenGrounded"};

HANDLE g_log = INVALID_HANDLE_VALUE;
volatile LONG g_poise_lines = 0, g_skip_lines = 0, g_faults = 0;

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

void log_line(const char* text) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[640];
    int n = snprintf(line, sizeof line, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour,
                     t.wMinute, t.wSecond, t.wMilliseconds, text);
    if (n <= 0) return;
    DWORD written;
    WriteFile(g_log, line, static_cast<DWORD>(n < static_cast<int>(sizeof line) ? n : sizeof line - 1), &written, nullptr);
}

struct Hit {
    uint32_t root_target = 0;
    bool has_equipment = false;
    const char* skipped = nullptr;  // why there is no equipment to read
    uint32_t items[kImpactSlots] = {};
};

// Reads the game's memory; a bad guess must end in a log line, never in a crash.
bool read_hit(const uint8_t* event, const uint8_t* frame, Hit& hit) {
    __try {
        hit.root_target = *reinterpret_cast<const uint32_t*>(event + kEventRootTarget);
        const uint8_t* entity = *reinterpret_cast<const uint8_t* const*>(frame + kFrameTargetEntity);
        hit.skipped = "no target entity";
        if (!entity) return true;
        hit.skipped = "another entity in the frame";
        if (*reinterpret_cast<const uint32_t*>(entity + kEntityId) != hit.root_target) return true;
        const uint8_t* layout = *reinterpret_cast<const uint8_t* const*>(entity + kEntityLayout);
        const uint8_t* storage = *reinterpret_cast<const uint8_t* const*>(entity + kEntityStorage);
        uint32_t row = *reinterpret_cast<const uint32_t*>(entity + kEntityRow);
        hit.skipped = "no layout";
        if (!layout || !storage) return true;

        int found = -1;
        for (uint32_t i = 0; i < kMaxComponents; ++i) {
            uint64_t bits = *reinterpret_cast<const uint64_t*>(layout + (i >> 6) * 8);
            if (!((bits >> (i & 63)) & 1)) continue;
            uint16_t stride = *reinterpret_cast<const uint16_t*>(layout + kLayoutStrides + i * 2);
            if (stride != kEquipmentSize) continue;
            found = static_cast<int>(i);
            break;
        }
        hit.skipped = "no Equipment on the target";
        if (found < 0) return true;
        uint16_t offset = *reinterpret_cast<const uint16_t*>(layout + kLayoutOffsets + found * 2);
        const uint8_t* equipment = storage + offset + static_cast<size_t>(row) * kEquipmentSize;
        for (int slot = 0; slot < kImpactSlots; ++slot) {
            hit.items[slot] = *reinterpret_cast<const uint32_t*>(equipment + kImpactsAt + slot * kImpactSize + kItemIdAt);
        }
        hit.has_equipment = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int cost_of(uint64_t reaction) {
    if (reaction == kLight) return g_rules.light;
    if (reaction == kMedium) return g_rules.medium;
    if (reaction == kHeavy) return g_rules.heavy;
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
    int cost = cost_of(reaction);
    if (!g_rules.enabled || cost < 0) return reaction;
    Hit hit;
    if (!read_hit(event, frame, hit)) {
        if (InterlockedIncrement(&g_faults) <= 5) log_line("hit: memory fault while reading the target");
        return reaction;
    }
    if (!hit.has_equipment) {
        // On a client connected to a server this answers whether the hit code runs here at all.
        if (InterlockedIncrement(&g_skip_lines) <= kMaxSkipLines) {
            char text[160];
            snprintf(text, sizeof text, "%s hit, target %u: left alone, %s", kReactions[reaction], hit.root_target, hit.skipped);
            log_line(text);
        }
        return reaction;
    }

    auto weapon = g_points.find(hit.items[kWeaponSlot]);
    bool two_handed = weapon != g_points.end() && weapon->second < 0;
    int full = 0;
    for (int slot = 0; slot < kImpactSlots; ++slot) {
        auto found = g_points.find(hit.items[slot]);
        if (found == g_points.end() || found->second <= 0) continue;
        if (slot == kShieldSlot && two_handed) continue;  // most two-handed weapons already empty this slot
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
        snprintf(text, sizeof text, "%s hit, target %u: poise %d -> %d of %d, %s (weapon %u%s, shield %u)", kReactions[reaction],
                 hit.root_target, before, after, full, absorbed ? "absorbed" : "flinch", hit.items[kWeaponSlot],
                 two_handed ? " two-handed" : "", hit.items[kShieldSlot]);
        log_line(text);
    }
    return absorbed ? kVeryLight : reaction;
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

// The only place in the code section holding the pattern, or nullptr.
unsigned char* find_hit_site(unsigned char* base) {
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    unsigned char* found = nullptr;
    int matches = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        unsigned char* p = base + section[i].VirtualAddress;
        size_t size = section[i].Misc.VirtualSize;
        for (size_t at = 0; at + sizeof kHitSite <= size; ++at) {
            if (p[at] == kHitSite[0] && memcmp(p + at, kHitSite, sizeof kHitSite) == 0) {
                found = p + at;
                ++matches;
            }
        }
    }
    char text[96];
    snprintf(text, sizeof text, "hit site pattern: %d match(es), expected 1", matches);
    log_line(text);
    return matches == 1 ? found : nullptr;
}

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
    put(site, kPatchSize);
    put(kAbsoluteJump, sizeof kAbsoluteJump);
    put(&back, 8);
    FlushInstructionCache(GetCurrentProcess(), stub, kStubSize);

    DWORD old;
    if (!VirtualProtect(site, kPatchSize, PAGE_EXECUTE_READWRITE, &old)) return false;
    uint64_t target = reinterpret_cast<uint64_t>(stub);
    memcpy(site, kAbsoluteJump, sizeof kAbsoluteJump);
    memcpy(site + sizeof kAbsoluteJump, &target, 8);
    VirtualProtect(site, kPatchSize, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, kPatchSize);
    return true;
}

void open_log() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    *slash = 0;
    wchar_t file[MAX_PATH];
    if (swprintf(file, MAX_PATH, L"%s\\shroudforge\\logs\\clash-and-cleave-native.log", path) <= 0) return;
    g_log = CreateFileW(file, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// The data mod's export: a rules line, then one "<itemId> <points>" line per armour piece or shield.
void load_rules() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    *slash = 0;
    wchar_t file[MAX_PATH];
    if (swprintf(file, MAX_PATH, L"%s\\shroudforge\\exports\\clash-and-cleave\\poise.txt", path) <= 0) return;
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

void start() {
    auto base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    char host[MAX_PATH] = "?";
    GetModuleFileNameA(nullptr, host, MAX_PATH);
    const char* name = strrchr(host, '\\');
    char text[MAX_PATH + 96];
    snprintf(text, sizeof text, "clash-and-cleave-native %s loaded in %s, PE TimeDateStamp 0x%08X", kVersion, name ? name + 1 : host,
             nt->FileHeader.TimeDateStamp);
    log_line(text);
    load_rules();
    if (!g_rules.enabled) return;
    unsigned char* site = find_hit_site(base);
    if (!site) {
        log_line("inactive: hit site not found (game version changed?), nothing patched");
        return;
    }
    snprintf(text, sizeof text, "%s at +0x%llX", install(site) ? "poise hook installed" : "poise hook FAILED to install",
             static_cast<unsigned long long>(site - base));
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
