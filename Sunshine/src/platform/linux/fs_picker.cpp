/**
 * @file src/platform/linux/fs_picker.cpp
 * @brief Linux stub for fs_picker (§N.7 deferred).
 *
 * Real implementation should spawn zenity --file-selection subprocess from
 * the tray callback thread (which lives in the active console user's
 * session under sunshinesvc's CreateProcessAsUserW spawn — DISPLAY /
 * DBUS_SESSION_BUS_ADDRESS env are inherited correctly there).
 *
 * Until that lands, this stub returns nullopt so:
 *   - Linux server build links cleanly (system_tray.cpp references
 *     fs_picker::pick_open_file unconditionally).
 *   - tray "Send file to client" callback no-ops gracefully on Linux
 *     (returns "user cancelled" branch).
 *
 * Server-side file_transfer endpoint itself works on Linux fine for
 * receive-from-client (Web UI Pull) and any other transfer that doesn't
 * go through native file picker.  TODO §N.7.
 */
#include "src/fs_picker.h"

#include "src/logging.h"

namespace fs_picker {

  std::optional<std::filesystem::path> pick_open_file(const open_options &opts) {
    BOOST_LOG(info) << "[VIPLE-XFER] Linux fs_picker stub invoked (title='"
                    << opts.title << "', default_dir='" << opts.default_dir
                    << "') — §N.7 zenity impl pending, returning nullopt";
    return std::nullopt;
  }

}  // namespace fs_picker
