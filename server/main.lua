-- Server-side game mode.
--
-- The host owns transport and the tick loop. This script owns the rules and
-- delegates champion specifics to server/champions/<champion>.lua.

local ATTACK_COOLDOWN = 0.8
local ATTACK_DAMAGE = 45
local ATTACK_RANGE = 600

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
    local id = world.spawn({ model = "minion", hp = spec.hp, x = spec.x, z = spec.z, team = 1 })
    minions[#minions + 1] = id
  end
  world.log(string.format("wave %d: %d minions", wave_index, #minions))
end

spawn_wave()

-- ---- scheduled effects ---------------------------------------------------
local pending = {}

local function schedule(delay, fn)
  pending[#pending + 1] = { at = world.time() + delay, fn = fn }
end

local function run_pending()
  local now = world.time()
  local index = 1
  while index <= #pending do
    if pending[index].at <= now then
      local event = table.remove(pending, index)
      local ok, err = pcall(event.fn)
      if not ok then world.log("scheduled action failed: " .. tostring(err)) end
    else
      index = index + 1
    end
  end
end

-- ---- champion modules ----------------------------------------------------
local champions = {}

local function load_champion(name)
  local key = string.lower(name)
  if champions[key] ~= nil then return champions[key] or nil end
  local path = "server/champions/" .. key .. ".lua"
  local chunk = loadfile(path)
  if not chunk then
    champions[key] = false
    world.log("no champion module for " .. name)
    return nil
  end
  local module = chunk()
  champions[key] = module or false
  world.log(string.format("loaded champion module %s (%s)", name, path))
  return module
end

-- ---- player intentions ---------------------------------------------------
local last_attack = {}
local spell_ready_at = {}

local function apply_damage(unit_id, amount, source)
  local remaining = world.damage(unit_id, amount)
  world.log(string.format("%s hit unit %d for %d (%.0f hp left)", source, unit_id, amount,
                          remaining))
  if remaining <= 0 then
    world.log(string.format("unit %d died", unit_id))
    world.remove(unit_id)
  end
  return remaining
end

world.on_intent(function(player_id, kind, slot, target, x, z)
  local now = world.time()

  if kind == "attack" then
    if last_attack[player_id] and now - last_attack[player_id] < ATTACK_COOLDOWN then return end
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
    apply_damage(target, ATTACK_DAMAGE, string.format("player %d", player_id))
    return
  end

  if kind ~= "cast" then return end

  local caster = world.player_unit(player_id)
  if not caster then return end
  local module = load_champion(caster.model or "")

  local cooldown = 8.0
  if module and module.spells and module.spells[slot] and module.spells[slot].cooldown then
    cooldown = module.spells[slot].cooldown
  end

  local key = player_id .. ":" .. slot
  if spell_ready_at[key] and now < spell_ready_at[key] then
    world.log(string.format("player %d cast %d rejected: cooldown %.1fs", player_id, slot,
                            spell_ready_at[key] - now))
    return
  end
  spell_ready_at[key] = now + cooldown

  if module then
    local ctx = { world = world, unit = caster, schedule = schedule }
    local ok, err = pcall(module.cast, ctx, slot, x, z)
    if not ok then
      world.log(string.format("cast failed in %s: %s", module.name, tostring(err)))
    end
    return
  end

  -- Fallback for champions without a module: circular burst at the aim point.
  world.play_anim(caster.id, "spell" .. (slot + 1), 1.1)
  local hits = 0
  for _, unit in ipairs(world.units()) do
    if unit.team ~= 0 and unit.hp > 0 then
      local dx, dz = unit.x - x, unit.z - z
      if dx * dx + dz * dz <= 320 * 320 then
        apply_damage(unit.id, 100, string.format("player %d slot %d", player_id, slot))
        hits = hits + 1
      end
    end
  end
  if hits == 0 then
    world.log(string.format("player %d slot %d (nothing in range)", player_id, slot))
  end
end)

-- ---- world tick ----------------------------------------------------------
world.on_tick(function(dt)
  local alive = 0
  for _, id in ipairs(minions) do
    local unit = world.unit(id)
    if unit and unit.hp > 0 then
      alive = alive + 1
      if unit.z > 0 then world.move(id, unit.x, math.max(0, unit.z - 120 * dt)) end
    end
  end

  if alive == 0 and world.time() >= next_wave_at then
    next_wave_at = world.time() + 4
    spawn_wave()
  end

  run_pending()

  -- Let champion modules advance their own state (orbits, projectiles, ...).
  for _, module in pairs(champions) do
    if module and module.on_tick then
      local ok, err = pcall(module.on_tick, world, dt)
      if not ok then world.log("on_tick failed: " .. tostring(err)) end
    end
  end
end)
