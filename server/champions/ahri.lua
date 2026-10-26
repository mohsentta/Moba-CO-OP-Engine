-- Ahri: server-side spell book.
--
-- W spawns three orbs that orbit Ahri for four seconds. If an enemy comes close
-- while they orbit, they break away, fly to that enemy and damage it on arrival.

local ahri = { name = "Ahri" }

ahri.spells = {
  [0] = { anim = "spell1", cooldown = 7.0, damage = 60, radius = 250 },
  [1] = { anim = "spell2", cooldown = 9.0, damage = 40 },  -- orbs (see cast)
  [2] = { anim = "spell3", cooldown = 14.0, damage = 80, radius = 300 },
  [3] = { anim = "spell4", cooldown = 120.0, damage = 200, radius = 350 },
}

local ORBIT_TIME = 4.0
local ORBIT_RADIUS = 130.0
local ORBIT_SPEED = 2.2       -- radians per second
local LAUNCH_RANGE = 700.0    -- an enemy this close makes the orbs break away
local ORB_SPEED = 900.0
local ORB_DAMAGE = 40.0
local ORB_COUNT = 3
local HIT_RADIUS = 60.0

local flocks = {}

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

local function burst(ctx, unit, x, z, radius, damage, source)
  local hits = 0
  for _, other in ipairs(ctx.world.units()) do
    if other.team ~= 0 and other.hp > 0 then
      local dx, dz = other.x - x, other.z - z
      if dx * dx + dz * dz <= radius * radius then
        apply_damage(ctx.world, other, damage, source)
        hits = hits + 1
      end
    end
  end
  return hits
end

function ahri.cast(ctx, slot, x, z)
  local unit = ctx.unit
  if not unit then return false end
  local spell = ahri.spells[slot]
  if not spell then return false end

  ctx.world.play_anim(unit.id, spell.anim, 1.2)

  if slot ~= 1 then
    local hits = burst(ctx, unit, x, z, spell.radius or 250, spell.damage,
                       string.format("%s slot %d", ahri.name, slot))
    if hits == 0 then ctx.world.log(string.format("%s slot %d (nothing in range)", ahri.name, slot)) end
    return true
  end

  -- W: three orbs circling Ahri.
  local flock = { owner = unit.id, born = ctx.world.time(), orbs = {} }
  for i = 1, ORB_COUNT do
    local id = ctx.world.spawn({
      model = "orb",
      hp = 1,
      max_hp = 0,  -- no health bar
      x = unit.x,
      z = unit.z,
      team = 0,
    })
    flock.orbs[i] = { id = id, phase = (i - 1) * (2 * math.pi / ORB_COUNT), state = "orbit" }
  end
  flocks[#flocks + 1] = flock
  ctx.world.log(string.format("%s W: %d orbs orbiting", ahri.name, ORB_COUNT))
  return true
end

function ahri.on_tick(world, dt)
  local now = world.time()

  for index = #flocks, 1, -1 do
    local flock = flocks[index]
    local owner = world.unit(flock.owner)
    local finished = (now - flock.born) > ORBIT_TIME

    -- Closest enemy to Ahri decides whether the orbs break away.
    local nearest, nearest_sq = nil, math.huge
    if owner then
      for _, other in ipairs(world.units()) do
        if other.team ~= 0 and other.hp > 0 then
          local dx, dz = other.x - owner.x, other.z - owner.z
          local sq = dx * dx + dz * dz
          if sq < nearest_sq then
            nearest, nearest_sq = other, sq
          end
        end
      end
    end

    local all_done = true
    for _, orb in ipairs(flock.orbs) do
      if orb.state == "orbit" then
        if owner and nearest and nearest_sq <= LAUNCH_RANGE * LAUNCH_RANGE then
          orb.state = "fly"
          orb.target = nearest.id
          world.log(string.format("Ahri orb leaves for unit %d", nearest.id))
        elseif owner then
          orb.phase = orb.phase + dt * ORBIT_SPEED
          world.move(orb.id, owner.x + math.cos(orb.phase) * ORBIT_RADIUS,
                     owner.z + math.sin(orb.phase) * ORBIT_RADIUS)
        else
          orb.state = "done"
        end
      end

      if orb.state == "fly" then
        local me = world.unit(orb.id)
        local target = world.unit(orb.target)
        if not me or not target or target.hp <= 0 then
          orb.state = "done"
        else
          local dx, dz = target.x - me.x, target.z - me.z
          local distance = math.sqrt(dx * dx + dz * dz)
          if distance <= HIT_RADIUS then
            apply_damage(world, target, ORB_DAMAGE, "Ahri orb")
            orb.state = "done"
          else
            local step = ORB_SPEED * dt
            world.move(orb.id, me.x + dx / distance * step, me.z + dz / distance * step)
          end
        end
      end

      if orb.state ~= "done" then all_done = false end
    end

    if finished or all_done then
      for _, orb in ipairs(flock.orbs) do world.remove(orb.id) end
      flocks[index] = nil
    end
  end
end

return ahri
