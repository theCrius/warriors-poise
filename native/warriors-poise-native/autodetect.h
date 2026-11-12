// Finds the DLL's places in the game's code by rules, not by fixed addresses. Every rule was
// checked on the client and dedicated-server executables of game versions 1283531 and 1287369
// with native/autodetect-test. A rule that finds nothing, or more than one candidate, fails, and
// the DLL patches nothing.
//
//   1. hit site: the store of a HitEvent's hitReaction while the event is copied:
//        mov [rbx+X],esi / mov [rbx+R],r12w / mov eax,[r13+Y]
//      with R = the hitReaction offset (from the type dump). The registers are fixed: the hook
//      reads the hit type from r12w and the source event from r13. Exactly one match.
//   2. component getter: the only match of its body, which reads the entity layout; the entity
//      record's layout, storage and row offsets and the layout's offset and stride tables are
//      read from its instructions (ShroudForge's ecsReadLayout holds the same values).
//   3. target entity: the getter calls in the hit site's function whose entity argument (r9)
//      comes from the frame, [rbp+F]; all must agree on F.
//
// Works on any image mapped with its sections at their RVAs (the game itself, or a file loaded
// as an image resource by the test tool). Reads only; no code runs.
//
// Image, Pattern and function_start follow Elemental Armaments' autodetect.h (same author).
#pragma once
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace autodetect {

struct Found {
    uint32_t hit_site = 0, getter = 0;  // RVAs (0 = not found)
    // entity record and layout, read from the getter
    uint32_t entity_layout = 0, entity_storage = 0, entity_row = 0, layout_offsets = 0, layout_strides = 0;
    uint32_t frame_target = 0;  // rbp-relative slot holding the target entity at the hit site
    bool ok = false;
};

struct Image {
    const uint8_t* base = nullptr;
    uint32_t text_rva = 0, text_size = 0;
    const IMAGE_RUNTIME_FUNCTION_ENTRY* pdata = nullptr;
    size_t pdata_count = 0;
};

inline bool open(const uint8_t* base, Image& im) {
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            im.text_rva = sec[i].VirtualAddress;
            im.text_size = sec[i].Misc.VirtualSize;
            break;
        }
    }
    const IMAGE_DATA_DIRECTORY& ex = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    im.base = base;
    im.pdata = reinterpret_cast<const IMAGE_RUNTIME_FUNCTION_ENTRY*>(base + ex.VirtualAddress);
    im.pdata_count = ex.Size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);
    return im.text_rva != 0 && im.text_size > 16 && im.pdata_count > 0;
}

// A byte pattern written as hex with "??" for any byte.
struct Pattern {
    std::vector<uint8_t> bytes, mask;  // mask 1 = must match
    explicit Pattern(const char* text) {
        for (const char* p = text; *p;) {
            if (*p == ' ') { ++p; continue; }
            if (p[0] == '?' && p[1] == '?') { bytes.push_back(0); mask.push_back(0); p += 2; continue; }
            auto hex = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
            bytes.push_back(static_cast<uint8_t>(hex(p[0]) << 4 | hex(p[1])));
            mask.push_back(1);
            p += 2;
        }
    }
    bool at(const uint8_t* p) const {
        for (size_t i = 0; i < bytes.size(); ++i) {
            if (mask[i] && p[i] != bytes[i]) return false;
        }
        return true;
    }
};

// RVAs of every place in the code section where the pattern starts (its first byte is fixed).
inline std::vector<uint32_t> find_all(const Image& im, const Pattern& pat) {
    std::vector<uint32_t> hits;
    const uint8_t* p = im.base + im.text_rva;
    const uint8_t* end = p + im.text_size - pat.bytes.size();
    while (p <= end) {
        p = static_cast<const uint8_t*>(memchr(p, pat.bytes[0], static_cast<size_t>(end - p) + 1));
        if (!p) break;
        if (pat.at(p)) hits.push_back(static_cast<uint32_t>(p - im.base));
        ++p;
    }
    return hits;
}

inline uint32_t u32_at(const Image& im, uint32_t rva) {
    uint32_t v;
    memcpy(&v, im.base + rva, 4);
    return v;
}

// Start of the function containing rva, following chained unwind entries to the primary one.
inline uint32_t function_start(const Image& im, uint32_t rva) {
    size_t lo = 0, hi = im.pdata_count;
    const IMAGE_RUNTIME_FUNCTION_ENTRY* f = nullptr;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (rva < im.pdata[mid].BeginAddress) hi = mid;
        else if (rva >= im.pdata[mid].EndAddress) lo = mid + 1;
        else { f = &im.pdata[mid]; break; }
    }
    for (int depth = 0; f && depth < 32; ++depth) {
        const uint8_t* info = im.base + f->UnwindData;
        uint8_t flags = info[0] >> 3;
        if (!(flags & UNW_FLAG_CHAININFO)) return f->BeginAddress;
        uint8_t codes = info[2];
        f = reinterpret_cast<const IMAGE_RUNTIME_FUNCTION_ENTRY*>(info + 4 + ((codes + 1) & ~1) * 2);
    }
    return f ? f->BeginAddress : 0;
}

inline void note(std::vector<std::string>& notes, const char* fmt, ...) {
    char text[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    notes.emplace_back(text);
}

// Rule 3 helper: the frame slot passed as the getter's entity argument at the call `site`, or 0.
// Accepts "mov r9,[rbp+F]" or "mov reg,[rbp+F]" followed by "mov r9,reg", within 0x20 bytes.
inline uint32_t frame_argument(const Image& im, uint32_t site) {
    uint32_t from = site > 0x20 ? site - 0x20 : 0;
    for (uint32_t at = from; at + 7 <= site; ++at) {
        const uint8_t* p = im.base + at;
        if (p[0] == 0x4C && p[1] == 0x8B && p[2] == 0x8D) return u32_at(im, at + 3);  // mov r9,[rbp+disp32]
        if (p[0] != 0x48 || p[1] != 0x8B || (p[2] & 0xC7) != 0x85) continue;          // mov reg,[rbp+disp32]
        uint8_t reg = (p[2] >> 3) & 7;
        if (reg == 4 || reg == 5) continue;  // rsp and rbp are never the argument's carrier
        for (uint32_t m = at + 7; m + 3 <= site; ++m) {
            const uint8_t* q = im.base + m;
            if (q[0] == 0x4C && q[1] == 0x8B && q[2] == (0xC8 | reg)) return u32_at(im, at + 3);  // mov r9,reg
        }
    }
    return 0;
}

// hit_reaction_at: the HitEvent.hitReaction offset the store must use.
inline bool detect(const Image& im, uint32_t hit_reaction_at, Found& f, std::vector<std::string>& notes) {
    // 1. hit site
    static const Pattern kSite("89 B3 ?? ?? ?? ?? 66 44 89 A3 ?? ?? ?? ?? 41 8B 85 ?? ?? ?? ??");
    std::vector<uint32_t> sites = find_all(im, kSite);
    int matching = 0;
    for (uint32_t s : sites) {
        if (u32_at(im, s + 10) == hit_reaction_at) {
            f.hit_site = s;
            ++matching;
        }
    }
    note(notes, "hit site: %zu store pattern(s), %d storing hitReaction (+0x%X)%s", sites.size(), matching, hit_reaction_at,
         matching == 1 ? "" : ", expected 1");
    if (matching != 1) f.hit_site = 0;

    // 2. component getter
    static const Pattern kGetter("44 0F B7 08 4D 8B 42 ?? 41 8B C1 48 C1 E8 06 41 0F B6 C9 49 8B 14 C0 48 D3 EA F6 C2 01 75 ?? "
                                 "0F 10 05 ?? ?? ?? ?? 66 48 0F 7E C2 EB ?? 43 0F B7 8C 48 ?? ?? ?? ?? 41 8B 42 ?? "
                                 "43 0F B7 94 48 ?? ?? ?? ?? 48 0F AF C1 48 89 4C 24 ?? 48 03 D0 49 03 52 ??");
    std::vector<uint32_t> bodies = find_all(im, kGetter);
    if (bodies.size() == 1) {
        uint32_t b = bodies[0];
        f.getter = function_start(im, b);
        f.entity_layout = im.base[b + 7];
        f.layout_strides = u32_at(im, b + 50);
        f.entity_row = im.base[b + 57];
        f.layout_offsets = u32_at(im, b + 63);
        f.entity_storage = im.base[b + 82];
        note(notes, "component getter: +0x%X; entity layout +0x%X, storage +0x%X, row +0x%X; layout offsets +0x%X, strides +0x%X",
             f.getter, f.entity_layout, f.entity_storage, f.entity_row, f.layout_offsets, f.layout_strides);
        // The tables follow the presence bits and each other; anything else is not this layout.
        if (!f.getter || f.layout_offsets < 8 || f.layout_strides <= f.layout_offsets || f.layout_strides - f.layout_offsets > 0x2000) {
            note(notes, "component getter: offsets do not look like an entity layout");
            f.getter = 0;
        }
    } else {
        note(notes, "component getter: %zu match(es), expected 1", bodies.size());
    }

    // 3. target entity in the hit site's frame
    if (f.hit_site && f.getter) {
        uint32_t function = function_start(im, f.hit_site);
        const uint8_t* p = im.base + im.text_rva;
        uint32_t from = f.hit_site > 0x2000 ? f.hit_site - 0x2000 : im.text_rva;
        int calls = 0, from_frame = 0;
        bool agree = true;
        for (uint32_t site = from; site < f.hit_site; ++site) {
            if (p[site - im.text_rva] != 0xE8) continue;
            int32_t rel;
            memcpy(&rel, p + (site - im.text_rva) + 1, 4);
            if (site + 5 + static_cast<uint32_t>(rel) != f.getter || function_start(im, site) != function) continue;
            ++calls;
            uint32_t slot = frame_argument(im, site);
            if (!slot) continue;
            if (from_frame && slot != f.frame_target) agree = false;
            f.frame_target = slot;
            ++from_frame;
        }
        note(notes, "target entity: %d getter call(s) before the hit site, %d with the entity from the frame, slot [rbp+0x%X]%s", calls,
             from_frame, f.frame_target, from_frame >= 1 && agree ? "" : " (rule failed)");
        if (!from_frame || !agree) f.frame_target = 0;
    }

    f.ok = f.hit_site && f.getter && f.frame_target;
    return f.ok;
}

}  // namespace autodetect
