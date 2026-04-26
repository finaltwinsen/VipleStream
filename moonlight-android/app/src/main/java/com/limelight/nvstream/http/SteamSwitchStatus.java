package com.limelight.nvstream.http;

/**
 * VipleStream H.4 (v1.2.119): one snapshot of an in-flight or
 * recently-finished steam-switch task.  Returned by
 * NvHTTP.startSteamSwitch() (initial response) and
 * NvHTTP.pollSteamSwitchStatus() (each subsequent poll).
 *
 * Field layout matches the server's serialize_steam_switch_task() XML
 * schema in Sunshine/src/nvhttp.cpp.
 */
public class SteamSwitchStatus {
    public String taskId = "";
    public String state = "";              // starting | shutting_down | logging_in | done | error | already_active
    public String message = "";            // human-readable progress text
    public String error = "";              // populated when state == error
    public String accountName = "";
    public String personaName = "";
    public String currentUserAfter = "";   // populated when state == done
    public int    httpStatus;              // final logical status (200 / 4xx / 5xx)
    public long   elapsedMs = -1;
    public long   finishedMs = -1;         // -1 if still running

    public boolean isTerminal() {
        return "done".equals(state)
            || "error".equals(state)
            || "already_active".equals(state);
    }

    public boolean isSuccess() {
        return "done".equals(state) || "already_active".equals(state);
    }
}
