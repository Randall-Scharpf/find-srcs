#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <sys/wait.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <regex>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace std;

#define IDLE_TASK_SLEEP_US 100

class git_ignore_pruner {
    public:
        git_ignore_pruner(filesystem::path repository_path) {
            repo_root = repository_path;

            int to_git_fds[2];
            int from_git_fds[2];
            pipe(to_git_fds);
            pipe(from_git_fds);
            fcntl(to_git_fds[1], F_SETFD, fcntl(to_git_fds[1], F_GETFD) | FD_CLOEXEC);
            fcntl(from_git_fds[0], F_SETFD, fcntl(from_git_fds[0], F_GETFD) | FD_CLOEXEC);

            git_pid = fork();
            if (git_pid == 0) {
                dup2(to_git_fds[0], STDIN_FILENO);
                dup2(from_git_fds[1], STDOUT_FILENO);
                if (chdir(repository_path.c_str()) == 0) {
                    execlp("git", "git", "check-ignore", "--stdin", "--verbose", "--non-matching", nullptr);
                    perror("execlp");
                }
                exit(1);
            }

            close(to_git_fds[0]);
            close(from_git_fds[1]);
            to_git_write_stream = fdopen(to_git_fds[1], "w");
            if (to_git_write_stream == nullptr) perror("fdopen");
            from_git_read_stream = fdopen(from_git_fds[0], "r");
            if (from_git_read_stream == nullptr) perror("fdopen");

            pending_paths.store(0);
            stop_reader_thread = false;
            output_reader = thread([&]{
                char* line = nullptr;
                size_t line_alloc;
                size_t line_length;
                while (!stop_reader_thread) {
                    if ((line_length = getline(&line, &line_alloc, from_git_read_stream)) != -1) {
                        line[line_length - 1] = '\0';

                        char* first_colon = line ? strchr(line, ':') : nullptr;
                        char* second_colon = first_colon ? strchr(first_colon + 1, ':') : nullptr;
                        char* tab = second_colon ? strchr(second_colon + 1, '\t') : nullptr;

                        if (((second_colon[1] == '!') || (second_colon[1] == '\t')) && (tab != nullptr)) {
                            queued_outputs_lock.lock();
                            queued_outputs.push_back(filesystem::path(tab + 1));
                            queued_outputs_lock.unlock();
                        }
                        pending_paths.fetch_sub(1);
                    } else {
                        usleep(IDLE_TASK_SLEEP_US);
                    }
                }
                free(line);
            });
        }

        ~git_ignore_pruner() {
            fclose(to_git_write_stream);
            int waitstatus = 0;
            do {
                waitpid(git_pid, &waitstatus, WNOHANG);
            } while(!WIFEXITED(waitstatus));

            stop_reader_thread = true;
            output_reader.join();
            fclose(from_git_read_stream);
        }

        void check_path(filesystem::path path_to_check) {
            if ((path_to_check.filename() == ".git") && (path_to_check.parent_path() == repo_root)) {
                return;
            }
            string path_str = path_to_check.string();
            fputs(path_str.c_str(), to_git_write_stream);
            fputs("\n", to_git_write_stream);
            fflush(to_git_write_stream);
            pending_paths.fetch_add(1);
        }

        vector<filesystem::path> get_paths() {
            vector<filesystem::path> result;
            queued_outputs_lock.lock();
            swap(result, queued_outputs);
            queued_outputs_lock.unlock();
            return result;
        }

        bool all_paths_checked() {
            return pending_paths.load() == 0;
        }

    private:
        filesystem::path repo_root;

        thread output_reader;
        bool stop_reader_thread;

        vector<filesystem::path> queued_outputs;
        mutex queued_outputs_lock;
        atomic<size_t> pending_paths;

        pid_t git_pid;
        FILE* to_git_write_stream;
        FILE* from_git_read_stream;
};

void print_files_tracked_by_repo_subfolder(filesystem::path gitroot, string gitroot_to_rootpath = "") {
    ifstream gitmodules(gitroot / ".gitmodules");
    unordered_set<filesystem::path> submodules_in_rootpath;
    if (gitmodules.is_open())
    {
        regex path_key_regex("\\s*path\\s*=\\s*(.*)\\s*");
        smatch match;

        string line;
        while (getline(gitmodules, line)) {
            if (regex_match(line, match, path_key_regex)) {
                if (match[1].str().substr(0, gitroot_to_rootpath.size()) == gitroot_to_rootpath) {
                    submodules_in_rootpath.insert(gitroot / match[1].str());
                }
            }
        }
    }

    git_ignore_pruner repo_file_pruner(gitroot);
    git_ignore_pruner repo_folder_pruner(gitroot);

    vector<filesystem::path> folders_to_go;
    folders_to_go.push_back(gitroot);

    bool done_producing_inputs = false;
    thread output_handler([&]{
        vector<filesystem::path> files_to_go;

        while (true) {
            for (const filesystem::path& output : files_to_go) {
                cout << output.string() << endl;
            }

            bool all_inputs_processed = repo_file_pruner.all_paths_checked();
            files_to_go = repo_file_pruner.get_paths();

            if (files_to_go.empty()) {
                if (done_producing_inputs && all_inputs_processed) {
                    break;
                }
                else {
                    usleep(IDLE_TASK_SLEEP_US);
                }
            }
        }
    });

    while (true) {
        for (const filesystem::path& folder : folders_to_go) {
            for (const filesystem::directory_entry& item : filesystem::directory_iterator(folder)) {
                if (!item.is_symlink() && item.is_directory()) {
                    if (submodules_in_rootpath.find(item.path()) == submodules_in_rootpath.end()) {
                        repo_folder_pruner.check_path(item.path());
                    }
                }
                else {
                    repo_file_pruner.check_path(item.path());
                }
            }
        }

        bool all_inputs_processed = repo_folder_pruner.all_paths_checked();
        folders_to_go = repo_folder_pruner.get_paths();

        if (folders_to_go.empty()) {
            if (all_inputs_processed) {
                break;
            }
            else {
                usleep(IDLE_TASK_SLEEP_US);
            }
        }
    }

    done_producing_inputs = true;
    output_handler.join();

    for (const filesystem::path& sub : submodules_in_rootpath) {
        print_files_tracked_by_repo_subfolder(sub);
    }
}

int main(int argc, char** argv) {
    if (argc != 2) {
        cerr << "Usage: " << argv[0] << " ROOT_PATH" << endl;
        return 1;
    }

    try {
        bool found_gitroot = false;
        filesystem::path gitroot_to_rootpath;
        filesystem::path gitroot = filesystem::weakly_canonical(argv[1]);

        if (!filesystem::exists(gitroot)) {
            cerr << gitroot << " does not exist" << endl;
            return 1;
        }

        if (!filesystem::is_directory(gitroot)) {
            cerr << gitroot << " is not a directory" << endl;
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
                    cerr << filesystem::weakly_canonical(argv[1]) << " is not inside a git repository" << endl;
                    return 1;
                }
                gitroot_to_rootpath = gitroot.filename() / gitroot_to_rootpath;
                gitroot = gitroot.parent_path();
            }
        }

        print_files_tracked_by_repo_subfolder(gitroot, gitroot_to_rootpath.string());
    } catch (filesystem::filesystem_error err) {
        string problem_path = "\"" + err.path1().string() + "\"";
        if (problem_path == "\"\"") {
            problem_path = string("Some subpath of ") + argv[1];
        }

        if (err.code().value() == ELOOP) {
            cerr << problem_path << " is a symlink loop, but a directory was required" << endl;
        } else if (err.code().value() == EACCES) {
            cerr << problem_path << " is inaccessible (permission denied)" << endl;
        } else {
            cerr << err.what() << endl;
        }
        return 1;
    }

    return 0;
}
