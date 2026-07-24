local MOD_NAME = "PalVerify"
local EXECUTABLE =
    ".\\ue4ss\\Mods\\PalVerify\\Scripts\\PalVerifyClient.exe"

local function log(level, message)
    print(string.format("[%s] [%s] %s\n", MOD_NAME, level, message))
end

local function start_client_agent()
    local command = "cmd.exe /d /c " .. EXECUTABLE
    ExecuteAsync(function()
        local invoked, result, reason, code =
            pcall(os.execute, command)
        if not invoked or not result then
            log(
                "ERROR",
                "PalVerifyClient.exe launch failed (invoked "
                    .. tostring(invoked)
                    .. ", result "
                    .. tostring(result)
                    .. ", reason "
                    .. tostring(reason)
                    .. ", code "
                    .. tostring(code)
                    .. ")."
            )
            return
        end
        log(
            "INFO",
            "PalVerifyClient.exe exited (reason "
                .. tostring(reason)
                .. ", code "
                .. tostring(code)
                .. ")."
        )
    end)
    log("INFO", "Client observation agent requested.")
end

log("INFO", "Loading PalVerify client v0.2.0.")
start_client_agent()
