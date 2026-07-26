-- Example script. Drop any .lua in this folder, restart the server, done.
-- Available: cybermp.log, cybermp.warn, cybermp.on, cybermp.players

cybermp.log("welcome.lua running")

cybermp.on("playerJoin", function(id)
    local player = cybermp.players.find(id)
    if not player then
        return
    end

    cybermp.log(player.name .. " joined from " .. player.address
        .. " (" .. cybermp.players.count() .. " online)")

    -- Uncomment to see kicks working:
    -- if player.name == "banned" then player.kick("not welcome") end
end)

cybermp.on("playerLeave", function(id)
    cybermp.log("#" .. id .. " left, " .. cybermp.players.count() .. " online")
end)
