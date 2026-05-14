/**
 * @file src/platform/linux/fs_picker.cpp
 * @brief Linux native file picker — zenity --file-selection subprocess.
 *
 * Spawns `zenity --file-selection` as a child process and reads the selected
 * path from its stdout.  zenity is the de-facto GTK file dialog binary on
 * mainstream Linux desktops; available in apt/dnf/pacman repos as `zenity`.
 *
 * Called from the tray callback thread.  On Linux Sunshine is launched per-
 * user (systemd --user service), so the spawning process inherits DISPLAY /
 * WAYLAND_DISPLAY / DBUS_SESSION_BUS_ADDRESS / XDG_RUNTIME_DIR correctly
 * from the user session — no extra wrangling needed (cf. the Windows path
 * where sunshinesvc.exe runs as LocalSystem and has to CreateProcessAsUserW
 * the dialog into the active console user's session manually).
 *
 * If zenity isn't installed (rare on desktop distros, common on headless
 * server installs), we return std::nullopt with a clear log line pointing
 * the user at apt/dnf/pacman; the tray "Send file to client" action then
 * surfaces as the standard "user cancelled" no-op.
 *
 * §N.7 (v1.4.41): real impl replaces the v1.4.40 stub (which returned
 * std::nullopt unconditionally just to let the Linux server build link).
 */
#include "src/fs_picker.h"

#include "src/logging.h"

#include <boost/process/v1.hpp>
#include <boost/process/v1/args.hpp>
#include <boost/process/v1/child.hpp>
#include <boost/process/v1/io.hpp>
#include <boost/process/v1/pipe.hpp>
#include <boost/process/v1/search_path.hpp>

#include <string>
#include <vector>

namespace bp = boost::process::v1;

namespace fs_picker {

  std::optional<std::filesystem::path> pick_open_file(const open_options &opts) {
    auto zenity = bp::search_path("zenity");
    if (zenity.empty()) {
      BOOST_LOG(warning) << "[VIPLE-XFER] Linux fs_picker: zenity not in PATH. "
                            "Install with: apt install zenity / dnf install zenity / "
                            "pacman -S zenity.  Returning nullopt (Send-file-to-"
                            "client tray action will no-op).";
      return std::nullopt;
    }

    // Argv vector — zenity invoked WITHOUT shell, so no quoting / escaping
    // needed for user-supplied title/default_dir strings.
    std::vector<std::string> args = {"--file-selection"};
    if (!opts.title.empty()) {
      args.push_back("--title=" + opts.title);
    }
    if (!opts.default_dir.empty()) {
      // Trailing slash tells zenity "this is a directory; start the browser
      // here" vs "pre-select this file path".
      std::string dir = opts.default_dir;
      if (dir.empty() || dir.back() != '/') dir += '/';
      args.push_back("--filename=" + dir);
    }

    bp::ipstream out_stream;
    bp::ipstream err_stream;
    bp::child child_proc;
    try {
      child_proc = bp::child(zenity.string(),
                             bp::args(args),
                             bp::std_out > out_stream,
                             bp::std_err > err_stream);
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "[VIPLE-XFER] Linux fs_picker: spawning zenity failed: " << e.what();
      return std::nullopt;
    }

    // zenity prints one line of output (selected path) then exits.  Reading
    // before wait() is fine since the pipe buffer holds the small string.
    std::string path_str;
    std::getline(out_stream, path_str);

    child_proc.wait();
    int exit_code = child_proc.exit_code();

    // zenity exit codes:
    //   0 = OK (selection made; stdout has the path)
    //   1 = user cancelled (no error, no log)
    //   5 = timeout (not used unless --timeout)
    //   other = backend error (display unavailable, etc.) → warn
    if (exit_code == 1) {
      BOOST_LOG(debug) << "[VIPLE-XFER] Linux fs_picker: user cancelled";
      return std::nullopt;
    }
    if (exit_code != 0) {
      std::string err_line;
      std::getline(err_stream, err_line);
      BOOST_LOG(warning) << "[VIPLE-XFER] Linux fs_picker: zenity exit=" << exit_code
                         << " stderr=" << err_line;
      return std::nullopt;
    }

    if (path_str.empty()) {
      BOOST_LOG(warning) << "[VIPLE-XFER] Linux fs_picker: zenity exit=0 but empty stdout";
      return std::nullopt;
    }
    // Defensive trim in case any version of zenity emits CRLF.
    if (path_str.back() == '\r') path_str.pop_back();

    BOOST_LOG(info) << "[VIPLE-XFER] Linux fs_picker: picked '" << path_str << "'";
    return std::filesystem::path(path_str);
  }

}  // namespace fs_picker
