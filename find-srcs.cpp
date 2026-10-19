#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <sys/wait.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <vector>

using namespace std;

void print_files_tracked_by_repo_subfolder(filesystem::path gitroot, string gitroot_to_rootpath = "") {
    ifstream gitmodules(gitroot / ".gitmodules");
    vector<filesystem::path> submodules_in_rootpath;
    if (gitmodules.is_open())
    {
        regex path_key_regex("\\s*path\\s*=\\s*(.*)");
        smatch match;

        string line;
        while (getline(gitmodules, line)) {
            if (regex_match(line, match, path_key_regex)) {
                if (match[1].str().substr(0, gitroot_to_rootpath.size()) == gitroot_to_rootpath) {
                    submodules_in_rootpath.push_back(gitroot / match[1].str());
                }
            }
        }
    }

    vector<string> all_files;
    size_t max_path_length = 0;
    for (
        auto dir_it = filesystem::recursive_directory_iterator(gitroot / gitroot_to_rootpath);
        dir_it != filesystem::recursive_directory_iterator();
        dir_it++
    ) {
        if (dir_it->path().filename() == ".git") {
            dir_it.disable_recursion_pending();
            continue;
        }
        if (dir_it->is_directory() && !dir_it->is_symlink()) {
            if (find(submodules_in_rootpath.begin(), submodules_in_rootpath.end(), dir_it->path()) != submodules_in_rootpath.end()) {
                dir_it.disable_recursion_pending();
            }
        } else {
            string path = dir_it->path().string();
            all_files.push_back(path);
            max_path_length = max(path.size(), max_path_length);
        }
    }

    int to_git_fds[2];
    int from_git_fds[2];
    pipe(to_git_fds);
    pipe(from_git_fds);
    fcntl(to_git_fds[1], F_SETFD, fcntl(to_git_fds[1], F_GETFD) | FD_CLOEXEC);
    fcntl(from_git_fds[0], F_SETFD, fcntl(from_git_fds[0], F_GETFD) | FD_CLOEXEC);

    pid_t pid = fork();
    if (pid == 0) {
        dup2(to_git_fds[0], STDIN_FILENO);
        dup2(from_git_fds[1], STDOUT_FILENO);
        if (chdir(gitroot.c_str()) == 0) {
            execlp("git", "git", "check-ignore", "--stdin", nullptr);
        }
        exit(1);
    }

    close(to_git_fds[0]);
    close(from_git_fds[1]);

    for (const string &file : all_files) {
        write(to_git_fds[1], file.c_str(), file.size());
        write(to_git_fds[1], "\n", 1);
    }
    close(to_git_fds[1]);

    stringstream full_read;
    char block_read[1024];
    int waitstatus = false;
    do {
        int read_len;
        do {
            read_len = read(from_git_fds[0], block_read, sizeof(block_read));
            full_read.write(block_read, read_len);
        } while (read_len > 0);
        waitpid(pid, &waitstatus, WNOHANG);
    } while (!WIFEXITED(waitstatus));
    close(from_git_fds[0]);

    vector<string>::iterator file_iterator = all_files.begin();
    std::string file_to_skip;

    while (getline(full_read, file_to_skip)) {
        while (file_iterator->compare(file_to_skip) != 0) {
            cout << *file_iterator << endl;
            file_iterator++;
        }
        file_iterator++;
    }
    while (file_iterator != all_files.end()) {
        cout << *file_iterator << endl;
        file_iterator++;
    }

    for (const filesystem::path& sub : submodules_in_rootpath) {
        print_files_tracked_by_repo_subfolder(sub);
    }
}

int main(int argc, char** argv) {
    if (argc != 2) {
        cerr << "Usage: " << argv[0] << " ROOT_PATH" << endl;
        return 1;
    }

    bool found_gitroot = false;
    filesystem::path gitroot_to_rootpath;
    filesystem::path gitroot = filesystem::weakly_canonical(argv[1]);

    if (!filesystem::exists(gitroot)) {
        cerr << argv[1] << " (" << gitroot.string() << ") does not exist" << endl;
        return 1;
    }

    if (!filesystem::is_directory(gitroot)) {
        cerr << argv[1] << " (" << gitroot.string() << ") is not a directory" << endl;
        return 1;
    }

    while (!found_gitroot) {
        for (const filesystem::directory_entry& dir_entry : filesystem::directory_iterator(gitroot)) {
            if (dir_entry.path().filename() == ".git") {
                found_gitroot = true;
                break;
            }
        }
        if (!found_gitroot) {
            if (gitroot.parent_path() == gitroot) {
                cerr << argv[1] << " (" << filesystem::weakly_canonical(argv[1]).string() << ") is not inside a git repository" << endl;
                return 1;
            }
            gitroot_to_rootpath = gitroot.filename() / gitroot_to_rootpath;
            gitroot = gitroot.parent_path();
        }
    }

    print_files_tracked_by_repo_subfolder(gitroot, gitroot_to_rootpath.string());

    return 0;
}
