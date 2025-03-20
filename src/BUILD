load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

cc_binary(
    name = "comp_db_hook",
    srcs = ["comp_db_hook.cc"],
    deps = [
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/log:initialize",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/types:span",
        "@com_tsdb2_platform//common:env",
        "@com_tsdb2_platform//common:flat_set",
        "@com_tsdb2_platform//common:utilities",
        "@com_tsdb2_platform//io:advisory_file_lock",
        "@com_tsdb2_platform//io:fd",
        "@com_tsdb2_platform//json",
    ],
)
