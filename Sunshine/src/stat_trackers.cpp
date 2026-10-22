/**
 * @file src/stat_trackers.cpp
 * @brief Definitions for streaming statistic tracking.
 */
// local includes
#include "stat_trackers.h"

namespace stat_trackers {

  boost::format one_digit_after_decimal() {
    static const boost::format fmt("%1$.1f");
    return fmt;
  }

  boost::format two_digits_after_decimal() {
    static const boost::format fmt("%1$.2f");
    return fmt;
  }

}  // namespace stat_trackers
