#include <windows.h>
#include <stdio.h>
#include <string.h>

/* File redirection avoids pipe deadlocks, while retaining a per-process timeout. */
static char out[262144], err[262144];
static int failures;
static void read_output(HANDLE file, char *buffer, DWORD capacity) {
    DWORD count = 0;
    SetFilePointer(file, 0, NULL, FILE_BEGIN);
    if (!ReadFile(file, buffer, capacity - 1, &count, NULL)) failures++;
    buffer[count] = 0;
    if (count == capacity - 1) { fprintf(stderr, "Test output truncated\n"); failures++; }
}
static DWORD run(const char *exe, const char *args) {
    SECURITY_ATTRIBUTES security = { sizeof(security), NULL, TRUE };
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    char command[4096], stdout_path[MAX_PATH], stderr_path[MAX_PATH];
    HANDLE stdout_file, stderr_file, input;
    DWORD code = 1;
    out[0] = err[0] = 0;
    sprintf(stdout_path, "tests\\runner-%lu.out", (unsigned long)GetCurrentProcessId());
    sprintf(stderr_path, "tests\\runner-%lu.err", (unsigned long)GetCurrentProcessId());
    stdout_file = CreateFileA(stdout_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, &security,
        CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    stderr_file = CreateFileA(stderr_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, &security,
        CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    input = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, NULL);
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = stdout_file; startup.hStdError = stderr_file; startup.hStdInput = input;
    sprintf(command, "\"%s\" %s", exe, args);
    if (stdout_file == INVALID_HANDLE_VALUE || stderr_file == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE ||
        !CreateProcessA(exe, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process)) {
        fprintf(stderr, "Cannot start %s: %lu\n", command, (unsigned long)GetLastError());
        failures++;
    } else {
        if (WaitForSingleObject(process.hProcess, 60000) != WAIT_OBJECT_0) {
            fprintf(stderr, "Test timed out: %s\n", command);
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
            failures++;
        }
        GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
    }
    read_output(stdout_file, out, sizeof(out)); read_output(stderr_file, err, sizeof(err));
    CloseHandle(stdout_file); CloseHandle(stderr_file); CloseHandle(input);
    return code;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL runner line %d: %s\nstdout: %s\nstderr: %s\n", __LINE__, #x, out, err); failures++; } } while (0)
static int occurrences(const char *text, const char *needle) {
    int count = 0;
    while ((text = strstr(text, needle)) != NULL) { count++; text += strlen(needle); }
    return count;
}
int main(void) {
    const char *cli = "tests\\usb-eject-smoke.exe", *flow = "tests\\flow-tests.exe";
    const char *programs[] = { "usb-eject-tests", "card-tests", "portable-tests", "deadline-tests", "inventory-tests", "diagnose-smoke" };
    const char *cases[] = { "partial", "quiet", "multiple", "remaining", "inventory", "force", "unsafe-scan", "media-ambiguity", "redirect" };
    const char *helps[] = { "eject E: --help", "diagnose --label=--help --help", "list --help --bad" };
    char exe[MAX_PATH];
    DWORD code;
    unsigned int i;
    for (i = 0; i < sizeof(programs)/sizeof(programs[0]); i++) {
        sprintf(exe, "tests\\%s.exe", programs[i]);
        CHECK(run(exe, "") == 0);
    }
    CHECK(run(cli, "--version") == 0); CHECK(strncmp(out, "usb-eject 0.2.2", strlen("usb-eject 0.2.2")) == 0 && !err[0]);
    CHECK(run(cli, "help") == 0); CHECK(strstr(out, "--no-prompt") && strstr(out, "Exit codes:") && !err[0]);
    CHECK(run(cli, "eject --help") == 0); CHECK(strstr(out, "Forced termination") && !err[0]);
    CHECK(run(cli, "diagnose --letter 12") == 1); CHECK(!out[0] && strstr(err, "--letter expects A through Z"));
    code = run(cli, "list --format tsv");
    CHECK((code == 0 && !err[0]) || (code == 9 && strstr(err, "Discovery incomplete")));
    CHECK(strstr(out, "mount_point\tlabel\tvendor\tproduct") == out);
    CHECK(run(cli, "--internal-wait-pid 1 instance volume eject --mount E:") == 1);
    CHECK(strstr(err, "Invalid internal continuation command"));
    CHECK(run(cli, "--internal-handle-worker") == 1);
    for (i = 0; i < sizeof(helps)/sizeof(helps[0]); i++) {
        CHECK(run(cli, helps[i]) == 0); CHECK(!err[0] && strstr(out, "Usage:"));
    }
    CHECK(run(cli, "diagnose --label --no-prompt") == 1);
    CHECK(!out[0] && strstr(err, "at '--label'") && strstr(err, "requires a value"));
    CHECK(run(cli, "eject E: --typo") == 1); CHECK(!out[0] && strstr(err, "at '--typo'"));
    CHECK(run(cli, "eject E: --result-file unused.txt") == 1); CHECK(strstr(err, "only with eject --this"));
    for (i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        CHECK(run(flow, cases[i]) == 0); CHECK(!out[0]);
    }
    CHECK(run(flow, "target") == 0);
    CHECK(strstr(out, "Read-only inspection") && strstr(out, "Photos") && strstr(out, "Archive") && !strstr(out, "will be removed"));
    CHECK(run(flow, "card-target") == 0); CHECK(strstr(out, "keep the reader") && !strstr(out, "Archive"));
    CHECK(run(flow, "grouped") == 0);
    CHECK(occurrences(out, "PID 123") == 1 && !strstr(out, "    native ") && !strstr(out, "0x0000") && strstr(out, "4 resource findings"));
    CHECK(run(flow, "verbose") == 0); CHECK(strstr(out, "native") && occurrences(out, "handle   0x") == 4);
    CHECK(run(flow, "diagnostic-tsv") == 0); CHECK(occurrences(out, "\n") == 5 && !err[0]);
    CHECK(run(flow, "recovery") == 0);
    CHECK(!err[0] && strstr(out, "O''Brien & Photos") && strstr(out, "--card --quiet --no-prompt --verbose") && strstr(out, "--kill-blocker 123 --yes"));
    printf("Test suite: %d failures\n", failures);
    return failures != 0;
}
