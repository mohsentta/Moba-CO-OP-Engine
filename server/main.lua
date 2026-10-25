-- Server-side game mode script.
--
-- The C++ host owns the transport and the tick loop; everything game-specific
-- lives here. This first version spawns a minion wave and lets it walk and take
-- damage, so the host's world state can be inspected from the logs.

local WAVE = {
  { x =   0, z = 700, hp = 480 },
  { x = 180, z = 780, hp = 480 },
  { x = -180, z = 780, hp = 480 },
}

local minions = {}
for _, spec in ipairs(WAVE) do
  local id = world.spawn({
    model = "minion",
    hp = spec.hp,
    x = spec.x,
    z = spec.z,
    team = 1,
  })
  minions[#minions + 1] = id
end

world.log(string.format("spawned %d minions", #minions))

-- Every tick the wave advances toward the player spawn (z = 0) and the first
-- minion loses health, exercising the HP path the client will read later.
local elapsed = 0
world.on_tick(function(dt)
  elapsed = elapsed + dt
  for _, id in ipairs(minions) do
    local unit = world.unit(id)
    if unit and unit.hp > 0 then
      world.move(id, unit.x, math.max(0, unit.z - 120 * dt))
    end
  end

  local first = minions[1]
  if first then
    local unit = world.unit(first)
    if unit then
      world.set_hp(first, math.max(0, unit.hp - 40 * dt))
    end
  end
end)
