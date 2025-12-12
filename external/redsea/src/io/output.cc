#include "output.hh"

#include "src/group.hh"
#include "src/options.hh"
#include "src/util/tree.hh"
#include "src/util/util.hh"

#include <iomanip>
#include <ostream>
#include <sstream>
#include <variant>

// REMOVED: #include <nlohmann/json.hpp>

namespace redsea {

namespace {
// REMOVED: toJson helper function
}  // namespace

void printAsHex(const Group& group, const Options& options, std::ostream& output_ostream) {
  if (!group.isEmpty()) {
    if (group.getDataStream() > 0) {
      output_ostream << "#S" << group.getDataStream() << ' ';
    }
    output_ostream << group.asHex();
    if (options.timestamp) {
      output_ostream << ' ' << getTimePointString(group.getRxTime().value, options.time_format);
    }
    if (options.time_from_start && group.getTimeFromStart().has_value) {
      output_ostream << ' ' << std::fixed << std::setprecision(6) << group.getTimeFromStart().value;
    }
    output_ostream << '\n' << std::flush;
  }
}

void printAsJson(const ObjectTree& tree, std::ostream& output_ostream) {
  // Stubbed out to remove JSON dependency.
  // We extract data directly via C++ getters in the adapter instead.
  (void)tree;
  (void)output_ostream;
}

}  // namespace redsea
