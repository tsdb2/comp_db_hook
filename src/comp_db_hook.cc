#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/limits.h>
#include <unistd.h>

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
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

std::string_view constexpr kCompilerNameEnvVar = "COMP_DB_HOOK_COMPILER";
std::string_view constexpr kDefaultCompilerName = "clang++";

std::string_view constexpr kWorkspaceDirEnvVar = "COMP_DB_HOOK_WORKSPACE_DIR";

auto constexpr kCompilerFlagsWithArgument = tsdb2::common::fixed_flat_set_of<std::string_view>(
    {"-MF", "-include", "-iquote", "-isystem", "-o", "-target"});

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

std::string JoinPath(std::string_view const base_directory, std::string_view const file_name) {
  if (base_directory.empty() || absl::StartsWith(file_name, "/")) {
    return std::string(file_name);
  } else if (absl::EndsWith(base_directory, "/")) {
    return absl::StrCat(base_directory, file_name);
  } else {
    return absl::StrCat(base_directory, "/", file_name);
  }
}

struct SourceFile {
  // Custom "less-than" functor to index source files by their absolute path.
  struct Less {
    bool operator()(SourceFile const& lhs, SourceFile const& rhs) const {
      return lhs.absolute_path() < rhs.absolute_path();
    }
  };

  explicit SourceFile(std::string_view const base_directory, std::string_view const relative_path)
      : relative_path_(relative_path), absolute_path_(JoinPath(base_directory, relative_path)) {}

  ~SourceFile() = default;

  SourceFile(SourceFile&&) noexcept = default;
  SourceFile& operator=(SourceFile&&) noexcept = default;
  SourceFile(SourceFile const&) = default;
  SourceFile& operator=(SourceFile const&) = default;

  std::string_view relative_path() const { return relative_path_; }
  std::string_view absolute_path() const { return absolute_path_; }

 private:
  std::string relative_path_;
  std::string absolute_path_;
};

using SourceFileSet = tsdb2::common::flat_set<SourceFile, SourceFile::Less>;

absl::StatusOr<std::string> GetWorkspaceDirectory() {
  auto maybe_directory = tsdb2::common::GetEnv(std::string(kWorkspaceDirEnvVar));
  if (maybe_directory.has_value()) {
    return std::move(maybe_directory).value();
  }
  char buffer[PATH_MAX + 1];
  if (::getcwd(buffer, PATH_MAX) != nullptr) {
    return std::string(buffer);
  } else {
    return absl::ErrnoToStatus(errno, "getcwd");
  }
}

absl::StatusOr<std::string> GetCommandFilePath() {
  DEFINE_VAR_OR_RETURN(workspace_directory, GetWorkspaceDirectory());
  return JoinPath(std::move(workspace_directory), "compile_commands.json");
}

std::string GetCompilerName() {
  return tsdb2::common::GetEnv(std::string(kCompilerNameEnvVar))
      .value_or(std::string(kDefaultCompilerName));
}

absl::StatusOr<FD> OpenCommandFile() {
  DEFINE_CONST_OR_RETURN(file_path, GetCommandFilePath());
  FD fd{::open(  // NOLINT(cppcoreguidelines-pro-type-vararg)
      file_path.c_str(), /*flags=*/O_CREAT | O_CLOEXEC | O_RDWR, /*mode=*/0664)};
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
  result.emplace_back(GetCompilerName());
  for (size_t i = 1; i < argc; ++i) {
    result.emplace_back(argv[i]);
  }
  return result;
}

SourceFileSet GetCurrentFiles(std::string_view const cwd,
                              absl::Span<std::string const> const args) {
  SourceFileSet files;
  for (size_t i = 1; i < args.size(); ++i) {
    if (kCompilerFlagsWithArgument.contains(args[i])) {
      ++i;
    } else if (!absl::StartsWith(args[i], "-")) {
      files.emplace(cwd, args[i]);
    }
  }
  return files;
}

absl::Status UpdateEntries(absl::Span<std::string const> arguments, CommandEntries* const entries) {
  DEFINE_CONST_OR_RETURN(cwd, GetWorkspaceDirectory());
  auto source_files = GetCurrentFiles(cwd, arguments);
  for (auto& entry : *entries) {
    auto const maybe_file_path = entry.get<kFileField>();
    if (!maybe_file_path.has_value()) {
      LOG(ERROR) << "compile_commands.json contains an entry without a `file` field:\n"
                 << json::Stringify(entry);
      continue;
    }
    auto const base_directory = entry.get<kDirectoryField>().value_or(cwd);
    SourceFile file{base_directory, maybe_file_path.value()};
    if (source_files.erase(file) > 0) {
      entry.get<kArgumentsField>() = std::vector<std::string>(arguments.begin(), arguments.end());
    }
  }
  for (auto const& file : source_files) {
    // NOLINTBEGIN(bugprone-argument-comment)
    entries->emplace_back(CommandEntry{
        json::kInitialize,
        /*directory=*/cwd,
        /*arguments=*/std::vector<std::string>(arguments.begin(), arguments.end()),
        /*file=*/file.relative_path(),
    });
    // NOLINTEND(bugprone-argument-comment)
  }
  return absl::OkStatus();
}

absl::Status RewriteFile(FD const& fd, CommandEntries const& entries) {
  if (::ftruncate(*fd, 0) < 0) {
    return absl::ErrnoToStatus(errno, "ftruncate");
  }
  if (::lseek(*fd, 0, SEEK_SET) < 0) {
    return absl::ErrnoToStatus(errno, "lseek");
  }
  auto const json = tsdb2::json::Stringify(entries, json::StringifyOptions{.pretty = true}) + "\n";
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
  auto const arguments = MakeArguments(argc, argv);
  RETURN_IF_ERROR(UpdateEntries(arguments, &entries));
  return RewriteFile(fd, entries);
}

}  // namespace

int main(int const argc, char* const argv[]) {
  absl::InitializeLog();
  CHECK_OK(UpdateCommandFile(argc, argv));
  ::execvp(GetCompilerName().c_str(), argv);
  LOG(ERROR) << absl::ErrnoToStatus(errno, "execvp");
  return 1;
}
