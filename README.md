 # find-srcs
Recursively searches a directory in a `git` repository for all files contained therein, including files in submodules, and excluding any files specified in `.gitignore` files.

Usage:

```
find-srcs ROOT_PATH
```

## Constraints
 - ROOT_PATH must be an absolute or relative path to a directory, not a regular file.
 - The directory indicated by ROOT_PATH must be contained within a git repository.
 - If ROOT_PATH is a symlink, it must not point to a regular file, be dangling, or point to a file system loop.
 - The user running `find-srcs` must have read access to ROOT_PATH and every non-ignored subdirectory thereof.
 - The `git` command-line tool must be installed and accessible on the system PATH.

## Output Details
- On success, the command returns `0`. If an error occurs, the command returns `1`.
- The absolute path of each file found is printed to standard output.
- If the command fails, it will print debugging information to standard error. Some paths may still be printed on standard output.
- Symbolic links are included in the output. They are not followed and instead treated as regular files.
- Directories are not included in the output, except for directory symbolic links, since `git` treats these as regular files.
- The paths output will not include any file which is returned when `git check-ignore` is run from the repository containing that file.
- The paths output will not include files from `.git` directories, unless ROOT_PATH is itself inside a `.git` directory.
- The paths output will include files from submodules within ROOT_PATH, except for files already excluded above.

## Example

```
$ git clone git@github.com:github/teach.github.com.git
Cloning into 'teach.github.com'...
remote: Enumerating objects: 8151, done.
remote: Total 8151 (delta 0), reused 0 (delta 0), pack-reused 8151 (from 1)
Receiving objects: 100% (8151/8151), 38.40 MiB | 2.21 MiB/s, done.
Resolving deltas: 100% (4242/4242), done.
$ ~/find-srcs/build/find-srcs teach.github.com/
/tmp/teach.github.com/LICENSE.md
/tmp/teach.github.com/README.md
$ ls -a teach.github.com/
.  ..  .git  LICENSE.md  README.md
$
```

## Building

To build the project, just run `build.sh` from the root directory of the project.

The executable is built from a single C++ file whose only dependency is the standard library. Therefore, to build a C++ compiler with
alias `c++` must be available on the path. A command-line installation of `git` is a dependency at runtime which gets called
via `execlp`, but it is not involved in the compilation process.

The build script produces an MD5 checksum of the source file contents when building so that rebuilds which would produce the same executable
as is already built can be detected and skipped. This method is more reliable than using file modification times, since the system clock
on some computers may be inaccurate and there are many ways that a file's last modification time can be updated without the actual content
of the file changing.
