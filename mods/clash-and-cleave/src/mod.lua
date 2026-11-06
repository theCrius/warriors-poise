-- Clash & Cleave 0.1.0 prototype (client side only for now).
--
--   * Swing cancel: vanilla locks block and dodge from the wind-up through the recovery. Here the
--     lock covers only the swing itself: from SWING_LEAD_MS before the first hit to RECOVERY_MS
--     after the last. Locks are switches (research/combat-rebalanced-analysis.md): CanBlock and
--     CanEvade are taken out of the attack's DisableAbilityEvents, then one disable and one enable
--     are inserted at the new times. Idea from Combat Rebalanced by TakeyaSaito (MIT), which
--     removes the locks entirely.
--   * Armour defence: multiplies every armour piece's armorSetup.distribution. The classes keep
--     their vanilla split (Warrior physical, Mage magical, Ranger even).
--
-- Runs after Forged & Found (optional dependency), so its armour variants are covered too.

local function log(msg) shroudforge.log.info("[clash-and-cleave] " .. msg) end

if not loader.features.patch then
    log("no patch access in this phase, nothing to do")
    return
end

local function feature(key) return shroudforge.settings.get(key, false) == true end

local SWING_CANCEL = feature("swingCancel")
local ARMOUR_DEFENCE = feature("armourDefence")
local ARMOUR_MULTIPLIER = tonumber(shroudforge.settings.get("armourDefenceMultiplier", 1.25)) or 1.25

log(string.format("features: swing cancel %s, armour defence %s (x%.2f)", SWING_CANCEL and "on" or "off",
    ARMOUR_DEFENCE and "on" or "off", ARMOUR_MULTIPLIER))

local MS = 1000000 -- sequence times are nanoseconds
local SWING_LEAD_MS = 250
local RECOVERY_MS = 150

local LOCK_FLAGS = { CanBlock = true, CanEvade = true }
local DISABLE_EVENT = "keen::actor::DisableAbilityEvent"
local ENABLE_EVENT = "keen::actor::EnableAbilityEvent"
local HIT_EVENT = "keen::actor::SpawnImpact"

-- Same selection idea as Combat Rebalanced: melee only. Special abilities keep their own timing.
local SKIP_ATTACKS = { "Staff", "Wand", "Ranged", "Bow", "SpecialAbility", "Deprecated", "_Old", "_Test" }

local function wanted_attack(name)
    if not name:find("^Attack_Player_") then return false end
    for _, word in ipairs(SKIP_ATTACKS) do
        if name:find(word, 1, true) then return false end
    end
    return true
end

local function has_lock_flag(value)
    for _, flag in ipairs(value.mask) do
        if LOCK_FLAGS[flag] then return true end
    end
    return false
end

-- Reads one sub-sequence and works out the new lock; nil when there is nothing to change.
local function plan_attack(events)
    local plan = { disables = {} }
    local first_off, last_off
    local on_times, hit_from, hit_to = {}, nil, nil
    for _, event in ipairs(events) do
        local kind = event.type
        if kind == DISABLE_EVENT then
            plan.disable_source = plan.disable_source or event
            local value = event.value
            if has_lock_flag(value) then
                local t = value.time.value
                plan.disables[#plan.disables + 1] = value
                first_off = math.min(first_off or t, t)
                last_off = math.max(last_off or t, t)
            end
        elseif kind == ENABLE_EVENT then
            plan.enable_source = plan.enable_source or event
            local value = event.value
            if has_lock_flag(value) then on_times[#on_times + 1] = value.time.value end
        elseif kind == HIT_EVENT then
            local value = event.value
            local from = value.time.value
            local to = from + value.duration.value
            hit_from = math.min(hit_from or from, from)
            hit_to = math.max(hit_to or to, to)
        end
    end
    if #plan.disables == 0 or not hit_from or not plan.enable_source then return nil end

    local vanilla_end
    for _, t in ipairs(on_times) do
        if t > last_off then vanilla_end = math.min(vanilla_end or t, t) end
    end
    if not vanilla_end then return nil end

    -- Never lock earlier than vanilla did, never free later than vanilla did.
    local from = math.max(hit_from - SWING_LEAD_MS * MS, last_off)
    local to = math.max(hit_to + RECOVERY_MS * MS, from + RECOVERY_MS * MS)
    plan.from = math.floor(from)
    plan.to = math.floor(math.min(to, vanilla_end))
    plan.first_off, plan.vanilla_end = first_off, vanilla_end
    if plan.to <= plan.from then return nil end
    return plan
end

-- Position that keeps the array sorted by time, after any event at the same time.
local function sorted_position(events, time)
    local position = 1
    for i, event in ipairs(events) do
        if event.value.time.value <= time then position = i + 1 end
    end
    return position
end

local function insert_lock_event(events, source, time)
    local position = sorted_position(events, time)
    table.insert(events, position, source)
    local value = events[position].value
    value.mask = { "CanBlock", "CanEvade" }
    value.time.value = time
end

local function times_sorted(events)
    local last = -1
    for _, event in ipairs(events) do
        local t = event.value.time.value
        if t < last then return false end
        last = t
    end
    return true
end

local function patch_swing_cancel()
    local changed, skipped, freed = 0, 0, 0
    local example
    for _, resource in ipairs(game.assets.get_resources_by_type("keen::actor::ActorSequenceResource")) do
        -- The text form names the sub-sequences, so unrelated resources stay untouched.
        if tostring(resource.data):find("Attack_Player_", 1, true) then
            for _, sub in ipairs(resource.data.subSequences) do
                if wanted_attack(sub.name) then
                    local events = sub.events
                    local plan = plan_attack(events)
                    if plan then
                        local source_time = plan.disable_source.value.time.value
                        for _, value in ipairs(plan.disables) do
                            local kept = {}
                            for _, flag in ipairs(value.mask) do
                                if not LOCK_FLAGS[flag] then kept[#kept + 1] = flag end
                            end
                            value.mask = kept
                        end
                        insert_lock_event(events, plan.disable_source, plan.from)
                        insert_lock_event(events, plan.enable_source, plan.to)
                        if not times_sorted(events) then
                            error("clash-and-cleave: " .. sub.name .. " events are out of order")
                        end
                        if plan.disable_source.value.time.value ~= source_time then
                            error("clash-and-cleave: inserting into " .. sub.name .. " changed the original event")
                        end
                        changed = changed + 1
                        freed = freed + (plan.vanilla_end - plan.first_off) - (plan.to - plan.from)
                        if sub.name:find("Heavy_Sword", 1, true) then
                            example = string.format("%s: locked %d..%d ms, was %d..%d", sub.name, plan.from // MS, plan.to // MS,
                                plan.first_off // MS, plan.vanilla_end // MS)
                        end
                    else
                        skipped = skipped + 1
                    end
                end
            end
        end
    end
    log(string.format("swing cancel: %d attacks changed, %d without a lock to move, %d ms of lock removed in total",
        changed, skipped, freed // MS))
    if example then log("  example " .. example) end
    if changed == 0 then error("clash-and-cleave: swing cancel found no attacks, the game data changed") end
end

local DEFENCE_FIELDS = { "physical", "blunt", "pierce", "cut", "magical", "fire", "ice", "fog", "lightning" }

local function patch_armour_defence(multiplier)
    local changed, skipped = 0, 0
    local example
    for _, registry in ipairs(game.assets.get_resources_by_type("keen::ItemRegistryResource")) do
        local data = registry.data
        for i, name in ipairs(data.dbgNames) do
            if name:find("Armor_", 1, true) then
                local item = game.assets.get_resource(data.itemRefs[i], "keen::ItemInfo").data
                local setup = item.armorSetup
                if setup.isSet then
                    local distribution = setup.distribution
                    if not example then
                        example = string.format("%s: physical %.3f, magical %.3f", name, distribution.physical, distribution.magical)
                    end
                    for _, field in ipairs(DEFENCE_FIELDS) do
                        local value = distribution[field]
                        if value ~= 0 then distribution[field] = value * multiplier end
                    end
                    changed = changed + 1
                else
                    skipped = skipped + 1
                end
            end
        end
    end
    log(string.format("armour defence: %d pieces x%.2f, %d without armour values", changed, multiplier, skipped))
    if example then log("  example before " .. example) end
    if changed == 0 then error("clash-and-cleave: armour defence found no armour, the game data changed") end
end

if SWING_CANCEL then patch_swing_cancel() end
if ARMOUR_DEFENCE then patch_armour_defence(math.max(0, ARMOUR_MULTIPLIER)) end
log("done")
