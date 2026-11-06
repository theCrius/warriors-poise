// clash-and-cleave-native 0.3.0: armour-based poise and dodge cost.
//
// Poise: every worn armour piece and shield adds points to a pool, every Light, Medium or Heavy
// hit removes some. While points remain the hit only makes the character twitch; when they run
// out it lands as usual. The pool refills after a few seconds without a poise hit.
//
// Dodge: the stamina cost of a dodge becomes the sum of the worn pieces' dodge points, with a
// minimum. The game checks whether a dodge is affordable against its own value, before this.
//
// The data mod assigns the points per item and exports them with the rules to
// <game>/shroudforge/exports/clash-and-cleave/poise.txt.
//
// Where: the code that fills a keen::ecs::HitEvent, at the store of its hitReaction. That spot is
// inside a function fragment with no entry of its own, so it is patched in place. The byte
// pattern comes from Combat Rebalanced by TakeyaSaito (MIT), which patches the same store.
// Registers there: r13 = source event, r12w = hit type, rbp = frame holding the target entity.
//
// Worn items: the entity's keen::ecs::Equipment component lists them. Entity layout as in the
// game's own getter (+0x9BF460 in game 1287369) and ShroudForge's ecsReadLayout. The component's
// index differs per build, so it is found by its size instead.
//
// Log: <game>/shroudforge/logs/clash-and-cleave-native.log. Nothing is written into the mod folder.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace {

constexpr char kVersion[] = "0.3.1";

// mov [rbx+0BCh],esi / mov [rbx+0E0h],r12w / mov eax,[r13+0B8h]
const unsigned char kHitSite[] = {0x89, 0xB3, 0xBC, 0x00, 0x00, 0x00, 0x66, 0x44, 0x89, 0xA3, 0xE0,
                                  0x00, 0x00, 0x00, 0x41, 0x8B, 0x85, 0xB8, 0x00, 0x00, 0x00};
// The first two instructions: exactly the size of the absolute jump written over them.
constexpr size_t kPatchSize = 14;

// Dodge cost: in the PayUsageCost handler of the actor_pay_usage_cost system, where the stamina
// cost is loaded and negated before it is applied (+0x223B8D in game 1287369). Registers there:
// r15 = the item paying (keen::ItemInfo), r14 = the event, rsi = the system's context, cost at
// [rsp+48h]. mov ebx,[rsp+48h] / mov rcx,[rsi+0E0h] / neg ebx / mov rdi,[rsi+100h]: 20 bytes of
// whole, position independent instructions.
const unsigned char kStaminaCost[] = {0x8B, 0x5C, 0x24, 0x48, 0x48, 0x8B, 0x8E, 0xE0, 0x00, 0x00,
                                      0x00, 0xF7, 0xDB, 0x48, 0x8B, 0xBE, 0x00, 0x01, 0x00, 0x00};
constexpr size_t kStaminaPatchSize = sizeof kStaminaCost;
constexpr uint32_t kAbilityEvade = 406279847, kAbilityWaterDodge = 3770110946;
constexpr LONG kMaxDodgeLines = 300;
// The handler's frame holds the acting entity's component pointers, and at +0 the system's
// iterator: its query at +0 and the next entity's index at +8. The query's per-entity arrays are
// searched for the entity record until the right one is confirmed.
constexpr size_t kContextSearchBytes = 0x140;
constexpr size_t kQuerySearchBytes = 0x850;
constexpr size_t kIteratorIndex = 8;

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
constexpr LONG kMaxPoiseLines = 400;

const char* const kReactions[] = {"None", "VeryLight", "Light", "Medium", "Heavy", "Boss", "LongStun",
                                  "TrapStun", "ShortStun_BeGoneSkill", "ThrowIntoAir", "ThrowIntoAir_OnlyWhenGrounded"};

HANDLE g_log = INVALID_HANDLE_VALUE;
volatile LONG g_poise_lines = 0, g_faults = 0, g_dodge_lines = 0;
volatile LONG g_known_dodge_points = -1;  // from the last pass that could see the entity's equipment
const uint8_t* volatile g_last_hit_entity = nullptr;  // the last player the poise code saw hit

struct Rules {
    bool poise = false, dodge = false;
    int light = 0, medium = 0, heavy = 0;
    uint64_t refill_ms = 0;
    int dodge_minimum = 0;
};
Rules g_rules;
struct ItemPoints {
    int poise = 0;  // negative marks a two-handed weapon
    int dodge = 0;
};
// Armour pieces, shields and two-handed weapons. Read-only once the game runs.
std::unordered_map<uint32_t, ItemPoints> g_points;

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
    const uint8_t* entity = nullptr;
    bool has_equipment = false;
    uint32_t items[kImpactSlots] = {};
};

// Reads the worn items of an entity record. Guarded: callers also pass guesses.
bool read_equipment(const uint8_t* entity, uint32_t* items) {
    __try {
        const uint8_t* layout = *reinterpret_cast<const uint8_t* const*>(entity + kEntityLayout);
        const uint8_t* storage = *reinterpret_cast<const uint8_t* const*>(entity + kEntityStorage);
        uint32_t row = *reinterpret_cast<const uint32_t*>(entity + kEntityRow);
        if (!layout || !storage) return false;
        int found = -1;
        for (uint32_t i = 0; i < kMaxComponents; ++i) {
            uint64_t bits = *reinterpret_cast<const uint64_t*>(layout + (i >> 6) * 8);
            if (!((bits >> (i & 63)) & 1)) continue;
            uint16_t stride = *reinterpret_cast<const uint16_t*>(layout + kLayoutStrides + i * 2);
            if (stride != kEquipmentSize) continue;
            found = static_cast<int>(i);
            break;
        }
        if (found < 0) return false;
        uint16_t offset = *reinterpret_cast<const uint16_t*>(layout + kLayoutOffsets + found * 2);
        const uint8_t* equipment = storage + offset + static_cast<size_t>(row) * kEquipmentSize;
        for (int slot = 0; slot < kImpactSlots; ++slot) {
            items[slot] = *reinterpret_cast<const uint32_t*>(equipment + kImpactsAt + slot * kImpactSize + kItemIdAt);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Reads the game's memory; a bad guess must end in a log line, never in a crash.
bool read_hit(const uint8_t* event, const uint8_t* frame, Hit& hit) {
    __try {
        hit.root_target = *reinterpret_cast<const uint32_t*>(event + kEventRootTarget);
        const uint8_t* entity = *reinterpret_cast<const uint8_t* const*>(frame + kFrameTargetEntity);
        if (!entity) return true;
        if (*reinterpret_cast<const uint32_t*>(entity + kEntityId) != hit.root_target) return true;
        hit.has_equipment = read_equipment(entity, hit.items);
        if (hit.has_equipment) hit.entity = entity;
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

Pool* pool_of(uint32_t target) {
    Pool* free_pool = nullptr;
    for (Pool& pool : g_pools) {
        if (pool.target == target) return &pool;
        if (!free_pool && !pool.target) free_pool = &pool;
    }
    if (free_pool) free_pool->target = target;
    return free_pool;
}

// Called by the stub for every hit. Returns the hit type to store.
uint64_t on_hit(const uint8_t* event, const uint8_t* frame, uint64_t reaction) {
    int cost = cost_of(reaction);
    // The dodge fallback needs the entity even when poise is off.
    if ((!g_rules.poise && !g_rules.dodge) || cost < 0) return reaction;
    Hit hit;
    if (!read_hit(event, frame, hit)) {
        if (InterlockedIncrement(&g_faults) <= 5) log_line("hit: memory fault while reading the target");
        return reaction;
    }
    if (!hit.has_equipment) return reaction;
    g_last_hit_entity = hit.entity;
    if (!g_rules.poise) return reaction;

    auto weapon = g_points.find(hit.items[kWeaponSlot]);
    bool two_handed = weapon != g_points.end() && weapon->second.poise < 0;
    int full = 0;
    for (int slot = 0; slot < kImpactSlots; ++slot) {
        auto found = g_points.find(hit.items[slot]);
        if (found == g_points.end() || found->second.poise <= 0) continue;
        if (slot == kShieldSlot && two_handed) continue;  // most two-handed weapons already empty this slot
        full += found->second.poise;
    }
    uint64_t now = GetTickCount64();
    int before = 0, after = 0;
    bool absorbed = false, repeat = false;
    AcquireSRWLockExclusive(&g_pools_lock);
    Pool* pool = pool_of(hit.root_target);
    if (pool) {
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

bool read_paying_item(const uint8_t* item, uint32_t& id) {
    __try {
        id = item ? *reinterpret_cast<const uint32_t*>(item) : 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_pointer(const uint8_t* address, const uint8_t*& value) {
    __try {
        value = *reinterpret_cast<const uint8_t* const*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_u32(const uint8_t* address, uint32_t& value) {
    __try {
        value = *reinterpret_cast<const uint32_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool plausible(const uint8_t* pointer) { return reinterpret_cast<uintptr_t>(pointer) >= 0x10000; }

// Looks for the acting entity through the iterator's query. Fills where with what worked.
bool find_through_query(const uint8_t* context, uint32_t* items, char* where, size_t where_size) {
    const uint8_t* iterator = nullptr;
    const uint8_t* query = nullptr;
    uint32_t index = 0;
    if (!read_pointer(context, iterator) || !plausible(iterator) || !read_pointer(iterator, query) || !plausible(query) ||
        !read_u32(iterator + kIteratorIndex, index)) {
        return false;
    }
    for (size_t offset = 0; offset < kQuerySearchBytes; offset += 8) {
        const uint8_t* array = nullptr;
        if (!read_pointer(query + offset, array) || !plausible(array)) continue;
        for (uint32_t back = 0; back < 2 && back <= index; ++back) {
            const uint8_t* candidate = nullptr;
            if (!read_pointer(array + static_cast<size_t>(index - back) * 8, candidate) || !plausible(candidate)) continue;
            if (read_equipment(candidate, items)) {
                snprintf(where, where_size, "query+0x%zX[index-%u]", offset, back);
                return true;
            }
        }
    }
    return false;
}

// Sets a dodge's stamina cost from the armour of the entity paying. One dodge passes here more
// than once, not always with its equipment in reach: the last known armour covers those passes.
uint64_t on_stamina_cost(const uint8_t* item, const uint8_t* context, int* cost) {
    uint32_t id = 0;
    if (!read_paying_item(item, id) || (id != kAbilityEvade && id != kAbilityWaterDodge)) return 0;

    char where[48] = "remembered";
    bool found = false;
    uint32_t items[kImpactSlots] = {};
    for (size_t offset = 0; offset < kContextSearchBytes && !found; offset += 8) {
        const uint8_t* candidate = nullptr;
        if (!read_pointer(context + offset, candidate) || !plausible(candidate)) continue;
        if (read_equipment(candidate, items)) {
            found = true;
            snprintf(where, sizeof where, "frame+0x%zX", offset);
        }
    }
    if (!found) found = find_through_query(context, items, where, sizeof where);
    if (!found && plausible(g_last_hit_entity) && read_equipment(g_last_hit_entity, items)) {
        found = true;
        snprintf(where, sizeof where, "last hit");
    }
    if (found) {
        int points = 0;
        for (uint32_t worn : items) {
            auto entry = g_points.find(worn);
            if (entry != g_points.end()) points += entry->second.dodge;
        }
        InterlockedExchange(&g_known_dodge_points, points);
    }
    LONG points = g_known_dodge_points;
    int vanilla = *cost;
    if (points >= 0) *cost = points > g_rules.dodge_minimum ? points : g_rules.dodge_minimum;
    if (InterlockedIncrement(&g_dodge_lines) <= kMaxDodgeLines) {
        char text[128];
        if (points < 0) snprintf(text, sizeof text, "dodge: armour not known yet, cost stays %d", vanilla);
        else snprintf(text, sizeof text, "dodge: cost %d -> %d (armour: %s)", vanilla, *cost, where);
        log_line(text);
    }
    return 0;
}
// Both stubs save what the game code may still need, call their callback and restore everything.
// Flags matter: a conditional jump right after the hit site tests them.
const unsigned char kStubSave[] = {
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
};
// on_hit(r13, rbp, r12w)
const unsigned char kHitArgs[] = {
    0x4C, 0x89, 0xE9,                          // mov rcx,r13
    0x48, 0x89, 0xEA,                          // mov rdx,rbp
    0x4D, 0x0F, 0xB7, 0xC4,                    // movzx r8,r12w
};
// on_stamina_cost(r15, rsi, address of the cost). r11 is the stack pointer after 8 pushes, so the
// game's [rsp+48h] is [r11+88h].
const unsigned char kStaminaArgs[] = {
    0x4C, 0x89, 0xF9,                          // mov rcx,r15
    0x48, 0x89, 0xF2,                          // mov rdx,rsi
    0x4D, 0x8D, 0x83, 0x88, 0x00, 0x00, 0x00,  // lea r8,[r11+88h]
};const unsigned char kLoadRax[] = {0x48, 0xB8};  // mov rax,<callback>
const unsigned char kCallRax[] = {0xFF, 0xD0};  // call rax
const unsigned char kStoreHitType[] = {0x66, 0x41, 0x89, 0xC4};  // mov r12w,ax
const unsigned char kStubRestore[] = {
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
unsigned char* find_unique(unsigned char* base, const unsigned char* pattern, size_t length, const char* name) {
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    unsigned char* found = nullptr;
    int matches = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        unsigned char* p = base + section[i].VirtualAddress;
        size_t size = section[i].Misc.VirtualSize;
        for (size_t at = 0; at + length <= size; ++at) {
            if (p[at] == pattern[0] && memcmp(p + at, pattern, length) == 0) {
                found = p + at;
                ++matches;
            }
        }
    }
    char text[96];
    snprintf(text, sizeof text, "%s pattern: %d match(es), expected 1", name, matches);
    log_line(text);
    return matches == 1 ? found : nullptr;
}

bool install(unsigned char* site, size_t patch_size, const unsigned char* args, size_t args_size, void* callback_function, bool store_hit_type) {
    constexpr size_t kMaxStub = 256;
    auto stub = static_cast<unsigned char*>(VirtualAlloc(nullptr, kMaxStub, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub) return false;
    unsigned char* p = stub;
    auto put = [&p](const void* bytes, size_t size) {
        memcpy(p, bytes, size);
        p += size;
    };
    uint64_t callback = reinterpret_cast<uint64_t>(callback_function);
    uint64_t back = reinterpret_cast<uint64_t>(site) + patch_size;
    put(kStubSave, sizeof kStubSave);
    put(args, args_size);
    put(kLoadRax, sizeof kLoadRax);
    put(&callback, 8);
    put(kCallRax, sizeof kCallRax);
    if (store_hit_type) put(kStoreHitType, sizeof kStoreHitType);
    put(kStubRestore, sizeof kStubRestore);
    put(site, patch_size);
    put(kAbsoluteJump, sizeof kAbsoluteJump);
    put(&back, 8);
    FlushInstructionCache(GetCurrentProcess(), stub, kMaxStub);

    DWORD old;
    if (!VirtualProtect(site, patch_size, PAGE_EXECUTE_READWRITE, &old)) return false;
    uint64_t target = reinterpret_cast<uint64_t>(stub);
    memcpy(site, kAbsoluteJump, sizeof kAbsoluteJump);
    memcpy(site + sizeof kAbsoluteJump, &target, 8);
    memset(site + sizeof kAbsoluteJump + 8, 0x90, patch_size - sizeof kAbsoluteJump - 8);  // nop out the rest
    VirtualProtect(site, patch_size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, patch_size);
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
    int poise = 0, dodge = 0, refill = 0;
    if (fgets(line, sizeof line, f) && sscanf_s(line, "poise %d light=%d medium=%d heavy=%d refill=%d dodge %d minimum=%d", &poise,
                                                &g_rules.light, &g_rules.medium, &g_rules.heavy, &refill, &dodge, &g_rules.dodge_minimum) == 7) {
        while (fgets(line, sizeof line, f)) {
            unsigned long id = 0;
            ItemPoints points;
            if (sscanf_s(line, "%lu %d %d", &id, &points.poise, &points.dodge) == 3) g_points[static_cast<uint32_t>(id)] = points;
        }
        g_rules.refill_ms = refill > 0 ? static_cast<uint64_t>(refill) : 0;
        g_rules.poise = poise == 1 && !g_points.empty();
        g_rules.dodge = dodge == 1 && !g_points.empty();
    }
    fclose(f);
    char text[200];
    snprintf(text, sizeof text, "%zu items; poise %s: costs light %d medium %d heavy %d, refill %llu ms; dodge cost %s, minimum %d", g_points.size(),
             g_rules.poise ? "on" : "off", g_rules.light, g_rules.medium, g_rules.heavy, static_cast<unsigned long long>(g_rules.refill_ms),
             g_rules.dodge ? "on" : "off", g_rules.dodge_minimum);
    log_line(text);
}

void start() {
    auto base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    char text[128];
    snprintf(text, sizeof text, "clash-and-cleave-native %s loaded, PE TimeDateStamp 0x%08X", kVersion, nt->FileHeader.TimeDateStamp);
    log_line(text);
    load_rules();
    if (g_rules.poise || g_rules.dodge) {
        unsigned char* site = find_unique(base, kHitSite, sizeof kHitSite, "hit site");
        if (site) {
            snprintf(text, sizeof text, "%s at +0x%llX",
                     install(site, kPatchSize, kHitArgs, sizeof kHitArgs, reinterpret_cast<void*>(&on_hit), true) ? "poise hook installed" : "poise hook FAILED to install",
                     static_cast<unsigned long long>(site - base));
            log_line(text);
        } else {
            log_line("poise inactive: hit site not found (game version changed?), nothing patched");
        }
    }
    if (g_rules.dodge) {
        unsigned char* stamina = find_unique(base, kStaminaCost, sizeof kStaminaCost, "stamina cost");
        if (stamina) {
            snprintf(text, sizeof text, "%s at +0x%llX",
                     install(stamina, kStaminaPatchSize, kStaminaArgs, sizeof kStaminaArgs, reinterpret_cast<void*>(&on_stamina_cost), false)
                         ? "dodge hook installed" : "dodge hook FAILED to install",
                     static_cast<unsigned long long>(stamina - base));
            log_line(text);
        } else {
            log_line("dodge cost inactive: stamina cost site not found (game version changed?), nothing patched");
        }
    }
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
