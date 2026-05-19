/**
 * @file src/confighttp.cpp
 * @brief Definitions for the Web UI Config HTTP server.
 *
 * @todo Authentication, better handling of routes common to nvhttp, cleanup
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <string_view>

// lib includes
#include <boost/algorithm/string.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <Simple-Web-Server/crypto.hpp>
#include <Simple-Web-Server/server_https.hpp>

#ifdef _WIN32
  #include "platform/windows/misc.h"

  #include <vector>
  #include <Windows.h>
#endif

// local includes
#include "config.h"
#include "confighttp.h"
#include "crypto.h"
#include "display_device.h"
#include "file_handler.h"
#include "file_transfer.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "process.h"
#include "rtsp.h"
#include "utility.h"
#include "uuid.h"

using namespace std::literals;

namespace confighttp {
  namespace fs = std::filesystem;

  using https_server_t = SimpleWeb::Server<SimpleWeb::HTTPS>;

  using args_t = SimpleWeb::CaseInsensitiveMultimap;
  using resp_https_t = std::shared_ptr<SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;
  using https_handler_t = std::function<void(resp_https_t, req_https_t)>;

  enum class op_e {
    ADD,  ///< Add client
    REMOVE  ///< Remove client
  };

  // CSRF token management
  struct csrf_token_t {
    std::string token;
    std::chrono::steady_clock::time_point expiration;
  };

  // Store CSRF tokens with thread safety
  std::map<std::string, csrf_token_t, std::less<>> csrf_tokens;  // NOSONAR(cpp:S5421) - intentionally mutable global
  std::mutex csrf_tokens_mutex;  // NOSONAR(cpp:S5421) - intentionally mutable global

  // CSRF token configuration
  constexpr auto CSRF_TOKEN_SIZE = 32;  // 32 bytes = 256 bits
  constexpr auto CSRF_TOKEN_LIFETIME = std::chrono::hours(1);  // Tokens valid for 1 hour

  /**
   * @brief Log the request details.
   * @param request The HTTP request object.
   */
  void print_req(const req_https_t &request) {
    BOOST_LOG(debug) << "METHOD :: "sv << request->method;
    BOOST_LOG(debug) << "DESTINATION :: "sv << request->path;

    for (auto &[name, val] : request->header) {
      BOOST_LOG(debug) << name << " -- " << (name == "Authorization" ? "CREDENTIALS REDACTED" : val);
    }

    BOOST_LOG(debug) << " [--] "sv;

    for (auto &[name, val] : request->parse_query_string()) {
      BOOST_LOG(debug) << name << " -- " << val;
    }

    BOOST_LOG(debug) << " [--] "sv;
  }

  /**
   * @brief Send a response.
   * @param response The HTTP response object.
   * @param output_tree The JSON tree to send.
   */
  void send_response(const resp_https_t &response, const nlohmann::json &output_tree) {
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(output_tree.dump(), headers);
  }

  /**
   * @brief Send a 401 Unauthorized response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void send_unauthorized(const resp_https_t &response, const req_https_t &request) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    BOOST_LOG(info) << "Web UI: ["sv << address << "] -- not authorized"sv;

    constexpr auto code = SimpleWeb::StatusCode::client_error_unauthorized;

    nlohmann::json tree;
    tree["status_code"] = code;
    tree["status"] = false;
    tree["error"] = "Unauthorized";

    // VipleStream §A.6 — HTTP Basic auth realm rebrand (TODO.md §A.6).
    // Old realm string was "Sunshine Gamestream Host" (shared coincidentally
    // with the TLS cert CN in httpcommon.cpp gen_creds(); the cert CN must
    // stay for wire-format compatibility per §A.7, but this realm is purely
    // a browser-side credential bucket label and is safe to rename).
    // After upgrade, browsers will prompt for the Web UI password again
    // because saved credentials are filed under the old realm.
    const SimpleWeb::CaseInsensitiveMultimap headers {
      {"Content-Type", "application/json"},
      {"WWW-Authenticate", R"(Basic realm="VipleStream-Server Web UI", charset="UTF-8")"},
      {"X-Frame-Options", "DENY"},
      {"Content-Security-Policy", "frame-ancestors 'none';"}
    };

    response->write(code, tree.dump(), headers);
  }

  /**
   * @brief Send a redirect response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param path The path to redirect to.
   */
  void send_redirect(const resp_https_t &response, const req_https_t &request, const char *path) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    BOOST_LOG(info) << "Web UI: ["sv << address << "] -- not authorized"sv;
    const SimpleWeb::CaseInsensitiveMultimap headers {
      {"Location", path},
      {"X-Frame-Options", "DENY"},
      {"Content-Security-Policy", "frame-ancestors 'none';"}
    };
    response->write(SimpleWeb::StatusCode::redirection_temporary_redirect, headers);
  }

  /**
   * @brief Authenticate the user.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @return True if the user is authenticated, false otherwise.
   */
  bool authenticate(const resp_https_t &response, const req_https_t &request) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());

    if (const auto ip_type = net::from_address(address); ip_type > http::origin_web_ui_allowed) {
      BOOST_LOG(info) << "Web UI: ["sv << address << "] -- denied"sv;
      response->write(SimpleWeb::StatusCode::client_error_forbidden);
      return false;
    }

    // If credentials are shown, redirect the user to a /welcome page
    if (config::sunshine.username.empty()) {
      send_redirect(response, request, "/welcome");
      return false;
    }

    auto fg = util::fail_guard([&]() {
      send_unauthorized(response, request);
    });

    const auto auth = request->header.find("authorization");
    if (auth == request->header.end()) {
      return false;
    }

    const auto &rawAuth = auth->second;
    auto authData = SimpleWeb::Crypto::Base64::decode(rawAuth.substr("Basic "sv.length()));

    const auto index = static_cast<int>(authData.find(':'));
    if (index >= authData.size() - 1) {
      return false;
    }

    const auto username = authData.substr(0, index);
    const auto password = authData.substr(index + 1);

    if (const auto hash = util::hex(crypto::hash(password + config::sunshine.salt)).to_string(); !boost::iequals(username, config::sunshine.username) || hash != config::sunshine.password) {
      return false;
    }

    fg.disable();
    return true;
  }

  /**
   * @brief Send a 404 Not Found response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param error_message The error message to include in the response.
   */
  void not_found(const resp_https_t &response, [[maybe_unused]] const req_https_t &request, const std::string &error_message) {
    constexpr auto code = SimpleWeb::StatusCode::client_error_not_found;

    nlohmann::json tree;
    tree["status_code"] = code;
    tree["error"] = error_message;

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");

    response->write(code, tree.dump(), headers);
  }

  /**
   * @brief Send a 400 Bad Request response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param error_message The error message to include in the response.
   */
  void bad_request(const resp_https_t &response, [[maybe_unused]] const req_https_t &request, const std::string &error_message) {
    constexpr auto code = SimpleWeb::StatusCode::client_error_bad_request;

    nlohmann::json tree;
    tree["status_code"] = code;
    tree["status"] = false;
    tree["error"] = error_message;

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");

    response->write(code, tree.dump(), headers);
  }

  /**
   * @brief Validate the request content type and send a bad request when mismatched.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param contentType The expected content type
   */
  bool check_content_type(const resp_https_t &response, const req_https_t &request, const std::string_view &contentType) {
    const auto requestContentType = request->header.find("content-type");
    if (requestContentType == request->header.end()) {
      bad_request(response, request, "Content type not provided");
      return false;
    }
    // Extract the media type part before any parameters (e.g., charset)
    std::string actualContentType = requestContentType->second;
    if (const size_t semicolonPos = actualContentType.find(';'); semicolonPos != std::string::npos) {
      actualContentType = actualContentType.substr(0, semicolonPos);
    }

    // Trim whitespace and convert to lowercase for case-insensitive comparison
    boost::algorithm::trim(actualContentType);
    boost::algorithm::to_lower(actualContentType);

    std::string expectedContentType(contentType);
    boost::algorithm::to_lower(expectedContentType);

    if (actualContentType != expectedContentType) {
      bad_request(response, request, "Content type mismatch");
      return false;
    }
    return true;
  }

  /**
   * @brief Get a unique client identifier for CSRF token management.
   * @param request The HTTP request object.
   * @return A unique identifier based on username or IP address.
   */
  std::string get_client_id(const req_https_t &request) {
    // Try to use the authenticated username as client ID
    if (const auto auth = request->header.find("authorization"); !config::sunshine.username.empty() && auth != request->header.end()) {
      if (const auto &rawAuth = auth->second; rawAuth.rfind("Basic "sv, 0) == 0) {
        auto authData = SimpleWeb::Crypto::Base64::decode(rawAuth.substr("Basic "sv.length()));
        if (const auto index = static_cast<int>(authData.find(':')); index < authData.size() - 1) {
          return authData.substr(0, index);  // Return username
        }
      }
    }

    // Fall back to IP address if no username
    return net::addr_to_normalized_string(request->remote_endpoint().address());
  }

  /**
   * @brief Generate a new CSRF token for a client.
   * @param client_id A unique identifier for the client (e.g., session ID or username).
   * @return The generated CSRF token.
   */
  std::string generate_csrf_token(const std::string &client_id) {
    // Generate a cryptographically secure random token
    std::string token = crypto::rand_alphabet(CSRF_TOKEN_SIZE);

    std::scoped_lock lock(csrf_tokens_mutex);

    // Clean up expired tokens first
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(csrf_tokens, [&now](const auto &entry) {
      return entry.second.expiration < now;
    });

    // Store the token with expiration
    csrf_tokens[client_id] = csrf_token_t {
      token,
      now + CSRF_TOKEN_LIFETIME
    };

    return token;
  }

  /**
   * @brief Validate a stored CSRF token for a client against a provided token string.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param client_id A unique identifier for the client.
   * @param provided_token The token string to validate.
   * @return True if the token is valid, false otherwise.
   */
  bool validate_stored_csrf_token(const resp_https_t &response, const req_https_t &request, const std::string_view client_id, const std::string_view provided_token) {
    std::scoped_lock lock(csrf_tokens_mutex);
    const auto token_it = csrf_tokens.find(client_id);

    if (token_it == csrf_tokens.end()) {
      auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
      BOOST_LOG(error) << "Web UI: ["sv << address << "] -- CSRF token validation failed: no token found for client"sv;
      bad_request(response, request, "Invalid CSRF token");
      return false;
    }

    if (const auto now = std::chrono::steady_clock::now(); token_it->second.expiration < now) {
      csrf_tokens.erase(token_it);
      auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
      BOOST_LOG(error) << "Web UI: ["sv << address << "] -- CSRF token validation failed: token expired"sv;
      bad_request(response, request, "CSRF token expired");
      return false;
    }

    if (token_it->second.token != provided_token) {
      auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
      BOOST_LOG(error) << "Web UI: ["sv << address << "] -- CSRF token validation failed: token mismatch"sv;
      bad_request(response, request, "Invalid CSRF token");
      return false;
    }

    return true;
  }

  bool validate_csrf_token(const resp_https_t &response, const req_https_t &request, const std::string &client_id) {
    // Helper function to check if a URL starts with any allowed origin
    auto is_allowed_origin = [](const std::string_view url) {
      return std::ranges::any_of(config::sunshine.csrf_allowed_origins, [&url](const std::string &allowed_origin) {
        // Ensure exact prefix match (with ":" or "/" after to prevent malicious.com matching allowed.com)
        if (url.rfind(allowed_origin, 0) != 0) {  // rfind with pos=0 checks if the url starts with allowed_origin
          return false;
        }
        // Check that it's followed by ":" (port) or "/" (path) or is an exact match
        const size_t len = allowed_origin.length();
        return url.length() == len || url[len] == ':' || url[len] == '/';
      });
    };

    // Check if the request is from the same origin (Origin or Referer header matches configured allowed origins)
    const auto origin_it = request->header.find("Origin");
    if (origin_it != request->header.end() && is_allowed_origin(origin_it->second)) {
      // Same origin request - allow without CSRF token
      return true;
    }

    // If we have a Referer header, check if it's same-origin
    const auto referer_it = request->header.find("Referer");
    if (referer_it != request->header.end() && is_allowed_origin(referer_it->second)) {
      // Same origin request - allow without CSRF token
      return true;
    }

    // If neither Origin nor Referer is present, this cannot be a browser-initiated CSRF attack.
    // Non-browser clients (e.g. curl, scripts) never send these headers, and a malicious web page
    // cannot cause a non-browser client to make requests on a user's behalf.
    if (origin_it == request->header.end() && referer_it == request->header.end()) {
      return true;
    }

    // A browser-like request arrived with an Origin/Referer that doesn't match an allowed origin.
    // Require a CSRF token.
    const std::string_view blocked_origin = (origin_it != request->header.end()) ? origin_it->second : referer_it->second;
    // Extract token from X-CSRF-Token header
    const auto header_it = request->header.find("X-CSRF-Token");
    if (header_it == request->header.end()) {
      // Also check query parameters as fallback
      auto query_params = request->parse_query_string();
      const auto query_it = query_params.find("csrf_token");
      if (query_it == query_params.end()) {
        auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
        BOOST_LOG(error) << "Web UI: ["sv << address << "] -- CSRF protection blocked request from origin: "sv << blocked_origin;
        BOOST_LOG(error) << "Web UI: To allow this origin, add it to the 'csrf_allowed_origins' option in your Sunshine configuration"sv;
        bad_request(response, request, "Missing CSRF token");
        return false;
      }

      return validate_stored_csrf_token(response, request, client_id, query_it->second);
    }

    // Validate token from header
    return validate_stored_csrf_token(response, request, client_id, header_it->second);
  }

  /**
   * @brief Validates the application index and sends an error response if invalid.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param index The application index/id.
   */
  bool check_app_index(const resp_https_t &response, const req_https_t &request, int index) {
    std::string file = file_handler::read_file(config::stream.file_apps.c_str());
    nlohmann::json file_tree = nlohmann::json::parse(file);
    if (const auto &apps = file_tree["apps"]; index < 0 || index >= static_cast<int>(apps.size())) {
      std::string error;
      if (const int max_index = static_cast<int>(apps.size()) - 1; max_index < 0) {
        error = "No applications found";
      } else {
        error = std::format("'index' {} out of range, max index is {}", index, max_index);
      }
      bad_request(response, request, error);
      return false;
    }
    return true;
  }

  /**
   * @brief Get an HTML page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param html_file The HTML file to serve (relative to WEB_DIR).
   * @param require_auth Whether to require authentication (default: true).
   * @param redirect_if_username If true, redirect to "/" when the username is set (for welcome page).
   */
  void getPage(const resp_https_t &response, const req_https_t &request, const char *html_file, const bool require_auth, const bool redirect_if_username) {
    // Special handling for welcome page: redirect if the username is already set
    if (redirect_if_username && !config::sunshine.username.empty()) {
      send_redirect(response, request, "/");
      return;
    }

    if (require_auth && !authenticate(response, request)) {
      return;
    }

    print_req(request);

    const std::string content = file_handler::read_file((std::string(WEB_DIR) + html_file).c_str());
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");

    // prevent click jacking
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");

    response->write(content, headers);
  }

  /**
   * @brief Get the favicon image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @todo combine function with getSunshineLogoImage and possibly getNodeModules
   * @todo use mime_types map
   */
  void getFaviconImage(const resp_https_t &response, const req_https_t &request) {
    print_req(request);

    std::ifstream in(WEB_DIR "images/sunshine.ico", std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/x-icon");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief Get the Sunshine logo image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @todo combine function with getFaviconImage and possibly getNodeModules
   * @todo use mime_types map
   */
  void getSunshineLogoImage(const resp_https_t &response, const req_https_t &request) {
    print_req(request);

    std::ifstream in(WEB_DIR "images/logo-sunshine-45.png", std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief Check if a path is a child of another path.
   * @param base The base path.
   * @param query The path to check.
   * @return True if the path is a child of the base path, false otherwise.
   */
  bool isChildPath(fs::path const &base, fs::path const &query) {
    auto relPath = fs::relative(base, query);
    return *(relPath.begin()) != fs::path("..");
  }

  /**
   * @brief Get an asset.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getAsset(const resp_https_t &response, const req_https_t &request) {
    print_req(request);
    fs::path webDirPath(WEB_DIR);
    fs::path nodeModulesPath(webDirPath / "assets");

    // .relative_path is needed to shed any leading slash that might exist in the request path
    auto filePath = fs::weakly_canonical(webDirPath / fs::path(request->path).relative_path());

    // Don't do anything if the file does not exist or is outside the assets directory
    if (!isChildPath(filePath, nodeModulesPath)) {
      BOOST_LOG(warning) << "Someone requested a path " << filePath << " that is outside the assets folder";
      bad_request(response, request);
      return;
    }
    if (!fs::exists(filePath)) {
      not_found(response, request);
      return;
    }

    auto relPath = fs::relative(filePath, webDirPath);
    // get the mime type from the file extension mime_types map
    // remove the leading period from the extension
    auto mimeType = mime_types.find(relPath.extension().string().substr(1));
    // check if the extension is in the map at the x position
    if (mimeType == mime_types.end()) {
      bad_request(response, request);
      return;
    }

    // if it is, set the content type to the mime type
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", mimeType->second);
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    std::ifstream in(filePath.string(), std::ios::binary);
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief Get a CSRF token for the authenticated user.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/csrf-token| GET| null}
   */
  void getCSRFToken(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::string client_id = get_client_id(request);
    std::string token = generate_csrf_token(client_id);

    nlohmann::json output_tree;
    output_tree["csrf_token"] = token;
    send_response(response, output_tree);
  }

  /**
   * @brief Get the list of available applications.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps| GET| null}
   */
  void getApps(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json file_tree = nlohmann::json::parse(content);

      // Legacy versions of Sunshine used strings for boolean and integers, let's convert them
      // List of keys to convert to boolean
      const std::vector<std::string> boolean_keys = {
        "exclude-global-prep-cmd",
        "elevated",
        "auto-detach",
        "wait-all"
      };

      // List of keys to convert to integers
      std::vector<std::string> integer_keys = {
        "exit-timeout"
      };

      // Walk fileTree and convert true/false strings to boolean or integer values
      for (auto &app : file_tree["apps"]) {
        for (const auto &key : boolean_keys) {
          if (app.contains(key) && app[key].is_string()) {
            app[key] = app[key] == "true";
          }
        }
        for (const auto &key : integer_keys) {
          if (app.contains(key) && app[key].is_string()) {
            app[key] = std::stoi(app[key].get<std::string>());
          }
        }
        if (app.contains("prep-cmd")) {
          for (auto &prep : app["prep-cmd"]) {
            if (prep.contains("elevated") && prep["elevated"].is_string()) {
              prep["elevated"] = prep["elevated"] == "true";
            }
          }
        }
      }

      send_response(response, file_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "GetApps: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Save an application. To save a new application, the index must be `-1`. To update an existing application, you must provide the current index of the application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the post request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "name": "Application Name",
   *   "output": "Log Output Path",
   *   "cmd": "Command to run the application",
   *   "index": -1,
   *   "exclude-global-prep-cmd": false,
   *   "elevated": false,
   *   "auto-detach": true,
   *   "wait-all": true,
   *   "exit-timeout": 5,
   *   "prep-cmd": [
   *     {
   *       "do": "Command to prepare",
   *       "undo": "Command to undo preparation",
   *       "elevated": false
   *     }
   *   ],
   *   "detached": [
   *     "Detached command"
   *   ],
   *   "image-path": "Full path to the application image. Must be a png file."
   * }
   * @endcode
   *
   * @api_examples{/api/apps| POST| {"name":"Hello, World!","index":-1}}
   */
  void saveApp(const resp_https_t &response, const req_https_t &request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      // TODO: Input Validation
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss);
      std::string file = file_handler::read_file(config::stream.file_apps.c_str());
      BOOST_LOG(info) << file;
      nlohmann::json file_tree = nlohmann::json::parse(file);

      if (input_tree["prep-cmd"].empty()) {
        input_tree.erase("prep-cmd");
      }

      if (input_tree["detached"].empty()) {
        input_tree.erase("detached");
      }

      auto &apps_node = file_tree["apps"];
      int index = input_tree["index"].get<int>();  // this will intentionally cause an exception if the provided value is the wrong type

      input_tree.erase("index");

      if (index == -1) {
        apps_node.push_back(input_tree);
      } else {
        nlohmann::json newApps = nlohmann::json::array();
        for (size_t i = 0; i < apps_node.size(); ++i) {
          if (i == index) {
            newApps.push_back(input_tree);
          } else {
            newApps.push_back(apps_node[i]);
          }
        }
        file_tree["apps"] = newApps;
      }

      // Sort the apps array by name
      std::sort(apps_node.begin(), apps_node.end(), [](const nlohmann::json &a, const nlohmann::json &b) {
        return a["name"].get<std::string>() < b["name"].get<std::string>();
      });

      file_handler::write_file(config::stream.file_apps.c_str(), file_tree.dump(4));
      proc::refresh(config::stream.file_apps);

      output_tree["status"] = true;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SaveApp: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Close the currently running application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps/close| POST| null}
   */
  void closeApp(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    proc::proc.terminate();

    nlohmann::json output_tree;
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Delete an application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps/9999| DELETE| null}
   */
  void deleteApp(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    try {
      nlohmann::json output_tree;
      nlohmann::json new_apps = nlohmann::json::array();
      const int index = std::stoi(request->path_match[1]);

      if (!check_app_index(response, request, index)) {
        return;
      }

      std::string file = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json file_tree = nlohmann::json::parse(file);
      auto &apps = file_tree["apps"];

      for (size_t i = 0; i < apps.size(); ++i) {
        if (i != index) {
          new_apps.push_back(apps[i]);
        }
      }
      file_tree["apps"] = new_apps;

      file_handler::write_file(config::stream.file_apps.c_str(), file_tree.dump(4));
      proc::refresh(config::stream.file_apps);

      output_tree["status"] = true;
      output_tree["result"] = std::format("application {} deleted", index);
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "DeleteApp: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Get the list of paired clients.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/clients/list| GET| null}
   */
  void getClients(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    const nlohmann::json named_certs = nvhttp::get_all_clients();

    nlohmann::json output_tree;
    output_tree["named_certs"] = named_certs;
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Enable or disable a client.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "uuid": "<uuid>",
   *   "enabled": true
   * }
   * @endcode
   *
   * @api_examples{/api/clients/update| POST| {"uuid":"<uuid>","enabled":true}}
   */
  void updateClient(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }
    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;
      std::string uuid = input_tree.value("uuid", "");
      bool enabled = input_tree.value("enabled", true);
      bool ok = nvhttp::set_client_enabled(uuid, enabled);

      // VipleStream §M.1 — optionally also update is_admin in the same call.
      // The Web UI clients page sends both fields together when the admin
      // toggle changes; legacy callers that only send {uuid, enabled} keep
      // their existing behaviour.
      if (input_tree.contains("is_admin")) {
        bool is_admin = input_tree.value("is_admin", false);
        ok = nvhttp::set_client_admin(uuid, is_admin) && ok;
      }

      output_tree["status"] = ok;

      if (!enabled && ok) {
        rtsp_stream::terminate_sessions();

        if (rtsp_stream::session_count() == 0 && proc::proc.running() > 0) {
          proc::proc.terminate();
        }
      }

      send_response(response, output_tree);
    } catch (nlohmann::json::exception &e) {
      BOOST_LOG(warning) << "Update Client: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief VipleStream §N — Receive file from client picker page.
   *
   * 流程：
   *   1. 使用者在 stream 中右鍵 server tray → "Receive file from client..."
   *   2. tray callback 已 queue 一個 LIST_DIR 命令給 client + 開瀏覽器到 /transfer
   *   3. 本 endpoint 服務 HTML 頁，頁面 JS 每 1s 抓 /transfer/listing/latest
   *      直到拿到 client 回傳的目錄列表，渲染成可選清單。
   *   4. 使用者按 Pull → POST /transfer/pull?filename=foo → server 排
   *      UPLOAD_FROM_CLIENT 命令；client 下次 poll 拿到、開始上傳。
   *
   * 採內聯 HTML/JS（不走 Vue 3 bundle）— 此頁只在 in-stream tray flow 用，
   * 不需要跟 Web UI 主 SPA 整合。
   */
  void xferPage(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);

    static const std::string html = R"HTML(<!DOCTYPE html>
<html lang="zh-TW">
<head>
<meta charset="utf-8">
<title>VipleStream — Receive from client</title>
<style>
  body { font-family: -apple-system, "Segoe UI", system-ui; background: #1a1a1a; color: #e0e0e0; padding: 18px; margin: 0; }
  h1 { font-size: 18px; margin: 0 0 8px; }
  .hint { color: #888; font-size: 12px; margin-bottom: 12px; }
  .pathbar { display: flex; gap: 6px; margin-bottom: 8px; }
  .pathbar input { flex: 1; background: #222; color: #eee; border: 1px solid #444; padding: 6px 10px; border-radius: 4px; font-family: monospace; font-size: 13px; }
  .pathbar button { padding: 6px 14px; }
  .breadcrumb { color: #888; font-size: 12px; margin-bottom: 8px; word-break: break-all; }
  .breadcrumb a { color: #4a9eff; cursor: pointer; text-decoration: none; }
  .breadcrumb a:hover { text-decoration: underline; }
  table { width: 100%; border-collapse: collapse; }
  th, td { text-align: left; padding: 6px 10px; border-bottom: 1px solid #2a2a2a; }
  th { color: #888; font-size: 11px; text-transform: uppercase; }
  tr:hover { background: #232323; }
  td.name { display: flex; align-items: center; gap: 8px; }
  td.name a { color: #79bdff; cursor: pointer; text-decoration: none; }
  td.name a:hover { text-decoration: underline; }
  .icon { display: inline-block; width: 16px; text-align: center; }
  .size { color: #aaa; font-variant-numeric: tabular-nums; min-width: 80px; }
  button { background: #4a9eff; color: white; border: none; padding: 5px 12px; border-radius: 4px; cursor: pointer; font-size: 12px; }
  button:hover { background: #5badff; }
  button:disabled { background: #555; cursor: default; }
  button.up { background: #555; }
  #status { margin: 10px 0; padding: 8px 12px; background: #2a2a2a; border-radius: 4px; min-height: 1em; font-size: 13px; }
  .err { color: #ff7777; }
  .ok { color: #77ff77; }
  #progress { margin: 8px 0; }
  #progressBar { width: 100%; height: 6px; background: #333; border-radius: 3px; overflow: hidden; }
  #progressFill { height: 100%; background: #4a9eff; width: 0%; transition: width 0.3s; }
  #progressText { font-size: 12px; color: #aaa; margin-top: 4px; }
</style>
</head>
<body>
<h1>Receive from client</h1>
<div class="hint">Browse client filesystem. Click a folder to enter; "Pull" downloads file or folder (folder is zipped first) to server's Downloads.</div>

<div class="pathbar">
  <button class="up" id="upBtn" title="Up one level">⬆</button>
  <input type="text" id="pathInput" placeholder="Client absolute path (空白 = Downloads)" />
  <button id="goBtn">Go</button>
  <button id="homeBtn" title="Home (Downloads)">🏠</button>
</div>
<div class="breadcrumb" id="breadcrumb"></div>

<div id="status">Loading…</div>
<div id="progress" style="display:none">
  <div id="progressBar"><div id="progressFill"></div></div>
  <div id="progressText"></div>
</div>

<table id="filesTable" style="display:none">
  <thead><tr><th>Name</th><th>Size</th><th></th></tr></thead>
  <tbody id="filesBody"></tbody>
</table>

<script>
const statusEl = document.getElementById('status');
const table = document.getElementById('filesTable');
const tbody = document.getElementById('filesBody');
const progress = document.getElementById('progress');
const progressFill = document.getElementById('progressFill');
const progressText = document.getElementById('progressText');
const pathInput = document.getElementById('pathInput');
const breadcrumb = document.getElementById('breadcrumb');
let currentPath = '';     // empty = client Downloads (server default)
let currentSep = '\\';    // detected when listing arrives; defaults to backslash for Windows
let activeToken = null;
let progressTimer = null;
let pollingTimer = null;

function fmtSize(n) {
  if (n < 0) return '-';
  if (n < 1024) return n + ' B';
  if (n < 1024*1024) return (n/1024).toFixed(1) + ' KB';
  if (n < 1024*1024*1024) return (n/1024/1024).toFixed(1) + ' MB';
  return (n/1024/1024/1024).toFixed(2) + ' GB';
}

function escapeHtml(s) {
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}

function detectSep(path) {
  if (path.includes('\\')) return '\\';
  if (path.includes('/')) return '/';
  return currentSep;
}

function parentPath(path) {
  if (!path) return '';
  // Strip trailing separator
  let p = path.replace(/[\\/]+$/, '');
  const lastSep = Math.max(p.lastIndexOf('\\'), p.lastIndexOf('/'));
  if (lastSep <= 0) return '';
  // Special-case "C:\foo" -> "C:\" (keep the root)
  if (lastSep === 2 && p[1] === ':') return p.slice(0, 3);
  return p.slice(0, lastSep);
}

function joinPath(base, name) {
  if (!base) return name;
  const sep = currentSep;
  const trimmed = base.replace(/[\\/]+$/, '');
  return trimmed + sep + name;
}

function renderBreadcrumb(path) {
  if (!path) {
    breadcrumb.innerHTML = '<em>Client default Downloads</em>';
    return;
  }
  const sep = detectSep(path);
  currentSep = sep;
  const parts = path.split(/[\\/]+/).filter(Boolean);
  let cumulative = '';
  const links = [];
  for (let i = 0; i < parts.length; ++i) {
    if (i === 0 && /^[A-Za-z]:$/.test(parts[0])) {
      cumulative = parts[0] + sep;
    } else {
      cumulative = cumulative ? cumulative.replace(/[\\/]+$/, '') + sep + parts[i] : sep + parts[i];
    }
    const target = cumulative;
    links.push(`<a data-path="${escapeHtml(target)}">${escapeHtml(parts[i])}</a>`);
  }
  breadcrumb.innerHTML = links.join(' <span style="color:#555">' + sep + '</span> ');
  for (const a of breadcrumb.querySelectorAll('a')) {
    a.onclick = () => navigateTo(a.dataset.path);
  }
}

function navigateTo(path) {
  currentPath = path || '';
  pathInput.value = currentPath;
  renderBreadcrumb(currentPath);
  statusEl.textContent = 'Loading ' + (currentPath || 'Downloads') + '…';
  table.style.display = 'none';
  tbody.innerHTML = '';
  // Ask server to refresh listing for this path
  fetch('/transfer/listing/latest?path=' + encodeURIComponent(currentPath), { credentials: 'include' })
      .catch(() => {});
  if (pollingTimer) clearTimeout(pollingTimer);
  pollingTimer = setTimeout(pollListing, 600);
}

async function pollListing() {
  try {
    const r = await fetch('/transfer/listing/latest', { credentials: 'include' });
    if (r.status === 204) {
      pollingTimer = setTimeout(pollListing, 800);
      return;
    }
    if (!r.ok) {
      statusEl.innerHTML = '<span class="err">Failed to fetch listing: '+r.status+'</span>';
      return;
    }
    const entries = await r.json();
    if (!Array.isArray(entries)) {
      statusEl.innerHTML = '<span class="err">Bad listing payload</span>';
      return;
    }
    // Sort: folders first, then files, both alphabetical
    entries.sort((a, b) => {
      if ((a.is_directory ? 1 : 0) !== (b.is_directory ? 1 : 0)) {
        return a.is_directory ? -1 : 1;
      }
      return a.name.localeCompare(b.name);
    });
    statusEl.textContent = entries.length + ' item(s) in ' + (currentPath || 'Downloads');
    table.style.display = '';
    tbody.innerHTML = '';
    for (const e of entries) {
      const tr = document.createElement('tr');
      const tdName = document.createElement('td');
      tdName.className = 'name';
      const isDir = !!e.is_directory;
      tdName.innerHTML = `<span class="icon">${isDir ? '📁' : '📄'}</span>`;
      if (isDir) {
        const a = document.createElement('a');
        a.textContent = e.name;
        a.onclick = () => navigateTo(joinPath(currentPath || (e.parent || ''), e.name));
        tdName.appendChild(a);
      } else {
        const span = document.createElement('span');
        span.textContent = e.name;
        tdName.appendChild(span);
      }
      tr.appendChild(tdName);

      const tdSize = document.createElement('td');
      tdSize.className = 'size';
      tdSize.textContent = isDir ? '-' : fmtSize(e.size);
      tr.appendChild(tdSize);

      const tdAct = document.createElement('td');
      const btn = document.createElement('button');
      btn.textContent = isDir ? 'Pull as zip' : 'Pull';
      btn.onclick = () => pull(e.name, e.size, isDir);
      tdAct.appendChild(btn);
      tr.appendChild(tdAct);

      tbody.appendChild(tr);
    }
  } catch (err) {
    statusEl.innerHTML = '<span class="err">Error: '+err+'</span>';
  }
}

async function pull(filename, size, isDir) {
  document.querySelectorAll('button').forEach(b => b.disabled = true);
  try {
    const params = new URLSearchParams();
    params.set('filename', filename);
    params.set('size', String(size || 0));
    params.set('is_directory', isDir ? '1' : '0');
    if (currentPath) params.set('path', currentPath);
    const r = await fetch('/transfer/pull?' + params.toString(),
                         { method: 'POST', credentials: 'include' });
    if (!r.ok) {
      statusEl.innerHTML = '<span class="err">Pull failed: '+r.status+'</span>';
      document.querySelectorAll('button').forEach(b => b.disabled = false);
      return;
    }
    const data = await r.json();
    activeToken = data.token;
    statusEl.textContent = (isDir ? 'Receiving folder (zipping on client): ' : 'Receiving ') + filename + '…';
    progress.style.display = '';
    progressTimer = setInterval(updateProgress, 800);
  } catch (err) {
    statusEl.innerHTML = '<span class="err">Pull error: '+err+'</span>';
  }
}

async function updateProgress() {
  if (!activeToken) return;
  try {
    const r = await fetch('/transfer/progress-proxy?token=' + activeToken, { credentials: 'include' });
    if (!r.ok) return;
    const t = await r.json();
    if (t.total_bytes > 0) {
      const pct = Math.floor(t.bytes_done * 100 / t.total_bytes);
      progressFill.style.width = pct + '%';
      progressText.textContent = pct + '% (' + fmtSize(t.bytes_done) + ' / ' + fmtSize(t.total_bytes) + ')';
    } else {
      progressFill.style.width = '50%';
      progressText.textContent = fmtSize(t.bytes_done) + ' received';
    }
    if (t.state === 'done') {
      clearInterval(progressTimer);
      progressFill.style.width = '100%';
      statusEl.innerHTML = '<span class="ok">完成：' + escapeHtml(t.filename) + ' → server Downloads</span>';
      document.querySelectorAll('button').forEach(b => b.disabled = false);
      activeToken = null;
    } else if (t.state === 'failed' || t.state === 'canceled') {
      clearInterval(progressTimer);
      statusEl.innerHTML = '<span class="err">失敗：' + escapeHtml(t.error || t.state) + '</span>';
      document.querySelectorAll('button').forEach(b => b.disabled = false);
      activeToken = null;
    }
  } catch (err) { /* keep retrying */ }
}

document.getElementById('goBtn').onclick = () => navigateTo(pathInput.value.trim());
document.getElementById('homeBtn').onclick = () => navigateTo('');
document.getElementById('upBtn').onclick = () => navigateTo(parentPath(currentPath));
pathInput.addEventListener('keydown', (ev) => {
  if (ev.key === 'Enter') navigateTo(pathInput.value.trim());
});

renderBreadcrumb('');
pollListing();
</script>
</body>
</html>
)HTML";
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    response->write(SimpleWeb::StatusCode::success_ok, html, headers);
  }

  /**
   * @brief VipleStream §N — return the cached listing for the current stream owner.
   * 204 if not yet ready (client hasn't responded), 200 with JSON array otherwise.
   *
   * 額外：query `?path=...` 觸發換目錄（重新 queue list_dir 給 client，本次
   * 回 204，下次 fetch 才能拿新 listing）。
   */
  void xferListing(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }
    auto owner = proc::proc.running_owner_uuid();
    if (proc::proc.running() <= 0 || owner.empty()) {
      response->write(SimpleWeb::StatusCode::client_error_conflict, R"({"error":"no_active_stream"})");
      return;
    }
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");

    auto args = request->parse_query_string();
    auto it = args.find("path");
    if (it != args.end()) {
      // Web UI 要求換目錄 — 重 queue list_dir + 立即回 204 讓 UI poll
      file_transfer::manager::instance().queue_list_dir(owner, it->second);
      response->write(SimpleWeb::StatusCode::success_no_content, std::string {}, headers);
      return;
    }

    auto listing = file_transfer::manager::instance().take_listing(owner);
    if (listing.empty()) {
      response->write(SimpleWeb::StatusCode::success_no_content, std::string {}, headers);
      return;
    }
    response->write(SimpleWeb::StatusCode::success_ok, listing, headers);
  }

  /**
   * @brief VipleStream §N — user clicked "Pull" in the web UI; queue an
   * UPLOAD_FROM_CLIENT command targeted at the current stream owner.
   */
  void xferPull(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }
    auto owner = proc::proc.running_owner_uuid();
    if (proc::proc.running() <= 0 || owner.empty()) {
      response->write(SimpleWeb::StatusCode::client_error_conflict, R"({"error":"no_active_stream"})");
      return;
    }
    auto args = request->parse_query_string();
    std::string filename;
    std::string path;
    std::uint64_t size = 0;
    bool is_directory = false;
    {
      auto it = args.find("filename");
      if (it == args.end() || it->second.empty()) {
        response->write(SimpleWeb::StatusCode::client_error_bad_request, R"({"error":"missing_filename"})");
        return;
      }
      filename = it->second;
    }
    {
      auto it = args.find("path");
      if (it != args.end()) path = it->second;
    }
    {
      auto it = args.find("size");
      if (it != args.end()) {
        try { size = std::stoull(it->second); } catch (...) { size = 0; }
      }
    }
    {
      auto it = args.find("is_directory");
      if (it != args.end()) {
        is_directory = (it->second == "1" || it->second == "true");
      }
    }
    auto token = file_transfer::manager::instance().queue_receive_from_client(
        owner, path, filename, is_directory, size);
    if (token.empty()) {
      response->write(SimpleWeb::StatusCode::client_error_bad_request, R"({"error":"bad_filename"})");
      return;
    }
    nlohmann::json out;
    out["token"] = token;
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    response->write(SimpleWeb::StatusCode::success_ok, out.dump(), headers);
  }

  /**
   * @brief VipleStream §N — progress proxy for the web UI page (so it can
   * read transfer progress without dealing with paired-cert HTTPS).
   */
  void xferProgressProxy(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }
    auto args = request->parse_query_string();
    auto it = args.find("token");
    if (it == args.end()) {
      response->write(SimpleWeb::StatusCode::client_error_bad_request, R"({"error":"missing_token"})");
      return;
    }
    auto t = file_transfer::manager::instance().get_transfer(it->second);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    if (!t) {
      response->write(SimpleWeb::StatusCode::client_error_not_found, R"({"error":"unknown_token"})", headers);
      return;
    }
    const char *state_str = "unknown";
    switch (t->st) {
      case file_transfer::state::pending: state_str = "pending"; break;
      case file_transfer::state::running: state_str = "running"; break;
      case file_transfer::state::done: state_str = "done"; break;
      case file_transfer::state::canceled: state_str = "canceled"; break;
      case file_transfer::state::failed: state_str = "failed"; break;
    }
    nlohmann::json out;
    out["token"] = t->token;
    out["state"] = state_str;
    out["bytes_done"] = t->bytes_done;
    out["total_bytes"] = t->total_bytes;
    out["filename"] = t->display_name;
    out["error"] = t->error_msg;
    response->write(SimpleWeb::StatusCode::success_ok, out.dump(), headers);
  }

  /**
   * @brief VipleStream §M.1 — Get information about the currently running session.
   * Used by the Web UI multi-user dashboard to show owner / app / start time
   * and surface an admin "Force Disconnect" button.
   *
   * @api_examples{/api/current-session| GET| null}
   */
  void getCurrentSession(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree = nvhttp::get_current_session();
    send_response(response, output_tree);
  }

  /**
   * @brief VipleStream §M.1 — Admin force-cancel the currently running session.
   * Bypasses the per-paired-device ownership check on /cancel because the
   * admin authenticated through the Web UI (HTTP basic auth + CSRF) is by
   * definition the host operator.
   *
   * @api_examples{/api/force-cancel| POST| null}
   */
  void forceCancelSession(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }
    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = nvhttp::force_cancel_current_session();
    send_response(response, output_tree);
  }

  /**
   * @brief Unpair a client.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *  "uuid": "<uuid>"
   * }
   * @endcode
   *
   * @api_examples{/api/unpair| POST| {"uuid":"1234"}}
   */
  void unpair(const resp_https_t &response, const req_https_t &request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();

    try {
      // TODO: Input Validation
      nlohmann::json output_tree;
      const nlohmann::json input_tree = nlohmann::json::parse(ss);
      const std::string uuid = input_tree.value("uuid", "");
      const bool removed = nvhttp::unpair_client(uuid);
      output_tree["status"] = removed;

      if (removed && nvhttp::get_all_clients().empty()) {
        proc::proc.terminate();
      }

      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "Unpair: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Unpair all clients.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/clients/unpair-all| POST| null}
   */
  void unpairAll(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    nvhttp::erase_all_clients();
    proc::proc.terminate();

    nlohmann::json output_tree;
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Get the configuration settings.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/config| GET| null}
   */
  void getConfig(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["platform"] = SUNSHINE_PLATFORM;
    output_tree["version"] = PROJECT_VERSION;

    auto vars = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));

    for (auto &[name, value] : vars) {
      output_tree[name] = std::move(value);
    }

    send_response(response, output_tree);
  }

  /**
   * @brief Get the locale setting. This endpoint does not require authentication.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/configLocale| GET| null}
   */
  void getLocale(const resp_https_t &response, const req_https_t &request) {
    // we need to return the locale whether authenticated or not

    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["locale"] = config::sunshine.locale;
    send_response(response, output_tree);
  }

  /**
   * @brief Save the configuration settings.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "key": "value"
   * }
   * @endcode
   *
   * @attention{It is recommended to ONLY save the config settings that differ from the default behavior.}
   *
   * @api_examples{/api/config| POST| {"key":"value"}}
   */
  void saveConfig(const resp_https_t &response, const req_https_t &request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      // TODO: Input Validation
      std::stringstream config_stream;
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss);
      for (const auto &[k, v] : input_tree.items()) {
        if (v.is_null() || (v.is_string() && v.get<std::string>().empty())) {
          continue;
        }

        // v.dump() will dump valid json, which we do not want for strings in the config, right now
        // we should migrate the config file to straight JSON and get rid of all this nonsense
        config_stream << k << " = " << (v.is_string() ? v.get<std::string>() : v.dump()) << std::endl;
      }
      file_handler::write_file(config::sunshine.config_file.c_str(), config_stream.str());
      output_tree["status"] = true;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SaveConfig: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Get an application's image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @note{The index in the url path is the application index.}
   *
   * @api_examples{/api/covers/9999 | GET| null}
   */
  void getCover(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      const int index = std::stoi(request->path_match[1]);
      if (!check_app_index(response, request, index)) {
        return;
      }

      std::string file = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json file_tree = nlohmann::json::parse(file);
      auto &apps = file_tree["apps"];

      auto &app = apps[index];

      // Get the image path from the app configuration
      std::string app_image_path;
      if (app.contains("image-path") && !app["image-path"].is_null()) {
        app_image_path = app["image-path"];
      }

      // Use validate_app_image_path to resolve and validate the path
      // This handles extension validation, PNG signature validation, and path resolution
      std::string validated_path = proc::validate_app_image_path(app_image_path);

      // Check if we got the default image path (means validation failed or no image configured)
      if (validated_path == DEFAULT_APP_IMAGE_PATH) {
        BOOST_LOG(debug) << "Application at index " << index << " does not have a valid cover image";
        not_found(response, request, "Cover image not found");
        return;
      }

      // Open and stream the validated file
      std::ifstream in(validated_path, std::ios::binary);
      if (!in) {
        BOOST_LOG(warning) << "Unable to read cover image file: " << validated_path;
        bad_request(response, request, "Unable to read cover image file");
        return;
      }

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "image/png");
      headers.emplace("X-Frame-Options", "DENY");
      headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");

      response->write(SimpleWeb::StatusCode::success_ok, in, headers);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "GetCover: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Upload a cover image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the post request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "key": "igdb_<game_id>",
   *   "url": "https://images.igdb.com/igdb/image/upload/t_cover_big_2x/<slug>.png"
   * }
   * @endcode
   *
   * @api_examples{/api/covers/upload| POST| {"key":"igdb_1234","url":"https://images.igdb.com/igdb/image/upload/t_cover_big_2x/abc123.png"}}
   */
  void uploadCover(const resp_https_t &response, const req_https_t &request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss);

      std::string key = input_tree.value("key", "");
      if (key.empty()) {
        bad_request(response, request, "Cover key is required");
        return;
      }
      std::string url = input_tree.value("url", "");

      const std::string coverdir = platf::appdata().string() + "/covers/";
      file_handler::make_directory(coverdir);

      std::basic_string path = coverdir + http::url_escape(key) + ".png";
      if (!url.empty()) {
        if (http::url_get_host(url) != "images.igdb.com") {
          bad_request(response, request, "Only images.igdb.com is allowed");
          return;
        }
        if (!http::download_file(url, path)) {
          bad_request(response, request, "Failed to download cover");
          return;
        }
      } else {
        auto data = SimpleWeb::Crypto::Base64::decode(input_tree.value("data", ""));

        std::ofstream imgfile(path);
        imgfile.write(data.data(), static_cast<int>(data.size()));
      }
      output_tree["status"] = true;
      output_tree["path"] = path;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "UploadCover: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Get the logs from the log file.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/logs| GET| null}
   */
  void getLogs(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(config::sunshine.log_file.c_str());
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/plain");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(SimpleWeb::StatusCode::success_ok, content, headers);
  }

  /**
   * @brief Update existing credentials.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the post request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "currentUsername": "Current Username",
   *   "currentPassword": "Current Password",
   *   "newUsername": "New Username",
   *   "newPassword": "New Password",
   *   "confirmNewPassword": "Confirm New Password"
   * }
   * @endcode
   *
   * @api_examples{/api/password| POST| {"currentUsername":"admin","currentPassword":"admin","newUsername":"admin","newPassword":"admin","confirmNewPassword":"admin"}}
   */
  void savePassword(const resp_https_t &response, const req_https_t &request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!config::sunshine.username.empty() && !authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    std::vector<std::string> errors = {};
    std::stringstream ss;
    std::stringstream config_stream;
    ss << request->content.rdbuf();
    try {
      // TODO: Input Validation
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss);
      std::string username = input_tree.value("currentUsername", "");
      std::string newUsername = input_tree.value("newUsername", "");
      std::string password = input_tree.value("currentPassword", "");
      std::string newPassword = input_tree.value("newPassword", "");
      std::string confirmPassword = input_tree.value("confirmNewPassword", "");
      if (newUsername.empty()) {
        newUsername = username;
      }
      if (newUsername.empty()) {
        errors.emplace_back("Invalid Username");
      } else {
        auto hash = util::hex(crypto::hash(password + config::sunshine.salt)).to_string();
        if (config::sunshine.username.empty() || (boost::iequals(username, config::sunshine.username) && hash == config::sunshine.password)) {
          if (newPassword.empty() || newPassword != confirmPassword) {
            errors.emplace_back("Password Mismatch");
          } else {
            http::save_user_creds(config::sunshine.credentials_file, newUsername, newPassword);
            http::reload_user_creds(config::sunshine.credentials_file);
            output_tree["status"] = true;
          }
        } else {
          errors.emplace_back("Invalid Current Credentials");
        }
      }

      if (!errors.empty()) {
        // join the errors array
        std::string error = std::accumulate(errors.begin(), errors.end(), std::string(), [](const std::string &a, const std::string &b) {
          return a.empty() ? b : a + ", " + b;
        });
        bad_request(response, request, error);
        return;
      }

      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SavePassword: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Send a pin code to the host. The pin is generated from the Moonlight client during the pairing process.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the post request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "pin": "<pin>",
   *   "name": "Friendly Client Name"
   * }
   * @endcode
   *
   * @api_examples{/api/pin| POST| {"pin":"1234","name":"My PC"}}
   */
  void savePin(const resp_https_t &response, const req_https_t &request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss);
      std::string name = input_tree.value("name", "");
      const std::string pin = input_tree.value("pin", "");

      // VipleStream §M.1.f.3 (v1.4.139) — name 驗證.  Web UI 的 pin.html 有
      // `required` 屬性但 client-side 不可靠 (whitespace 一個空白 / curl
      // 直接打 / DOM 改掉 required 都能繞).  名字進到 named_devices.json 後
      // 變成 owner_name → 之後 /launch 503 訊息 "Server in use by ..." 帶這
      // 個 name, Web UI session 列表也顯示.  空白會變成「unnamed device」,
      // 讓使用者完全不知道是哪台 device 卡住 owner lock.  Trim + 拒空白 +
      // 長度上限保證進到 state 的 name 永遠有意義.
      auto trim = [](std::string s) {
        size_t b = s.find_first_not_of(" \t\r\n");
        size_t e = s.find_last_not_of(" \t\r\n");
        if (b == std::string::npos) return std::string {};
        return s.substr(b, e - b + 1);
      };
      name = trim(name);
      if (name.empty()) {
        bad_request(response, request, "Device name is required (cannot be empty or whitespace-only)");
        return;
      }
      if (name.size() > 64) {
        bad_request(response, request, "Device name too long (max 64 characters)");
        return;
      }

      int _pin = 0;
      _pin = std::stoi(pin);
      if (_pin < 0 || _pin > 9999) {
        bad_request(response, request, "PIN must be between 0000 and 9999");
        return;
      }

      output_tree["status"] = nvhttp::pin(pin, name);
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SavePin: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Reset the display device persistence.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/reset-display-device-persistence| POST| null}
   */
  void resetDisplayDevicePersistence(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = display_device::reset_persistence();
    send_response(response, output_tree);
  }

  /**
   * @brief Restart Sunshine.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/restart| POST| null}
   */
  void restart(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    // We may not return from this call
    platf::restart();
  }

  /**
   * @brief Get ViGEmBus driver version and installation status.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/vigembus/status| GET| null}
   */
  void getViGEmBusStatus(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;

#ifdef _WIN32
    std::string version_str;
    bool installed = false;
    bool version_compatible = false;

    // Check if ViGEmBus driver exists
    std::filesystem::path driver_path = std::filesystem::path(std::getenv("SystemRoot") ? std::getenv("SystemRoot") : "C:\\Windows") / "System32" / "drivers" / "ViGEmBus.sys";

    if (std::filesystem::exists(driver_path)) {
      installed = platf::getFileVersionInfo(driver_path, version_str);
      if (installed) {
        // Parse version string to check compatibility (>= 1.17.0.0)
        std::vector<std::string> version_parts;
        std::stringstream ss(version_str);
        std::string part;
        while (std::getline(ss, part, '.')) {
          version_parts.push_back(part);
        }

        if (version_parts.size() >= 2) {
          int major = std::stoi(version_parts[0]);
          int minor = std::stoi(version_parts[1]);
          version_compatible = (major > 1) || (major == 1 && minor >= 17);
        }
      }
    }

    output_tree["installed"] = installed;
    output_tree["version"] = version_str;
    output_tree["version_compatible"] = version_compatible;
    output_tree["packaged_version"] = VIGEMBUS_PACKAGED_VERSION;
#else
    output_tree["error"] = "ViGEmBus is only available on Windows";
    output_tree["installed"] = false;
    output_tree["version"] = "";
    output_tree["version_compatible"] = false;
    output_tree["packaged_version"] = "";
#endif

    send_response(response, output_tree);
  }

  /**
   * @brief Install ViGEmBus driver with elevated permissions.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/vigembus/install| POST| null}
   */
  void installViGEmBus(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;

#ifdef _WIN32
    // Get the path to the vigembus installer
    const std::filesystem::path installer_path = platf::appdata().parent_path() / "scripts" / "vigembus_installer.exe";

    if (!std::filesystem::exists(installer_path)) {
      output_tree["status"] = false;
      output_tree["error"] = "ViGEmBus installer not found";
      send_response(response, output_tree);
      return;
    }

    // Run the installer with elevated permissions
    std::error_code ec;
    boost::filesystem::path working_dir = boost::filesystem::path(installer_path.string()).parent_path();
    boost::process::v1::environment env = boost::this_process::environment();

    // Run with elevated permissions, non-interactive
    const std::string install_cmd = std::format("{} /quiet", installer_path.string());
    auto child = platf::run_command(true, false, install_cmd, working_dir, env, nullptr, ec, nullptr);

    if (ec) {
      output_tree["status"] = false;
      output_tree["error"] = "Failed to start installer: " + ec.message();
      send_response(response, output_tree);
      return;
    }

    // Wait for the installer to complete
    child.wait(ec);

    if (ec) {
      output_tree["status"] = false;
      output_tree["error"] = "Installer failed: " + ec.message();
    } else {
      int exit_code = child.exit_code();
      output_tree["status"] = (exit_code == 0);
      output_tree["exit_code"] = exit_code;
      if (exit_code != 0) {
        output_tree["error"] = std::format("Installer exited with code {}", exit_code);
      }
    }
#else
    output_tree["status"] = false;
    output_tree["error"] = "ViGEmBus installation is only available on Windows";
#endif

    send_response(response, output_tree);
  }

  /**
   * @brief Checks whether a directory entry qualifies as an executable file.
   * @param entry The directory entry to check.
   * @param status The cached file status for the entry.
   * @return True if the file should be included in an executable-type listing.
   */
  bool is_browsable_executable([[maybe_unused]] const fs::directory_entry &entry, [[maybe_unused]] const fs::file_status &status) {
#ifdef _WIN32
    auto ext = entry.path().extension().string();
    boost::algorithm::to_lower(ext);
    return ext == ".exe" || ext == ".bat" || ext == ".cmd" || ext == ".com" || ext == ".ps1";
#else
    const auto perms = status.permissions();
    return (perms & fs::perms::owner_exec) != fs::perms::none ||
           (perms & fs::perms::group_exec) != fs::perms::none ||
           (perms & fs::perms::others_exec) != fs::perms::none;
#endif
  }

#ifdef _WIN32
  /**
   * @brief Builds a JSON array of available Windows drive letters.
   * @return JSON array of drive-letter entries.
   */
  nlohmann::json get_windows_drives() {
    nlohmann::json entries = nlohmann::json::array();
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
      if (drives & (1 << i)) {
        const auto drive_letter = static_cast<char>('A' + i);
        const auto drive_path = std::string(1, drive_letter) + ":\\";
        nlohmann::json entry;
        entry["name"] = drive_path;
        entry["type"] = "directory";
        entry["path"] = drive_path;
        entries.push_back(entry);
      }
    }
    return entries;
  }
#endif

  /**
   * @brief Lists, filters, and sorts the entries of a directory for the browse API.
   * @param dir_path The directory to list.
   * @param type_str Filter type: "directory", "executable", "file", or "any".
   * @return Sorted JSON array of entry objects with name/type/path fields.
   */
  nlohmann::json build_browse_entries(const fs::path &dir_path, const std::string &type_str) {
    nlohmann::json entries = nlohmann::json::array();

    std::error_code iter_ec;
    for (auto it = fs::directory_iterator(dir_path, fs::directory_options::skip_permission_denied, iter_ec);
         !iter_ec && it != fs::directory_iterator();
         it.increment(iter_ec)) {
      try {
        const auto status = it->status();
        const bool is_dir = fs::is_directory(status);

        if (const bool is_regular = fs::is_regular_file(status); !is_dir && !is_regular) {
          continue;
        }

        // Apply type filter (directories are always included for navigation)
        if (type_str == "directory" && !is_dir) {
          continue;
        }

        if (type_str == "executable" && !is_dir && !is_browsable_executable(*it, status)) {
          continue;
        }

        nlohmann::json file_entry;
        file_entry["name"] = it->path().filename().string();
        file_entry["path"] = it->path().string();
        file_entry["type"] = is_dir ? "directory" : "file";
        entries.push_back(file_entry);
      } catch (const fs::filesystem_error &e) {
        BOOST_LOG(debug) << "BrowseDirectory: skipping entry due to error: "sv << e.what();
      }
    }

    if (iter_ec) {
      BOOST_LOG(debug) << "BrowseDirectory: directory iteration error: "sv << iter_ec.message();
    }

    // Sort: directories first, then files; both case-insensitively alphabetical
    std::sort(entries.begin(), entries.end(), [](const nlohmann::json &a, const nlohmann::json &b) {
      const bool a_dir = (a["type"] == "directory");
      if (const bool b_dir = (b["type"] == "directory"); a_dir != b_dir) {
        return a_dir && !b_dir;
      }
      auto a_name = a["name"].get<std::string>();
      auto b_name = b["name"].get<std::string>();
      boost::algorithm::to_lower(a_name);
      boost::algorithm::to_lower(b_name);
      return a_name < b_name;
    });

    return entries;
  }

  /**
   * @brief Browse the server filesystem.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @note On Windows, an empty or root path returns the list of available drive letters.
   * @note On non-Windows, an empty path defaults to the filesystem root ("/").
   *
   * @api_examples{/api/browse?path=/home/user&type=directory| GET| null}
   */
  void browseDirectory(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      const auto query_params = request->parse_query_string();

      std::string path_str;
      if (const auto path_it = query_params.find("path"); path_it != query_params.end()) {
        path_str = path_it->second;
      }

      std::string type_str = "any";
      if (const auto type_it = query_params.find("type"); type_it != query_params.end() && !type_it->second.empty()) {
        type_str = type_it->second;
      }

      nlohmann::json output_tree;

#ifdef _WIN32
      // On Windows with an empty or root path, return the list of available drive letters
      if (path_str.empty() || path_str == "/" || path_str == "\\") {
        output_tree["path"] = "";
        output_tree["parent"] = "";
        output_tree["entries"] = get_windows_drives();
        send_response(response, output_tree);
        return;
      }
#else
      // On non-Windows, default an empty path to the filesystem root
      if (path_str.empty()) {
        path_str = "/";
      }
#endif

      // Normalize the path
      fs::path dir_path = fs::weakly_canonical(fs::path(path_str));

      // If the path points to a file, use its parent directory
      std::error_code ec;
      if (fs::is_regular_file(dir_path, ec)) {
        dir_path = dir_path.parent_path();
      }

      // If the path doesn't exist, try the parent
      if (!fs::exists(dir_path, ec)) {
        dir_path = dir_path.parent_path();
      }

      if (!fs::is_directory(dir_path, ec)) {
        bad_request(response, request, "Path is not a directory");
        return;
      }

      output_tree["path"] = dir_path.string();

      // Determine the parent path for the "Up" navigation
      const fs::path parent = dir_path.parent_path();
#ifdef _WIN32
      // At a drive root (e.g., C:\) the parent equals itself; signal the drive list with an empty string
      output_tree["parent"] = (parent == dir_path) ? "" : parent.string();
#else
      output_tree["parent"] = parent.string();
#endif

      output_tree["entries"] = build_browse_entries(dir_path, type_str);
      send_response(response, output_tree);
    } catch (const fs::filesystem_error &e) {
      BOOST_LOG(warning) << "BrowseDirectory: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  void start() {
    platf::set_thread_name("confighttp");
    const auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    const auto port_https = net::map_port(PORT_HTTPS);
    const auto address_family = net::af_from_enum_string(config::sunshine.address_family);

    https_server_t server {config::nvhttp.cert, config::nvhttp.pkey};

    // Helper to create page handler lambdas without repeating the signature
    auto page_handler = [](const char *file, bool require_auth = true, bool redirect_if_username = false) {
      return [file, require_auth, redirect_if_username](const resp_https_t &response, const req_https_t &request) {
        getPage(response, request, file, require_auth, redirect_if_username);
      };
    };

    // Default resource handlers
    const https_handler_t bad_request_handler = [](const resp_https_t &response, const req_https_t &request) {
      bad_request(response, request);
    };
    const https_handler_t not_found_handler = [](const resp_https_t &response, const req_https_t &request) {
      not_found(response, request);
    };

    // error by default
    server.default_resource["DELETE"] = bad_request_handler;
    server.default_resource["PATCH"] = bad_request_handler;
    server.default_resource["POST"] = bad_request_handler;
    server.default_resource["PUT"] = bad_request_handler;
    server.default_resource["GET"] = not_found_handler;

    // web pages
    server.resource["^/$"]["GET"] = page_handler("index.html");
    server.resource["^/apps/?$"]["GET"] = page_handler("apps.html");
    server.resource["^/clients/?$"]["GET"] = page_handler("clients.html");
    server.resource["^/config/?$"]["GET"] = page_handler("config.html");
    server.resource["^/featured/?$"]["GET"] = page_handler("featured.html");
    server.resource["^/password/?$"]["GET"] = page_handler("password.html");
    server.resource["^/pin/?$"]["GET"] = page_handler("pin.html");
    server.resource["^/troubleshooting/?$"]["GET"] = page_handler("troubleshooting.html");
    server.resource["^/welcome/?$"]["GET"] = page_handler("welcome.html", false, true);

    // rest api
    server.resource["^/api/browse$"]["GET"] = browseDirectory;
    server.resource["^/api/apps$"]["GET"] = getApps;
    server.resource["^/api/apps$"]["POST"] = saveApp;
    server.resource["^/api/apps/([0-9]+)$"]["DELETE"] = deleteApp;
    server.resource["^/api/apps/close$"]["POST"] = closeApp;
    server.resource["^/api/clients/list$"]["GET"] = getClients;
    server.resource["^/api/clients/unpair$"]["POST"] = unpair;
    server.resource["^/api/clients/unpair-all$"]["POST"] = unpairAll;
    server.resource["^/api/clients/update$"]["POST"] = updateClient;
    // VipleStream §M.1 — multi-user dashboard endpoints.
    server.resource["^/api/current-session$"]["GET"] = getCurrentSession;
    server.resource["^/api/force-cancel$"]["POST"] = forceCancelSession;
    server.resource["^/api/config$"]["GET"] = getConfig;
    server.resource["^/api/config$"]["POST"] = saveConfig;
    server.resource["^/api/configLocale$"]["GET"] = getLocale;
    server.resource["^/api/covers/([0-9]+)$"]["GET"] = getCover;
    server.resource["^/api/covers/upload$"]["POST"] = uploadCover;
    server.resource["^/api/csrf-token$"]["GET"] = getCSRFToken;
    server.resource["^/api/password$"]["POST"] = savePassword;
    server.resource["^/api/pin$"]["POST"] = savePin;
    server.resource["^/api/logs$"]["GET"] = getLogs;
    server.resource["^/api/reset-display-device-persistence$"]["POST"] = resetDisplayDevicePersistence;
    server.resource["^/api/restart$"]["POST"] = restart;
    server.resource["^/api/vigembus/status$"]["GET"] = getViGEmBusStatus;
    server.resource["^/api/vigembus/install$"]["POST"] = installViGEmBus;

    // VipleStream §N — in-stream file transfer Receive picker UI
    server.resource["^/transfer$"]["GET"]                = xferPage;
    server.resource["^/transfer/listing/latest$"]["GET"] = xferListing;
    server.resource["^/transfer/pull$"]["POST"]          = xferPull;
    server.resource["^/transfer/progress-proxy$"]["GET"] = xferProgressProxy;

    // static/dynamic resources
    server.resource["^/images/sunshine.ico$"]["GET"] = getFaviconImage;
    server.resource["^/images/logo-sunshine-45.png$"]["GET"] = getSunshineLogoImage;
    server.resource["^/assets\\/.+$"]["GET"] = getAsset;

    server.config.reuse_address = true;
    server.config.address = net::get_bind_address(address_family);
    server.config.port = port_https;

    auto accept_and_run = [&](auto *server) {
      try {
        platf::set_thread_name("confighttp::tcp");
        server->start([](const unsigned short port) {
          BOOST_LOG(info) << "Configuration UI available at [https://localhost:"sv << port << "]";
        });
      } catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling server->stop() from a different thread
        if (shutdown_event->peek()) {
          return;
        }

        BOOST_LOG(fatal) << "Couldn't start Configuration HTTPS server on port ["sv << port_https << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };
    std::thread tcp {accept_and_run, &server};

    // Wait for any event
    shutdown_event->view();

    server.stop();

    tcp.join();
  }
}  // namespace confighttp
