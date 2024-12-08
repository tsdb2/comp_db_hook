# Compilation Database Updater

## Overview

This small program can be used in a C++ project to intercept all calls to the compiler and update a
[compilation database JSON file](https://clang.llvm.org/docs/JSONCompilationDatabase.html). The JSON file can in turn be used with many Clang-based tools like [IWYU](https://include-what-you-use.org/).

`comp_db_hook` is particularly useful with [Bazel](https://bazel.build/), which doesn't generate a
compilation database natively and is quite hard to extend.

## Installation

Requirements:

* Clang with C++17 support,
* [Bazel](https://bazel.build/).

Compile with Bazel, then run the `install.sh` script:

```sh
$ bazel build -c opt ...
$ sudo ./install.sh
```

## Usage Instructions

Just replace the compiler name with `comp_db_hook` in the compilation command line.

For example, a `clang++` invocation like the following:

```sh
$ clang++ -std=c++17 -Wall src/file.cc -lssl -lcrypto
```

can be changed to (assuming `comp_db_hook` is in the `$PATH`):

```sh
$ comp_db_hook -std=c++17 -Wall src/file.cc -lssl -lcrypto
```

`comp_db_hook` will automatically update the corresponding entry in the compilation database file
and then spawn `clang++` forwarding all received flags.

The name of the compiler spawned by `comp_db_hook` is `clang++` by default and can be changed in the
`COMP_DB_HOOK_COMPILER` environment variable. Example for `g++`:

```sh
$ env COMP_DB_HOOK_COMPILER=g++ comp_db_hook -std=c++17 -Wall src/file.cc -lssl -lcrypto
```

The (relative) path of the produced JSON file is `compile_commands.json` by default, so the file
will be created or updated in the current working directory. The path can be changed using the
`COMP_DB_HOOK_COMMAND_FILE_PATH` environment variable and can also be an absolute path.
