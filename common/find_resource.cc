#include "drake/common/find_resource.h"

#include <cstdlib>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "drake/common/drake_marker.h"
#include "drake/common/drake_throw.h"
#include "drake/common/find_loaded_library.h"
#include "drake/common/find_runfiles.h"
#include "drake/common/never_destroyed.h"
#include "drake/common/text_logging.h"

using std::getenv;
using std::string;

namespace drake {

using Result = FindResourceResult;

optional<string>
Result::get_absolute_path() const {
  return absolute_path_;
}

string
Result::get_absolute_path_or_throw() const {
  // If we have a path, return it.
  const auto& optional_path = get_absolute_path();
  if (optional_path) { return *optional_path; }

  // Otherwise, throw the error message.
  const auto& optional_error = get_error_message();
  DRAKE_ASSERT(optional_error != nullopt);
  throw std::runtime_error(*optional_error);
}

optional<string>
Result::get_error_message() const {
  // If an error has been set, return it.
  if (error_message_ != nullopt) {
    DRAKE_ASSERT(absolute_path_ == nullopt);
    return error_message_;
  }

  // If successful, return no-error.
  if (absolute_path_ != nullopt) {
    return nullopt;
  }

  // Both optionals are null; we are empty; return a default error message.
  DRAKE_ASSERT(resource_path_.empty());
  return string("No resource was requested (empty result)");
}

string Result::get_resource_path() const {
  return resource_path_;
}

Result Result::make_success(string resource_path, string absolute_path) {
  DRAKE_THROW_UNLESS(!resource_path.empty());
  DRAKE_THROW_UNLESS(!absolute_path.empty());

  Result result;
  result.resource_path_ = std::move(resource_path);
  result.absolute_path_ = std::move(absolute_path);
  result.CheckInvariants();
  return result;
}

Result Result::make_error(string resource_path, string error_message) {
  DRAKE_THROW_UNLESS(!resource_path.empty());
  DRAKE_THROW_UNLESS(!error_message.empty());

  Result result;
  result.resource_path_ = std::move(resource_path);
  result.error_message_ = std::move(error_message);
  result.CheckInvariants();
  return result;
}

Result Result::make_empty() {
  Result result;
  result.CheckInvariants();
  return result;
}

void Result::CheckInvariants() {
  if (resource_path_.empty()) {
    // For our "empty" state, both success and error must be empty.
    DRAKE_DEMAND(absolute_path_ == nullopt);
    DRAKE_DEMAND(error_message_ == nullopt);
  } else {
    // For our "non-empty" state, we must have exactly one of success or error.
    DRAKE_DEMAND((absolute_path_ == nullopt) != (error_message_ == nullopt));
  }
  // When non-nullopt, the path and error cannot be the empty string.
  DRAKE_DEMAND((absolute_path_ == nullopt) || !absolute_path_->empty());
  DRAKE_DEMAND((error_message_ == nullopt) || !error_message_->empty());
}

namespace {

bool StartsWith(const string& str, const string& prefix) {
  return str.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(const string& str, const string& suffix) {
  if (suffix.size() > str.size()) { return false; }
  return str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Returns true iff the path is relative (not absolute nor empty).
bool IsRelativePath(const string& path) {
  return !path.empty() && (path[0] != '/');
}

// Add a commented-out macro, so that the deprecation grep will find it:
// DRAKE_DEPRECATED("2019-08-01", "See below")
void WarnDeprecatedDirectory(const string& resource_path) {
  static const logging::Warn log_once(
      "Using drake::FindResource to locate a directory (e.g., '{}') "
      "is deprecated, and will become an error after 2019-08-01. "
      "Always request a file within the directory instead, e.g., find "
      "'drake/manipulation/models/iiwa_description/package.xml', not "
      "'drake/manipulation/models/iiwa_description'.", resource_path);
}

// Taking `root` to be Drake's resource root, confirm that the sentinel file
// exists and return the found resource_path (or an error if either the
// sentinel or resource_path was missing).
Result CheckAndMakeResult(
    const string& root_description, const string& root,
    const string& resource_path) {
  DRAKE_DEMAND(!root_description.empty());
  DRAKE_DEMAND(!root.empty());
  DRAKE_DEMAND(!resource_path.empty());
  DRAKE_DEMAND(internal::IsDir(root));
  DRAKE_DEMAND(IsRelativePath(resource_path));

  // Check for the sentinel.
  const char* const sentinel_relpath = "drake/.drake-find_resource-sentinel";
  if (!internal::IsFile(root + "/" + sentinel_relpath)) {
    return Result::make_error(resource_path, fmt::format(
        "Could not find Drake resource_path '{}' because {} specified a "
        "resource root of '{}' but that root did not contain the expected "
        "sentinel file '{}'.",
        resource_path, root_description, root, sentinel_relpath));
  }

  // Check for the resource_path.
  const string abspath = root + '/' + resource_path;
  if (internal::IsDir(abspath)) {
    // As a compatibility shim, allow directory resources for now.
    WarnDeprecatedDirectory(resource_path);
    return Result::make_success(resource_path, abspath);
  }
  if (!internal::IsFile(abspath)) {
    return Result::make_error(resource_path, fmt::format(
        "Could not find Drake resource_path '{}' because {} specified a "
        "resource root of '{}' but that root did not contain the expected "
        "file '{}'.",
        resource_path, root_description, root, abspath));
  }

  return Result::make_success(resource_path, abspath);
}

// Opportunistically searches inside the attic for multibody resource paths.
// This function is not unit tested -- only acceptance-tested by the fact that
// none of the tests in the attic fail.
optional<string> MaybeFindResourceInAttic(const string& resource_path) {
  const string prefix("drake/");
  DRAKE_DEMAND(StartsWith(resource_path, prefix));
  const string substr = resource_path.substr(prefix.size());
  for (const auto& directory : {
           "multibody/collision/test",
           "multibody/parsers/test/package_map_test",
           "multibody/parsers/test/parsers_frames_test",
           "multibody/parsers/test/urdf_parser_test",
           "multibody/rigid_body_plant/test",
           "multibody/shapes/test",
           "multibody/test",
           "systems/controllers/qp_inverse_dynamics/test"
       }) {
    if (StartsWith(substr, directory)) {
      const auto rlocation_or_error =
          internal::FindRunfile(prefix + string("attic/") + substr);
      if (rlocation_or_error.error.empty()) {
        return rlocation_or_error.abspath;
      }
    }
  }
  return nullopt;
}

// If we are linked against libdrake_marker.so, and the install-tree-relative
// path resolves correctly, return it as the resource root, else return nullopt.
optional<string> MaybeGetInstallResourceRoot() {
  // Ensure that we have the library loaded.
  DRAKE_DEMAND(drake::internal::drake_marker_lib_check() == 1234);
  optional<string> libdrake_dir = LoadedLibraryPath("libdrake_marker.so");
  if (libdrake_dir) {
    const string root = *libdrake_dir + "/../share";
    if (internal::IsDir(root)) {
      return root;
    } else {
      log()->debug("FindResource ignoring CMake install candidate '{}' "
                   "because it does not exist", root);
    }
  } else {
    log()->debug("FindResource has no CMake install candidate");
  }
  return nullopt;
}

}  // namespace

const char* const kDrakeResourceRootEnvironmentVariableName =
    "DRAKE_RESOURCE_ROOT";

Result FindResource(const string& resource_path) {
  // Check if resource_path is well-formed: a relative path that starts with
  // "drake" as its first directory name.  A valid example would look like:
  // "drake/common/test/find_resource_test_data.txt".  Requiring strings passed
  // to drake::FindResource to start with "drake" is redundant, but preserves
  // compatibility with the original semantics of this function; if we want to
  // offer a function that takes paths without "drake", we can use a new name.
  if (!IsRelativePath(resource_path)) {
    return Result::make_error(resource_path, fmt::format(
        "Drake resource_path '{}' is not a relative path.",
        resource_path));
  }
  const string prefix("drake/");
  if (!StartsWith(resource_path, prefix)) {
    return Result::make_error(resource_path, fmt::format(
        "Drake resource_path '{}' does not start with {}.",
        resource_path, prefix));
  }

  // We will check each potential resource root one by one.  The first root
  // that is present will be chosen, even if does not contain the particular
  // resource_path.  We expect that all sources offer all files.

  // (1) Check the environment variable.
  if (char* guess = getenv(kDrakeResourceRootEnvironmentVariableName)) {
    const char* const env_name = kDrakeResourceRootEnvironmentVariableName;
    if (internal::IsDir(guess)) {
      return CheckAndMakeResult(
          fmt::format("{} environment variable ", env_name),
          guess, resource_path);
    } else {
      log()->debug("FindResource ignoring {}='{}' because it does not exist",
                   env_name, guess);
    }
  }

  // (2) Check the Runfiles.
  if (internal::HasRunfiles()) {
    auto rlocation_or_error = internal::FindRunfile(resource_path);
    if (rlocation_or_error.error.empty()) {
      return Result::make_success(
          resource_path, rlocation_or_error.abspath);
    }
    // As a compatibility shim, allow for directory resources for now.
    {
      const std::string sentinel_relpath =
          "drake/.drake-find_resource-sentinel";
      auto sentinel_rlocation_or_error =
          internal::FindRunfile(sentinel_relpath);
      DRAKE_THROW_UNLESS(sentinel_rlocation_or_error.error.empty());
      const std::string sentinel_abspath =
          sentinel_rlocation_or_error.abspath;
      DRAKE_THROW_UNLESS(EndsWith(sentinel_abspath, sentinel_relpath));
      const std::string resource_abspath =
          sentinel_abspath.substr(
              0, sentinel_abspath.size() - sentinel_relpath.size()) +
          resource_path;
      if (internal::IsDir(resource_abspath)) {
        WarnDeprecatedDirectory(resource_path);
        return Result::make_success(resource_path, resource_abspath);
      }
    }
    // As a compatibility shim, for resource paths that have been moved into the
    // attic, we opportunistically try a fallback search path for them.  This
    // heuristic is only helpful for source trees -- any install data files from
    // the attic should be installed without the "attic/" portion of their path.
    if (auto attic_abspath = MaybeFindResourceInAttic(resource_path)) {
      return Result::make_success(resource_path, *attic_abspath);
    }
    return Result::make_error(resource_path, rlocation_or_error.error);
  }

  // (3) Check the `libdrake_marker.so` location in the install tree.
  if (auto guess = MaybeGetInstallResourceRoot()) {
    return CheckAndMakeResult(
        "Drake CMake install marker",
        *guess, resource_path);
  }

  // No resource roots were found.
  return Result::make_error(resource_path, fmt::format(
      "Could not find Drake resource_path '{}' because no resource roots of "
      "any kind could be found: {} is unset, a {} could not be created, and "
      "there is no Drake CMake install marker.",
      resource_path, kDrakeResourceRootEnvironmentVariableName,
      "bazel::tools::cpp::runfiles::Runfiles"));
}

string FindResourceOrThrow(const string& resource_path) {
  return FindResource(resource_path).get_absolute_path_or_throw();
}

}  // namespace drake
