# VipleStream: Use fixed version from project(), skip CI/git version detection
# The version is set in the top-level CMakeLists.txt via project(VipleStream VERSION x.y.z)

# set date variables
set(PROJECT_YEAR "1990")
set(PROJECT_MONTH "01")
set(PROJECT_DAY "01")

# Extract year, month, and day (do this AFTER version parsing)
# Note: Cmake doesn't support "{}" regex syntax
if(PROJECT_VERSION MATCHES "^([0-9][0-9][0-9][0-9])\\.([0-9][0-9][0-9][0-9]?)\\.([0-9]+)$")
    message("Extracting year and month/day from PROJECT_VERSION: ${PROJECT_VERSION}")
    # First capture group is the year
    set(PROJECT_YEAR "${CMAKE_MATCH_1}")

    # Second capture group contains month and day
    set(MONTH_DAY "${CMAKE_MATCH_2}")

    # Extract month (first 1-2 digits) and day (last 2 digits)
    string(LENGTH "${MONTH_DAY}" MONTH_DAY_LENGTH)
    if(MONTH_DAY_LENGTH EQUAL 3)
        # Format: MDD (e.g., 703 = month 7, day 03)
        string(SUBSTRING "${MONTH_DAY}" 0 1 PROJECT_MONTH)
        string(SUBSTRING "${MONTH_DAY}" 1 2 PROJECT_DAY)
    elseif(MONTH_DAY_LENGTH EQUAL 4)
        # Format: MMDD (e.g., 1203 = month 12, day 03)
        string(SUBSTRING "${MONTH_DAY}" 0 2 PROJECT_MONTH)
        string(SUBSTRING "${MONTH_DAY}" 2 2 PROJECT_DAY)
    endif()

    # Ensure month is two digits
    if(PROJECT_MONTH LESS 10 AND NOT PROJECT_MONTH MATCHES "^0")
        set(PROJECT_MONTH "0${PROJECT_MONTH}")
    endif()
    # Ensure day is two digits
    if(PROJECT_DAY LESS 10 AND NOT PROJECT_DAY MATCHES "^0")
        set(PROJECT_DAY "0${PROJECT_DAY}")
    endif()
endif()

# Parse PROJECT_VERSION to extract major, minor, and patch components
if(PROJECT_VERSION MATCHES "([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(PROJECT_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(CMAKE_PROJECT_VERSION_MAJOR "${CMAKE_MATCH_1}")

    set(PROJECT_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(CMAKE_PROJECT_VERSION_MINOR "${CMAKE_MATCH_2}")

    set(PROJECT_VERSION_PATCH "${CMAKE_MATCH_3}")
    set(CMAKE_PROJECT_VERSION_PATCH "${CMAKE_MATCH_3}")
endif()

# Split PROJECT_VERSION_PATCH for RC file (Windows VERSIONINFO requires values <= 65535)
# PROJECT_VERSION_PATCH can be 0-245959, so we split it into two parts:
# - Last 2 digits for RC_VERSION_REVISION
# - Leading digits for RC_VERSION_BUILD (0 if original is <= 99)
math(EXPR RC_VERSION_BUILD "${PROJECT_VERSION_PATCH} / 100")
math(EXPR RC_VERSION_REVISION "${PROJECT_VERSION_PATCH} % 100")

message("PROJECT_FQDN: ${PROJECT_FQDN}")
message("PROJECT_NAME: ${PROJECT_NAME}")
message("PROJECT_VERSION: ${PROJECT_VERSION}")
message("PROJECT_VERSION_MAJOR: ${PROJECT_VERSION_MAJOR}")
message("PROJECT_VERSION_MINOR: ${PROJECT_VERSION_MINOR}")
message("PROJECT_VERSION_PATCH: ${PROJECT_VERSION_PATCH}")
message("CMAKE_PROJECT_VERSION: ${CMAKE_PROJECT_VERSION}")
message("CMAKE_PROJECT_VERSION_MAJOR: ${CMAKE_PROJECT_VERSION_MAJOR}")
message("CMAKE_PROJECT_VERSION_MINOR: ${CMAKE_PROJECT_VERSION_MINOR}")
message("CMAKE_PROJECT_VERSION_PATCH: ${CMAKE_PROJECT_VERSION_PATCH}")
message("RC_VERSION_BUILD: ${RC_VERSION_BUILD}")
message("RC_VERSION_REVISION: ${RC_VERSION_REVISION}")
message("PROJECT_YEAR: ${PROJECT_YEAR}")
message("PROJECT_MONTH: ${PROJECT_MONTH}")
message("PROJECT_DAY: ${PROJECT_DAY}")

list(APPEND SUNSHINE_DEFINITIONS PROJECT_FQDN="${PROJECT_FQDN}")
list(APPEND SUNSHINE_DEFINITIONS PROJECT_NAME="${PROJECT_NAME}")
list(APPEND SUNSHINE_DEFINITIONS PROJECT_VERSION="${PROJECT_VERSION}")
list(APPEND SUNSHINE_DEFINITIONS PROJECT_VERSION_MAJOR="${PROJECT_VERSION_MAJOR}")
list(APPEND SUNSHINE_DEFINITIONS PROJECT_VERSION_MINOR="${PROJECT_VERSION_MINOR}")
list(APPEND SUNSHINE_DEFINITIONS PROJECT_VERSION_PATCH="${PROJECT_VERSION_PATCH}")
list(APPEND SUNSHINE_DEFINITIONS PROJECT_VERSION_COMMIT="${GITHUB_COMMIT}")
