-- Converted from test.script by scripts/script-to-lua.py
-- See config.mk.

local ROM = F"${vars.TEST_DATA}/roms/iix-iicx-se30-97221136.rom"

-- The profile: each card with a substitute ROM, then its generic variant
local function card(id)
    for _, c in ipairs(gs.list(catalog.profile("iicx").cards)) do
        if c.id == id then
            return c
        end
    end
    return nil
end
-- The card's own name
local function card_name(id)
    if id == "mdc_8_24" then
        return "Macintosh Display Card 8•24"
    elseif id == "display_card_24ac" then
        return "Macintosh Display Card 24AC"
    end
    return "Macintosh Display Card 8•24 GC"
end
for _, id in ipairs({"mdc_8_24", "display_card_24ac", "824gc"}) do
    local c = card(id)
    local g = card(F"${id}_generic")
    assert(c ~= nil, F"the card ${id} is not listed")
    assert(c.status == "ok", F"Apple's ROM of ${id} is offered here")
    assert(g ~= nil, F"no generic variant of ${id}")
    assert(g.variant_of == id, "the variant does not name its card")
    assert(g.status == "ok", "the generic variant needs no ROM")
    assert(g.rom == nil, "the generic variant asks for a ROM file")
    assert(c.label == F"${card_name(id)} (real video ROM)", "the real entry's label")
    assert(g.label == F"${card_name(id)} (generic video ROM)", "the generic entry's label")
end
-- A card without a substitute has no variant
assert(card("mdc_8_24_generic_generic") == nil, "a variant of a variant")

-- The plain card runs Apple's ROM when it is offered
assert(machine.boot{model = "iicx", rom = ROM, config = "{\"cards\":[{\"slot\":\"nubus_9\",\"card\":\"mdc_8_24\"}]}"}, "the plain card boots")
assert(machine.nubus.slot[9].card.substitute == false, "the plain card does not run Apple's ROM")

-- The generic variant runs the substitute although Apple's is offered
for _, id in ipairs({"mdc_8_24", "display_card_24ac", "824gc"}) do
    assert(machine.boot{model = "iicx", rom = ROM, config = F"{\"cards\":[{\"slot\":\"nubus_9\",\"card\":\"${id}_generic\"}]}"}, F"the generic ${id} boots")
    assert(machine.nubus.slot[9].card.substitute, F"the generic ${id} runs Apple's ROM")
    assert(machine.nubus.slot[9].card.declrom.path == F"builtin:${id}", F"the generic ${id} is not on the generated ROM")
end

-- With a monitor and startup mode from the substitute's own modes
assert(machine.boot{model = "iicx", rom = ROM, config = "{\"cards\":[{\"slot\":\"nubus_9\",\"card\":\"824gc_generic\"}],\"displays\":{\"nubus_9\":{\"monitor\":\"13in_rgb\",\"mode\":\"640x480x8\"}}}"}, "the generic 8•24 GC with a startup mode boots")
assert(machine.nubus.slot[9].card.substitute, "the generic 8•24 GC runs Apple's ROM")

-- The SE/30's built-in video is no slot card: its ROM is the video_rom option
assert(catalog.default_config("se30").options.video_rom == "real", "the SE/30 does not default to its real video ROM")
assert((gs.try(function() return catalog.default_config("iicx").options.video_rom end, "absent") or "absent") == "absent", "a IIcx has no built-in video ROM choice")
assert(machine.boot{model = "se30", rom = ROM}, "the SE/30 boots")
assert(machine.nubus.slot[14].card.substitute == false, "the SE/30 does not run Apple's video ROM")
assert(machine.boot{model = "se30", rom = ROM, config = "{\"options\":{\"video_rom\":\"generic\"}}"}, "the SE/30 on its substitute video ROM boots")
assert(machine.nubus.slot[14].card.substitute, "video_rom=generic does not run the generic ROM")
assert(machine.nubus.slot[14].card.declrom.path == "builtin:builtin_se30_video", "the SE/30 is not on the generated video ROM")

echo("PASS: display-card-generic")
quit()
