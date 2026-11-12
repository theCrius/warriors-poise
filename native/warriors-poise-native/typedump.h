// Reads the data layout the DLL needs from ShroudForge's reflected type dump of the running
// executable: <game>/shroudforge/cache/types-<exe name>.json, with types-<exe name>.identity.json
// naming the executable's SHA-256. ShroudForge writes both when the executable changes, before
// mods run, so after a game update the dump describes the new build. The dump is used only when
// the identity matches the executable on disk.
//
// Read from it: keen::ecs::Equipment (size, equipmentImpacts), the static array of
// keen::ecs::ItemImpact (slot count, element size, itemInstance), keen::ItemInstanceInfo (itemId),
// keen::ecs::HitEvent (rootTargetId, hitReaction), keen::impact::HitReactionType (value of every
// name) and keen::ImpactEquippedItems (which slot is the main hand, which the shield).
//
// The file is about 20 MB of JSON; one pass finds the entries of the wanted types, and only those
// are parsed. Reads only.
#pragma once
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace typedump {

// What the hook needs to know about the game's data. Built-in values (dllmain.cpp) have the same shape.
struct Layout {
    uint32_t equipment_size = 0, impacts_at = 0, impact_size = 0, impact_slots = 0, item_id_at = 0;
    uint32_t weapon_slot = 0, shield_slot = 0;
    uint32_t event_root_target = 0, event_hit_reaction = 0;
    uint32_t very_light = 0, light = 0, medium = 0, heavy = 0;
    std::vector<std::string> reactions;  // name of every HitReactionType value (empty name = unused)
    std::string source;                  // where the values came from, for the log
};

// Executables the hook was tested on in game, by SHA-256: game 1287369, client and dedicated
// server (2026-11-06). Without a usable dump the built-in layout is used for these only.
constexpr const char* kTestedExecutables[] = {
    "5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77",  // enshrouded.exe 1287369
    "f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2",  // enshrouded_server.exe 1287369
};

// The layout of the tested builds (ShroudForge's type dump of game 1287369, client and server).
inline Layout tested_layout() {
    Layout l;
    l.equipment_size = 1380;
    l.impacts_at = 8;
    l.impact_size = 72;
    l.impact_slots = 11;
    l.item_id_at = 8;  // ItemImpact.itemInstance +0, ItemInstanceInfo.itemId +8
    l.weapon_slot = 0;
    l.shield_slot = 1;
    l.event_root_target = 0xA0;
    l.event_hit_reaction = 0xE0;
    l.very_light = 1;
    l.light = 2;
    l.medium = 3;
    l.heavy = 4;
    l.reactions = {"None", "VeryLight", "Light", "Medium", "Heavy", "Boss", "LongStun", "TrapStun", "ShortStun_BeGoneSkill",
                   "ThrowIntoAir", "ThrowIntoAir_OnlyWhenGrounded"};
    l.source = "built-in values of a tested build";
    return l;
}

// ---- SHA-256 (FIPS 180-4), so the DLL needs no crypto library while it loads ----

class Sha256 {
public:
    Sha256() { reset(); }
    void reset() {
        static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        memcpy(h_, init, sizeof h_);
        used_ = 0;
        total_ = 0;
    }
    void update(const uint8_t* data, size_t size) {
        total_ += size;
        if (used_) {
            size_t take = (std::min)(size, sizeof block_ - used_);
            memcpy(block_ + used_, data, take);
            used_ += take;
            data += take;
            size -= take;
            if (used_ < sizeof block_) return;
            compress(block_);
            used_ = 0;
        }
        for (; size >= 64; data += 64, size -= 64) compress(data);
        memcpy(block_, data, size);
        used_ = size;
    }
    std::string hex() {
        uint64_t bits = total_ * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (used_ != 56) update(&zero, 1);
        uint8_t length[8];
        for (int i = 0; i < 8; ++i) length[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
        update(length, 8);
        char out[65];
        for (int i = 0; i < 8; ++i) snprintf(out + i * 8, 9, "%08x", h_[i]);
        return std::string(out, 64);
    }

private:
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void compress(const uint8_t* p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
            0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
    }
    uint32_t h_[8];
    uint8_t block_[64];
    size_t used_;
    uint64_t total_;
};

// Lower-case hex SHA-256 of a file, or "" if it cannot be read.
inline std::string file_sha256(const wchar_t* path) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return std::string();
    Sha256 sha;
    std::unique_ptr<uint8_t[]> buf(new uint8_t[1 << 20]);
    DWORD got = 0;
    bool ok = true;
    while ((ok = ReadFile(f, buf.get(), 1 << 20, &got, nullptr) != FALSE) && got) sha.update(buf.get(), got);
    CloseHandle(f);
    return ok ? sha.hex() : std::string();
}

inline bool read_file(const wchar_t* path, std::string& out, size_t limit) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && static_cast<uint64_t>(size.QuadPart) <= limit;
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size();
    }
    CloseHandle(f);
    return ok;
}

// ---- a small JSON reader, enough for one type entry ----

struct Value {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    double number = 0;
    std::string text;
    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> fields;

    const Value* get(const char* key) const {
        if (kind != Object) return nullptr;
        for (const auto& f : fields) {
            if (f.first == key) return &f.second;
        }
        return nullptr;
    }
    // A whole, non-negative number below 2^32, or false.
    bool u32(uint32_t& out) const {
        if (kind != Number || number < 0 || number > 4294967295.0 || number != static_cast<double>(static_cast<uint64_t>(number))) return false;
        out = static_cast<uint32_t>(number);
        return true;
    }
};

class Parser {
public:
    Parser(const char* p, const char* end) : p_(p), end_(end) {}
    bool parse(Value& v, int depth = 0) {
        if (depth > 32) return false;
        skip();
        if (p_ >= end_) return false;
        switch (*p_) {
        case '{': {
            v.kind = Value::Object;
            ++p_;
            skip();
            if (p_ < end_ && *p_ == '}') { ++p_; return true; }
            for (;;) {
                std::string key;
                skip();
                if (!string(key)) return false;
                skip();
                if (p_ >= end_ || *p_ != ':') return false;
                ++p_;
                v.fields.emplace_back(std::move(key), Value());
                if (!parse(v.fields.back().second, depth + 1)) return false;
                skip();
                if (p_ < end_ && *p_ == ',') { ++p_; continue; }
                if (p_ < end_ && *p_ == '}') { ++p_; return true; }
                return false;
            }
        }
        case '[': {
            v.kind = Value::Array;
            ++p_;
            skip();
            if (p_ < end_ && *p_ == ']') { ++p_; return true; }
            for (;;) {
                v.items.emplace_back();
                if (!parse(v.items.back(), depth + 1)) return false;
                skip();
                if (p_ < end_ && *p_ == ',') { ++p_; continue; }
                if (p_ < end_ && *p_ == ']') { ++p_; return true; }
                return false;
            }
        }
        case '"':
            v.kind = Value::String;
            return string(v.text);
        case 't': case 'f': case 'n': {
            const char* word = *p_ == 't' ? "true" : *p_ == 'f' ? "false" : "null";
            size_t n = strlen(word);
            if (static_cast<size_t>(end_ - p_) < n || memcmp(p_, word, n) != 0) return false;
            v.kind = *p_ == 'n' ? Value::Null : Value::Bool;
            v.number = *p_ == 't' ? 1 : 0;
            p_ += n;
            return true;
        }
        default: {
            char* stop = nullptr;
            std::string number(p_, (std::min)(static_cast<size_t>(end_ - p_), static_cast<size_t>(32)));
            v.number = strtod(number.c_str(), &stop);
            if (stop == number.c_str()) return false;
            v.kind = Value::Number;
            p_ += stop - number.c_str();
            return true;
        }
        }
    }

private:
    void skip() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n')) ++p_;
    }
    // Escapes are kept as written: type names and keys never need them.
    bool string(std::string& out) {
        if (p_ >= end_ || *p_ != '"') return false;
        const char* start = ++p_;
        while (p_ < end_ && *p_ != '"') p_ += *p_ == '\\' ? 2 : 1;
        if (p_ >= end_) return false;
        out.assign(start, p_);
        ++p_;
        return true;
    }
    const char* p_;
    const char* end_;
};

// The entries of the "types" array whose qualifiedName is wanted (exactly, or by prefix when the
// wanted name ends in '*'), parsed. One pass over the file, strings skipped whole.
inline std::map<std::string, Value> find_types(const std::string& json, const std::vector<std::string>& wanted) {
    std::map<std::string, Value> out;
    const char* s = json.data();
    size_t n = json.size(), element = 0;
    int depth = 0;
    std::string pending;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (c == '"') {
            size_t end = i + 1;
            while (end < n && s[end] != '"') end += s[end] == '\\' ? 2 : 1;
            if (end >= n) break;
            // At an entry's own level: "qualifiedName": "<name>"
            if (depth == 3 && end - i - 1 == 13 && memcmp(s + i + 1, "qualifiedName", 13) == 0) {
                size_t v = end + 1;
                while (v < n && (s[v] == ' ' || s[v] == ':' || s[v] == '\t' || s[v] == '\r' || s[v] == '\n')) ++v;
                if (v < n && s[v] == '"') {
                    size_t ve = v + 1;
                    while (ve < n && s[ve] != '"') ve += s[ve] == '\\' ? 2 : 1;
                    std::string name(s + v + 1, ve < n ? ve - v - 1 : 0);
                    for (const std::string& w : wanted) {
                        bool prefix = !w.empty() && w.back() == '*';
                        if (prefix ? name.compare(0, w.size() - 1, w, 0, w.size() - 1) == 0 : name == w) pending = name;
                    }
                }
            }
            i = end;
            continue;
        }
        if (c == '{' || c == '[') {
            ++depth;
            if (depth == 3 && c == '{') {
                element = i;
                pending.clear();
            }
        } else if (c == '}' || c == ']') {
            if (depth == 3 && c == '}' && !pending.empty()) {
                Value v;
                Parser parser(s + element, s + i + 1);
                if (parser.parse(v) && !out.count(pending)) out.emplace(pending, std::move(v));
                pending.clear();
            }
            --depth;
        }
    }
    return out;
}

// Path of <game>/shroudforge/cache/types-<exe stem><suffix>.
inline bool cache_path(const wchar_t* exe, const wchar_t* suffix, wchar_t* out, size_t size) {
    const wchar_t* slash = wcsrchr(exe, L'\\');
    if (!slash) return false;
    std::wstring folder(exe, slash), stem(slash + 1);
    size_t dot = stem.rfind(L'.');
    if (dot != std::wstring::npos) stem.resize(dot);
    return swprintf(out, size, L"%s\\shroudforge\\cache\\types-%s%s", folder.c_str(), stem.c_str(), suffix) > 0;
}

inline bool field_offset(const Value* type, const char* field, uint32_t& out, uint32_t* type_index = nullptr) {
    const Value* fields = type ? type->get("structFields") : nullptr;
    const Value* f = fields ? fields->get(field) : nullptr;
    const Value* at = f ? f->get("dataOffset") : nullptr;
    if (!at || !at->u32(out)) return false;
    if (type_index) {
        const Value* t = f->get("type");
        if (!t || !t->u32(*type_index)) return false;
    }
    return true;
}

inline bool number(const Value* v, const char* key, uint32_t& out) {
    const Value* x = v ? v->get(key) : nullptr;
    return x && x->u32(out);
}

inline bool enum_value(const Value* type, const char* name, uint32_t& out) {
    const Value* fields = type ? type->get("enumFields") : nullptr;
    const Value* f = fields ? fields->get(name) : nullptr;
    return f && number(f, "value", out);
}

// Reads the layout from a dump's text, without checking whom it belongs to.
inline bool from_json(const std::string& json, Layout& l, std::string& why) {
    auto types = find_types(json, {"keen::ecs::Equipment", "keen::ecs::ItemImpact", "keen::ItemInstanceInfo", "keen::ecs::HitEvent",
                                   "keen::impact::HitReactionType", "keen::ImpactEquippedItems", "keen::StaticArray<keen::ecs::ItemImpact,*"});
    auto type = [&types](const char* name) -> const Value* {
        auto it = types.find(name);
        return it == types.end() ? nullptr : &it->second;
    };
    const Value* equipment = type("keen::ecs::Equipment");
    const Value* impact = type("keen::ecs::ItemImpact");
    const Value* instance = type("keen::ItemInstanceInfo");
    const Value* event = type("keen::ecs::HitEvent");
    const Value* reaction = type("keen::impact::HitReactionType");
    const Value* slots = type("keen::ImpactEquippedItems");
    const Value* array = nullptr;
    for (const auto& t : types) {
        if (t.first.rfind("keen::StaticArray<keen::ecs::ItemImpact,", 0) == 0) array = &t.second;
    }

    uint32_t impacts_type = 0, array_index = 0, array_inner = 0, array_size = 0, impact_index = 0, instance_type = 0, instance_index = 0;
    uint32_t instance_at = 0, reaction_type = 0, reaction_index = 0, reaction_size = 0, count = 0;
    struct Check { bool ok; const char* what; } checks[] = {
        {equipment && number(equipment, "size", l.equipment_size), "Equipment size"},
        {field_offset(equipment, "equipmentImpacts", l.impacts_at, &impacts_type), "Equipment.equipmentImpacts"},
        {array && number(array, "index", array_index) && array_index == impacts_type, "the ItemImpact array type"},
        {number(array, "fieldCount", l.impact_slots) && number(array, "innerType", array_inner) && number(array, "size", array_size),
         "the ItemImpact array's count and size"},
        {impact && number(impact, "size", l.impact_size) && number(impact, "index", impact_index) && impact_index == array_inner,
         "ItemImpact size"},
        {field_offset(impact, "itemInstance", instance_at, &instance_type), "ItemImpact.itemInstance"},
        {instance && number(instance, "index", instance_index) && instance_index == instance_type, "ItemInstanceInfo"},
        {field_offset(instance, "itemId", l.item_id_at), "ItemInstanceInfo.itemId"},
        {field_offset(event, "rootTargetId", l.event_root_target), "HitEvent.rootTargetId"},
        {field_offset(event, "hitReaction", l.event_hit_reaction, &reaction_type), "HitEvent.hitReaction"},
        {reaction && number(reaction, "index", reaction_index) && reaction_index == reaction_type && number(reaction, "size", reaction_size) &&
             reaction_size == 2, "HitReactionType (a 16-bit enum)"},
        {enum_value(reaction, "VeryLight", l.very_light) && enum_value(reaction, "Light", l.light) && enum_value(reaction, "Medium", l.medium) &&
             enum_value(reaction, "Heavy", l.heavy), "HitReactionType's VeryLight, Light, Medium, Heavy"},
        {enum_value(slots, "MainHand", l.weapon_slot) && enum_value(slots, "Shield", l.shield_slot) && enum_value(slots, "Count", count),
         "ImpactEquippedItems' MainHand, Shield, Count"},
    };
    for (const Check& c : checks) {
        if (!c.ok) {
            why = std::string("type dump lacks ") + c.what;
            return false;
        }
    }
    l.item_id_at += instance_at;
    // The pieces must fit together as they do in the builds the hook was tested on.
    if (count != l.impact_slots || array_size != l.impact_slots * l.impact_size || l.impacts_at + array_size > l.equipment_size ||
        l.weapon_slot >= l.impact_slots || l.shield_slot >= l.impact_slots || l.item_id_at + 4 > l.impact_size) {
        why = "type dump: Equipment's parts do not fit together";
        return false;
    }
    const Value* names = reaction->get("enumFields");
    for (const auto& f : names->fields) {
        uint32_t v = 0;
        if (!number(&f.second, "value", v) || v >= 64) continue;
        if (l.reactions.size() <= v) l.reactions.resize(v + 1);
        l.reactions[v] = f.first;
    }
    // The dump's own "version" (game build) sits before "types"; read it without parsing the whole file.
    l.source = "ShroudForge type dump";
    size_t at = json.find("\"version\"");
    if (at != std::string::npos && at < 256) {
        Value version;
        size_t colon = json.find(':', at);
        Parser vp(json.data() + colon + 1, json.data() + (std::min)(json.size(), colon + 200));
        if (vp.parse(version) && version.kind == Value::String) l.source += ", game " + version.text.substr(0, version.text.find('|'));
    }
    return true;
}

// Reads the layout for the executable `exe` (full path, SHA-256 `exe_sha` from file_sha256),
// only from a dump made for that exact file. On failure `why` says what was missing.
inline bool load(const wchar_t* exe, const std::string& exe_sha, Layout& l, std::string& why) {
    wchar_t identity_path[MAX_PATH], dump_path[MAX_PATH];
    if (!cache_path(exe, L".identity.json", identity_path, MAX_PATH) || !cache_path(exe, L".json", dump_path, MAX_PATH)) {
        why = "no path";
        return false;
    }
    std::string identity;
    if (!read_file(identity_path, identity, 1 << 16)) {
        why = "no type dump identity file";
        return false;
    }
    Value id;
    Parser idp(identity.data(), identity.data() + identity.size());
    const Value* sha = idp.parse(id) ? id.get("executableSha256") : nullptr;
    if (!sha || sha->kind != Value::String || exe_sha.empty() || _stricmp(sha->text.c_str(), exe_sha.c_str()) != 0) {
        why = "the type dump is for another executable (ShroudForge has not rewritten it for this build)";
        return false;
    }
    std::string json;
    if (!read_file(dump_path, json, 256u << 20)) {
        why = "type dump unreadable";
        return false;
    }
    return from_json(json, l, why);
}

}  // namespace typedump
