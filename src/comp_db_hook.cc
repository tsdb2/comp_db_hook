#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/types/span.h"
#include "common/env.h"
#include "common/flat_set.h"
#include "common/utilities.h"
#include "io/advisory_file_lock.h"
#include "io/fd.h"
#include "json/json.h"

namespace {

using ::tsdb2::io::FD;

namespace json = ::tsdb2::json;

std::string_view constexpr kCommandFilePathEnvVar = "COMP_DB_HOOK_COMMAND_FILE_PATH";
std::string_view constexpr kDefaultCommandFilePath = "compile_commands.json";

std::string_view constexpr kCompilerNameEnvVar = "COMP_DB_HOOK_COMPILER";
std::string_view constexpr kDefaultCompilerName = "clang++";

auto constexpr kCompilerFlagsWithArgument =
    tsdb2::common::fixed_flat_set_of<std::string_view>({"-include", "-o", "-target"});

char constexpr kDirectoryField[] = "directory";
char constexpr kArgumentsField[] = "arguments";
char constexpr kFileField[] = "file";

// See https://clang.llvm.org/docs/JSONCompilationDatabase.html for the format specification.
//
// NOTE: none of these fields are actually optional in our format, but we don't want to fail the
// entire run if for any reason we can't find one or more of them for one or more entries.
using CommandEntry =
    json::Object<json::Field<std::optional<std::string>, kDirectoryField>,
                 json::Field<std::optional<std::vector<std::string>>, kArgumentsField>,
                 json::Field<std::optional<std::string>, kFileField>>;

using CommandEntries = std::vector<CommandEntry>;

std::string GetCommandFilePath() {
  return tsdb2::common::GetEnv(std::string(kCommandFilePathEnvVar))
      .value_or(std::string(kDefaultCommandFilePath));
}

absl::StatusOr<FD> OpenCommandFile() {
  auto const file_path = GetCommandFilePath();
  FD fd{::open(file_path.c_str(), /*flags=*/O_CREAT | O_CLOEXEC | O_RDWR,
               /*mode=*/0664)};
  if (fd) {
    return std::move(fd);
  } else {
    return absl::ErrnoToStatus(errno, "open");
  }
}

absl::StatusOr<CommandEntries> ParseCommandFile(FD const& fd) {
  std::string json;
  static size_t constexpr kBufferSize = 4096;
  char buffer[kBufferSize];
  ssize_t result = 0;
  do {
    result = ::read(fd.get(), buffer, kBufferSize);
    if (result < 0) {
      return absl::ErrnoToStatus(errno, "read");
    } else if (result > 0) {
      json += std::string_view(buffer, result);
    }
  } while (!(result < kBufferSize));
  auto status_or_entries = json::Parse<CommandEntries>(json);
  if (status_or_entries.ok()) {
    return status_or_entries;
  } else {
    return CommandEntries();
  }
}

std::vector<std::string> MakeArguments(int const argc, char const* const argv[]) {
  std::vector<std::string> result;
  result.reserve(argc);
  for (size_t i = 0; i < argc; ++i) {
    result.emplace_back(argv[i]);
  }
  return result;
}

std::string GetFullPath(std::string const base_directory, std::string const file_name) {
  if (base_directory.empty() || absl::StartsWith(file_name, "/")) {
    return std::string(file_name);
  } else if (absl::EndsWith(base_directory, "/")) {
    return absl::StrCat(base_directory, file_name);
  } else {
    return absl::StrCat(base_directory, "/", file_name);
  }
}

tsdb2::common::flat_set<std::string> GetCurrentFiles(absl::Span<std::string const> const args) {
  tsdb2::common::flat_set<std::string> files;
  for (size_t i = 0; i < args.size(); ++i) {
    if (kCompilerFlagsWithArgument.contains(args[i])) {
      ++i;
    } else if (!absl::StartsWith(args[i], "-")) {
      files.emplace(args[i]);
    }
  }
  return files;
}

std::string GetCurrentWorkingDirectory() { return std::string(::get_current_dir_name()); }

void UpdateEntries(absl::Span<std::string const> arguments, CommandEntries* const entries) {
  auto source_files = GetCurrentFiles(arguments);
  for (auto& entry : *entries) {
    auto const maybe_file_name = entry.get<kFileField>();
    if (!maybe_file_name.has_value()) {
      LOG(ERROR) << GetCommandFilePath() << " contains an entry without a `file` field:\n"
                 << json::Stringify(entry);
      continue;
    }
    auto const& file_name = maybe_file_name.value();
    auto const base_directory = entry.get<kDirectoryField>().value_or("");
    auto const full_path = GetFullPath(base_directory, file_name);
    if (source_files.erase(full_path)) {
      entry.get<kArgumentsField>() = std::vector<std::string>(arguments.begin(), arguments.end());
    }
  }
  auto const cwd = GetCurrentWorkingDirectory();
  for (auto const& file : source_files) {
    entries->emplace_back(CommandEntry{
        json::kInitialize,
        /*directory=*/cwd,
        /*arguments=*/std::vector<std::string>(arguments.begin(), arguments.end()),
        /*file=*/file,
    });
  }
}

absl::Status RewriteFile(FD const& fd, CommandEntries const& entries) {
  if (::ftruncate(*fd, 0) < 0) {
    return absl::ErrnoToStatus(errno, "ftruncate");
  }
  if (::lseek(*fd, 0, SEEK_SET) < 0) {
    return absl::ErrnoToStatus(errno, "ftruncate");
  }
  auto const json = tsdb2::json::Stringify(entries);
  size_t written = 0;
  while (written < json.size()) {
    auto const result = ::write(*fd, json.data() + written, json.size() - written);
    if (result < 0) {
      return absl::ErrnoToStatus(errno, "write");
    }
    written += result;
  }
  return absl::OkStatus();
}

absl::Status UpdateCommandFile(int const argc, char const* const argv[]) {
  DEFINE_CONST_OR_RETURN(fd, OpenCommandFile());
  DEFINE_OR_RETURN(lock, tsdb2::io::ExclusiveFileLock::Acquire(fd));
  DEFINE_VAR_OR_RETURN(entries, ParseCommandFile(fd));
  auto const arguments = MakeArguments(argc - 1, argv + 1);
  UpdateEntries(arguments, &entries);
  return RewriteFile(fd, entries);
}

}  // namespace

int main(int const argc, char* const argv[]) {
  CHECK_OK(UpdateCommandFile(argc - 1, argv + 1));
  auto const compiler_name = tsdb2::common::GetEnv(std::string(kCompilerNameEnvVar))
                                 .value_or(std::string(kDefaultCompilerName));
  ::execvp(compiler_name.c_str(), argv + 1);
  LOG(ERROR) << absl::ErrnoToStatus(errno, "execvp");
  return 1;
}
