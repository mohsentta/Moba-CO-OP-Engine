-- Server-side game mode script.
--
-- The C++ host owns transport and the tick loop; the mode owns the rules:
-- minion waves, auto-attack damage with a cooldown, and spells with per-slot
-- cooldowns and area damage.

local ATTACK_COOLDOWN = 0.8
local ATTACK_RANGE = 600
local ATTACK_DAMAGE = 45

-- slot -> cooldown / damage / radius (Q W E R)
local SPELLS = {
  [0] = { cooldown = 7,   damage = 120, radius = 320 },
  [1] = { cooldown = 9,   damage = 100, radius = 320 },
  [2] = { cooldown = 14,  damage = 80,  radius = 320 },
  [3] = { cooldown = 120, damage = 300, radius = 420 },
}

local WAVE = {
  { x =   0, z = 700, hp = 120 },
  { x = 180, z = 780, hp = 120 },
  { x = -180, z = 780, hp = 120 },
}

local minions = {}
local wave_index = 0
local next_wave_at = 0

local function spawn_wave()
  wave_index = wave_index + 1
  minions = {}
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
  world.log(string.format("wave %d: %d minions", wave_index, #minions))
end

spawn_wave()

local last_attack = {}
local spell_ready_at = {}

local function damage_units_in_radius(x, z, radius, amount, source)
  local hit = 0
  for _, unit in ipairs(world.units()) do
    if unit.team ~= 0 and unit.hp > 0 then
      local dx, dz = unit.x - x, unit.z - z
      if dx * dx + dz * dz <= radius * radius then
        local remaining = world.damage(unit.id, amount)
        hit = hit + 1
        world.log(string.format("%s hit unit %d for %d (%.0f hp left)", source, unit.id,
                                amount, remaining))
        if remaining <= 0 then
          world.log(string.format("unit %d died", unit.id))
          world.remove(unit.id)
        end
      end
    end
  end
  return hit
end

world.on_intent(function(player_id, kind, slot, target, x, z)
  local now = world.time()

  if kind == "attack" then
    if last_attack[player_id] and now - last_attack[player_id] < ATTACK_COOLDOWN then
      return
    end
    local unit = world.unit(target)
    if not unit or unit.hp <= 0 then return end

    local attacker = world.player_unit(player_id)
    if attacker then
      local dx, dz = unit.x - attacker.x, unit.z - attacker.z
      if dx * dx + dz * dz > ATTACK_RANGE * ATTACK_RANGE then
        world.log(string.format("player %d attack rejected: out of range", player_id))
        return
      end
    end

    last_attack[player_id] = now
    local remaining = world.damage(target, ATTACK_DAMAGE)
    world.log(string.format("player %d hit unit %d for %d (%.0f hp left)", player_id, target,
                            ATTACK_DAMAGE, remaining))
    if remaining <= 0 then
      world.log(string.format("unit %d died", target))
      world.remove(target)
    end
    return
  end

  if kind == "cast" then
    local spell = SPELLS[slot]
    if not spell then return end
    local key = player_id .. ":" .. slot
    if spell_ready_at[key] and now < spell_ready_at[key] then
      world.log(string.format("player %d cast %d rejected: cooldown %.1fs", player_id, slot,
                              spell_ready_at[key] - now))
      return
    end

    spell_ready_at[key] = now + spell.cooldown
    world.log(string.format("player %d cast %d at (%.0f, %.0f)", player_id, slot, x, z))
    local hits = damage_units_in_radius(x, z, spell.radius, spell.damage,
                                        string.format("player %d slot %d", player_id, slot))
    if hits == 0 then
      world.log("  (no units in range)")
    end
  end
end)

-- The wave advances toward the player spawn (z = 0) until it arrives.
world.on_tick(function(dt)
  local alive = 0
  for _, id in ipairs(minions) do
    local unit = world.unit(id)
    if unit and unit.hp > 0 then
      alive = alive + 1
      if unit.z > 0 then
        world.move(id, unit.x, math.max(0, unit.z - 120 * dt))
      end
    end
  end

  -- Keep the range populated: a new wave shortly after the last one dies.
  if alive == 0 and world.time() >= next_wave_at then
    next_wave_at = world.time() + 4
    spawn_wave()
  end
end)
