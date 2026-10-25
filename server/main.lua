-- Server-side game mode script.
--
-- The C++ host owns transport and the tick loop; the mode owns the rules. A
-- wave of minions walks toward the player spawn and players can auto-attack
-- them: damage, cooldown and death all live here.

local ATTACK_COOLDOWN = 0.8
local ATTACK_DAMAGE = 45

local WAVE = {
  { x =   0, z = 700, hp = 120 },
  { x = 180, z = 780, hp = 120 },
  { x = -180, z = 780, hp = 120 },
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

local last_attack = {}

world.on_intent(function(player_id, kind, slot, target, x, z)
  if kind ~= "attack" then return end

  local now = world.time()
  if last_attack[player_id] and now - last_attack[player_id] < ATTACK_COOLDOWN then
    return
  end

  local unit = world.unit(target)
  if not unit or unit.hp <= 0 then return end

  last_attack[player_id] = now
  local remaining = world.damage(target, ATTACK_DAMAGE)
  world.log(string.format("player %d hit unit %d for %d (%.0f hp left)", player_id, target,
                          ATTACK_DAMAGE, remaining))

  if remaining <= 0 then
    world.log(string.format("unit %d died", target))
    world.remove(target)
  end
end)

-- The wave advances toward the player spawn (z = 0) until it arrives.
world.on_tick(function(dt)
  for _, id in ipairs(minions) do
    local unit = world.unit(id)
    if unit and unit.hp > 0 and unit.z > 0 then
      world.move(id, unit.x, math.max(0, unit.z - 120 * dt))
    end
  end
end)
