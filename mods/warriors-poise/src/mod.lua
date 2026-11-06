-- Warriors' Poise 0.5.0 beta (client and dedicated server, install it on both).
--
--   * Swing cancel: vanilla locks block and dodge from the wind-up through the recovery. Here the
--     lock covers only the swing itself: from SWING_LEAD_MS before the first hit to RECOVERY_MS
--     after the last. Locks are switches (research/combat-rebalanced-analysis.md): CanBlock and
--     CanEvade are taken out of the attack's DisableAbilityEvents, then one disable and one enable
--     are inserted at the new times. Idea from Combat Rebalanced by TakeyaSaito (MIT), which
--     removes the locks entirely.
--   * Armour defence: multiplies every armour piece's armorSetup.distribution. The classes keep
--     their vanilla split (Warrior physical, Mage magical, Ranger even).
--   * Poise: gives every armour piece and shield poise points and exports them with the rules
--     for the native DLL, which spends and refills the pool. The game has no armour class, only
--     item names: Warrior is heavy, Ranger medium, Mage light. See poise_class.
--
-- Depends on no other mod. ShroudForge loads mods in id order, so with Forged & Found installed this
-- runs after it and covers its armour variants too.
--
-- A feature that finds nothing to change only warns: error() fails the whole asset pass, the game
-- then starts on vanilla data, and vanilla data deletes Forged & Found's variant items. error() is
-- kept for a change that went wrong halfway, where the data would otherwise be left inconsistent.

local function log(msg) shroudforge.log.info("[warriors-poise] " .. msg) end
local function warn_feature(msg) shroudforge.log.warn("[warriors-poise] " .. msg) end

if not loader.features.patch then
    log("no patch access in this phase, nothing to do")
    return
end

local function feature(key) return shroudforge.settings.get(key, false) == true end

local SWING_CANCEL = feature("swingCancel")
local ARMOUR_DEFENCE = feature("armourDefence")
local ARMOUR_MULTIPLIER = tonumber(shroudforge.settings.get("armourDefenceMultiplier", 1.25)) or 1.25
local POISE = feature("poise")

local function number_setting(key, fallback) return tonumber(shroudforge.settings.get(key, fallback)) or fallback end

log(string.format("%s, features: swing cancel %s, armour defence %s (x%.2f), poise %s",
    loader.is_server and "dedicated server" or "client", SWING_CANCEL and "on" or "off",
    ARMOUR_DEFENCE and "on" or "off", ARMOUR_MULTIPLIER, POISE and "on" or "off"))

local MS = 1000000 -- sequence times are nanoseconds
local SWING_LEAD_MS = 150
local RECOVERY_MS = 100

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
                            error("warriors-poise: " .. sub.name .. " events are out of order")
                        end
                        if plan.disable_source.value.time.value ~= source_time then
                            error("warriors-poise: inserting into " .. sub.name .. " changed the original event")
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
    if changed == 0 then warn_feature("swing cancel found no attacks here: nothing changed") end
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
    if changed == 0 then warn_feature("armour defence found no armour here: nothing changed") end
end

-- Light, medium or heavy for an armour item's debug name.
local function poise_class(name)
    local class = name:match("Armor_T[%d%-TX]*_(%a+)_")
    if class == "Warrior" then return "heavy" end
    if class == "Mage" or class == "Fur" or class == "Rags" or class == "Starter" then return "light" end
    return "medium" -- Ranger, and the mixed sets (MultiClass, Unique) and anything else
end

local function export_poise()
    if not loader.features.export then
        log("poise: export not available, the DLL stays off")
        return
    end
    local points = { heavy = number_setting("poiseHeavyArmour", 10), medium = number_setting("poiseMediumArmour", 6),
        light = number_setting("poiseLightArmour", 3), shield = number_setting("poiseShield", 30) }
    local counts = { heavy = 0, medium = 0, light = 0, shield = 0, two_handed = 0 }
    points.two_handed = -1 -- marks the weapon for the DLL: a shield gives no poise with it
    local lines = { string.format("poise %d light=%d medium=%d heavy=%d refill=%d", POISE and 1 or 0,
        number_setting("poiseLightHit", 5), number_setting("poiseMediumHit", 10), number_setting("poiseHeavyHit", 50),
        math.floor(number_setting("poiseRefillSeconds", 3) * 1000)) }
    if POISE then
        for _, registry in ipairs(game.assets.get_resources_by_type("keen::ItemRegistryResource")) do
            local data = registry.data
            for i, name in ipairs(data.dbgNames) do
                local kind
                if name:find("^Armor_") or name:find("^Craft_Armor_") then kind = poise_class(name)
                elseif name:find("^Shield_") or name:find("^Craft_Shield_") then kind = "shield"
                elseif (name:find("^Weapon_") or name:find("^Craft_Weapon_")) and name:find("_2H_", 1, true) then
                    kind = "two_handed"
                end
                if kind then
                    -- From the text form: nothing here edits the item.
                    local text = tostring(game.assets.get_resource(data.itemRefs[i], "keen::ItemInfo").data)
                    local id = text:match('^{"itemId":{"value":(%d+)}')
                    if id then
                        lines[#lines + 1] = id .. " " .. math.floor(points[kind])
                        counts[kind] = counts[kind] + 1
                    else
                        warn_feature("poise: no item id for " .. name)
                    end
                end
            end
        end
        if counts.heavy == 0 or counts.shield == 0 then warn_feature("poise found no armour or no shields here") end
    end
    io.export("warriors-poise/poise.txt", table.concat(lines, "\n") .. "\n")
    log(string.format("poise: %d heavy, %d medium, %d light pieces, %d shields, %d two-handed weapons exported", counts.heavy,
        counts.medium, counts.light, counts.shield, counts.two_handed))
end

if SWING_CANCEL then patch_swing_cancel() end
if ARMOUR_DEFENCE then patch_armour_defence(math.max(0, ARMOUR_MULTIPLIER)) end
export_poise()
log("done")
