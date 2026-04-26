package com.limelight.nvstream.http;

/**
 * VipleStream H.4: a single Steam profile entry returned by
 * /steamprofiles. Mirrors the server-side Sunshine `viple::steam::Profile`
 * fields the client cares about. `current=true` means this is the active
 * Steam user on the host right now.
 *
 * Plain data class — populated by NvHTTP.getSteamProfileListByReader().
 */
public class SteamProfile {
    public String steamId3 = "";        // 32-bit account ID (dropdown key)
    public String steamId64 = "";
    public String accountName = "";     // Steam username — passed to /steamswitch?account=
    public String personaName = "";     // Display name shown in the dropdown
    public boolean rememberPassword;    // false ⇒ switch will fail with 409
    public boolean mostRecent;
    public long    lastLogin;
    public boolean current;             // matches /steamprofiles `current_user`

    /**
     * What the dropdown should show. Falls back to accountName when the
     * persona is empty (rare but seen with brand-new Steam profiles that
     * haven't fetched persona data from Steam servers yet).
     */
    public String displayName() {
        if (personaName != null && !personaName.isEmpty()) {
            return personaName;
        }
        return accountName;
    }
}
