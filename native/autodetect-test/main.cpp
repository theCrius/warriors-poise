// autodetect-test: runs the DLL's detection on a game executable on disk, mapped as an image (no
// code runs), and prints what it finds. The data layout comes from a ShroudForge type dump given
// as the second argument (any dump; its identity is not checked here), or else from the built-in
// values of the tested builds. Also prints the file's SHA-256 and whether it is a tested build.
// Usage: autodetect-test <enshrouded.exe | enshrouded_server.exe> [types-*.json]
// Exit code: 0 all found, 3 a code rule failed, 4 the dump could not be read.
#include "../warriors-poise-native/autodetect.h"
#include "../warriors-poise-native/typedump.h"

#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fwprintf(stderr, L"usage: autodetect-test <exe> [types.json]\n");
        return 2;
    }
    DWORD t0 = GetTickCount();
    std::string sha = typedump::file_sha256(argv[1]);
    bool tested = false;
    for (const char* t : typedump::kTestedExecutables) tested = tested || sha == t;
    std::printf("sha256 %s (%s, %lu ms)\n", sha.c_str(), tested ? "tested build" : "not a tested build", GetTickCount() - t0);

    typedump::Layout l = typedump::tested_layout();
    if (argc >= 3) {
        std::string json, why;
        t0 = GetTickCount();
        if (!typedump::read_file(argv[2], json, 256u << 20) || !typedump::from_json(json, l, why)) {
            std::printf("type dump: NOT usable: %s\n", why.empty() ? "unreadable" : why.c_str());
            return 4;
        }
        std::printf("type dump read in %lu ms\n", GetTickCount() - t0);
    }
    std::printf("layout (%s): Equipment %u B, impacts +%u, %u slots of %u B, itemId +%u, main hand %u, shield %u; "
                "HitEvent rootTargetId +0x%X, hitReaction +0x%X; VeryLight %u Light %u Medium %u Heavy %u; %zu reaction names\n",
                l.source.c_str(), l.equipment_size, l.impacts_at, l.impact_slots, l.impact_size, l.item_id_at, l.weapon_slot, l.shield_slot,
                l.event_root_target, l.event_hit_reaction, l.very_light, l.light, l.medium, l.heavy, l.reactions.size());

    HMODULE h = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!h) {
        std::fwprintf(stderr, L"cannot map %s (error %lu)\n", argv[1], GetLastError());
        return 1;
    }
    // A module loaded as an image resource has flag bits in the low bits of its handle.
    auto base = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(h) & ~static_cast<uintptr_t>(3));
    autodetect::Image im;
    if (!autodetect::open(base, im)) {
        std::fprintf(stderr, "not a 64-bit executable with code and unwind data\n");
        return 1;
    }
    autodetect::Found f;
    std::vector<std::string> notes;
    t0 = GetTickCount();
    bool ok = autodetect::detect(im, l.event_hit_reaction, f, notes);
    DWORD ms = GetTickCount() - t0;
    for (const std::string& n : notes) std::printf("  %s\n", n.c_str());
    std::printf("result: %s (hit site +0x%X, getter +0x%X, target [rbp+0x%X]; %lu ms)\n", ok ? "all found" : "NOT found", f.hit_site, f.getter,
                f.frame_target, ms);
    FreeLibrary(h);
    return ok ? 0 : 3;
}
