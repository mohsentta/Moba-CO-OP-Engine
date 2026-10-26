-- Jinx: server-side spell book.
--
-- The host calls `cast(ctx, slot, x, z)` when this player casts. The module owns
-- the animation the clients should play, the cooldown, the shape of the hit and
-- any delay before it lands.

local jinx = { name = "Jinx" }

-- slot 0..3 = Q W E R
jinx.spells = {
  [0] = { anim = "spell1", cooldown = 7.0, damage = 100, radius = 250 },
  -- W: 0.5 s charge, then a long thin line from the caster towards the aim.
  [1] = { anim = "spell2", cooldown = 9.0, delay = 0.5, damage = 120, length = 2000,
          width = 120 },
  [2] = { anim = "spell3", cooldown = 14.0, damage = 80, radius = 300 },
  [3] = { anim = "spell4", cooldown = 120.0, damage = 300, radius = 420 },
}

local function apply_damage(world, unit, amount, source)
  local remaining = world.damage(unit.id, amount)
  world.log(string.format("%s hit unit %d for %d (%.0f hp left)", source, unit.id, amount,
                          remaining))
  if remaining <= 0 then
    world.log(string.format("unit %d died", unit.id))
    world.remove(unit.id)
  end
  return remaining
end

-- Everyone within `radius` of the aim point.
local function radius_damage(world, x, z, radius, damage, source)
  local hits = 0
  for _, unit in ipairs(world.units()) do
    if unit.team ~= 0 and unit.hp > 0 then
      local dx, dz = unit.x - x, unit.z - z
      if dx * dx + dz * dz <= radius * radius then
        apply_damage(world, unit, damage, source)
        hits = hits + 1
      end
    end
  end
  return hits
end

-- Everyone inside a rectangle from the caster along (dir_x, dir_z).
local function line_damage(world, caster, dir_x, dir_z, length, width, damage, source)
  local hits = 0
  for _, unit in ipairs(world.units()) do
    if unit.team ~= 0 and unit.hp > 0 then
      local dx, dz = unit.x - caster.x, unit.z - caster.z
      local along = dx * dir_x + dz * dir_z
      local offset = math.abs(dx * dir_z - dz * dir_x)
      if along >= 0 and along <= length and offset <= width * 0.5 then
        apply_damage(world, unit, damage, source)
        hits = hits + 1
      end
    end
  end
  return hits
end

-- ctx = { world = world, unit = caster table, schedule = function(delay, fn) }
function jinx.cast(ctx, slot, x, z)
  local spell = jinx.spells[slot]
  local unit = ctx.unit
  if not spell or not unit then return false end

  -- Tell every client which animation to play for this action.
  local anim_seconds = (spell.delay or 0) + 1.0
  ctx.world.play_anim(unit.id, spell.anim, anim_seconds)

  local dx, dz = x - unit.x, z - unit.z
  local distance = math.sqrt(dx * dx + dz * dz)
  if distance < 0.001 then return true end
  local dir_x, dir_z = dx / distance, dz / distance
  local source = string.format("%s slot %d", jinx.name, slot)

  if spell.length then
    local fire = function()
      local caster = ctx.world.unit(unit.id)
      if not caster then return end
      local hits = line_damage(ctx.world, caster, dir_x, dir_z, spell.length, spell.width,
                               spell.damage, source)
      ctx.world.log(string.format("%s fired%s", source,
                                  hits == 0 and " (nothing in the line)" or ""))
    end
    if (spell.delay or 0) > 0 then
      ctx.schedule(spell.delay, fire)
    else
      fire()
    end
    return true
  end

  local hit_x, hit_z = unit.x + dir_x * distance, unit.z + dir_z * distance
  local hits = radius_damage(ctx.world, hit_x, hit_z, spell.radius, spell.damage, source)
  if hits == 0 then ctx.world.log(source .. " (nothing in range)") end
  return true
end

return jinx
