#define UNICODE
#define _UNICODE

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "cli.h"
#include "text.h"

static void set_error(ParseError *error, int index, const wchar_t *message) {
    if (error != NULL) {
        error->argument_index = index;
        error->message = message;
    }
}

static wchar_t *duplicate_text(const wchar_t *value) {
    size_t length;
    wchar_t *copy;

    length = wcslen(value);
    if (length > ((size_t)-1) / sizeof(wchar_t) - 1) {
        return NULL;
    }
    copy = (wchar_t *)malloc((length + 1) * sizeof(wchar_t));
    if (copy != NULL) {
        memcpy(copy, value, (length + 1) * sizeof(wchar_t));
    }
    return copy;
}

void command_init(Command *command) {
    memset(command, 0, sizeof(*command));
}

void command_dispose(Command *command) {
    if (command == NULL) {
        return;
    }
    free(command->selector.value);
    free(command->authorized_pids);
    command_init(command);
}

static int set_selector(
    Command *command,
    SelectorKind kind,
    const wchar_t *value,
    int argument_index,
    ParseError *error)
{
    if (command->selector.kind != SELECTOR_NONE) {
        set_error(error, argument_index, L"only one target selector may be supplied");
        return 0;
    }
    if (value != NULL && value[0] == L'\0') {
        set_error(error, argument_index, L"target value may not be empty");
        return 0;
    }
    command->selector.kind = kind;
    if (value != NULL) {
        command->selector.value = duplicate_text(value);
        if (command->selector.value == NULL) {
            set_error(error, argument_index, L"out of memory");
            return 0;
        }
    }
    return 1;
}

static int parse_pid(const wchar_t *value, DWORD *pid) {
    unsigned long long result;
    size_t index;

    if (value == NULL || value[0] == L'\0') {
        return 0;
    }
    result = 0;
    for (index = 0; value[index] != L'\0'; index++) {
        if (value[index] < L'0' || value[index] > L'9') {
            return 0;
        }
        result = result * 10 + (unsigned)(value[index] - L'0');
        if (result > 0xffffffffULL) {
            return 0;
        }
    }
    if (result == 0) {
        return 0;
    }
    *pid = (DWORD)result;
    return 1;
}

static int valid_drive_letter(const wchar_t *value) {
    size_t length;
    if (value == NULL) return 0;
    length = wcslen(value);
    if (length != 1 && length != 2) return 0;
    if (!((value[0] >= L'A' && value[0] <= L'Z') ||
          (value[0] >= L'a' && value[0] <= L'z'))) return 0;
    return length == 1 || value[1] == L':';
}

static int help_alias(const wchar_t *value) {
    return text_iequals(value, L"help") || text_iequals(value, L"--help") ||
        text_iequals(value, L"-h") || text_iequals(value, L"/?");
}

static int add_pid(Command *command, DWORD pid) {
    DWORD *new_values;
    size_t new_capacity;
    size_t index;

    for (index = 0; index < command->authorized_pid_count; index++) {
        if (command->authorized_pids[index] == pid) {
            return 1;
        }
    }
    if (command->authorized_pid_count == command->authorized_pid_capacity) {
        new_capacity = command->authorized_pid_capacity == 0
            ? 4
            : command->authorized_pid_capacity * 2;
        if (new_capacity < command->authorized_pid_capacity ||
            new_capacity > ((size_t)-1) / sizeof(DWORD)) {
            return 0;
        }
        new_values = (DWORD *)realloc(
            command->authorized_pids,
            new_capacity * sizeof(DWORD));
        if (new_values == NULL) {
            return 0;
        }
        command->authorized_pids = new_values;
        command->authorized_pid_capacity = new_capacity;
    }
    command->authorized_pids[command->authorized_pid_count++] = pid;
    return 1;
}

static int option_value(
    int argc,
    wchar_t **argv,
    int *index,
    const wchar_t **value,
    ParseError *error)
{
    if (*index + 1 >= argc) {
        set_error(error, *index, L"option requires a value");
        return 0;
    }
    (*index)++;
    *value = argv[*index];
    return 1;
}

int cli_parse(int argc, wchar_t **argv, Command *command, ParseError *error) {
    int index;
    const wchar_t *argument;
    const wchar_t *value;
    DWORD pid;

    if (error != NULL) {
        error->message = NULL;
        error->argument_index = -1;
    }
    if (command == NULL || argc < 1 || argv == NULL) {
        set_error(error, -1, L"invalid parser input");
        return 0;
    }
    if (argc == 1) {
        command->kind = COMMAND_HELP;
        return 1;
    }

    argument = argv[1];
    if (help_alias(argument)) {
        command->kind = COMMAND_HELP;
    } else if (text_iequals(argument, L"version") ||
               text_iequals(argument, L"--version")) {
        command->kind = COMMAND_VERSION;
    } else if (text_iequals(argument, L"list")) {
        command->kind = COMMAND_LIST;
    } else if (text_iequals(argument, L"diagnose")) {
        command->kind = COMMAND_DIAGNOSE;
    } else if (text_iequals(argument, L"eject")) {
        command->kind = COMMAND_EJECT;
    } else {
        set_error(error, 1, L"unknown command");
        return 0;
    }


    if (command->kind == COMMAND_HELP && argc == 3) {
        if (text_iequals(argv[2], L"list")) command->help_topic = COMMAND_LIST;
        else if (text_iequals(argv[2], L"diagnose")) command->help_topic = COMMAND_DIAGNOSE;
        else if (text_iequals(argv[2], L"eject")) command->help_topic = COMMAND_EJECT;
        else {
            set_error(error, 2, L"help topic must be list, diagnose, or eject");
            return 0;
        }
        return 1;
    }
    if ((command->kind == COMMAND_LIST || command->kind == COMMAND_DIAGNOSE ||
         command->kind == COMMAND_EJECT) && argc == 3 && help_alias(argv[2])) {
        command->help_topic = command->kind;
        command->kind = COMMAND_HELP;
        return 1;
    }

    for (index = 2; index < argc; index++) {
        argument = argv[index];
        if (text_iequals(argument, L"--format")) {
            if (!option_value(argc, argv, &index, &value, error)) return 0;
            if (text_iequals(value, L"text")) command->format = OUTPUT_TEXT;
            else if (text_iequals(value, L"tsv")) command->format = OUTPUT_TSV;
            else {
                set_error(error, index, L"format must be text or tsv");
                return 0;
            }
        } else if (text_iequals(argument, L"--letter") ||
                   text_iequals(argument, L"--mount") ||
                   text_iequals(argument, L"--label") ||
                   text_iequals(argument, L"--name")) {
            SelectorKind kind;
            if (!option_value(argc, argv, &index, &value, error)) return 0;
            if (text_iequals(argument, L"--letter")) kind = SELECTOR_LETTER;
            else if (text_iequals(argument, L"--mount")) kind = SELECTOR_MOUNT;
            else if (text_iequals(argument, L"--label")) kind = SELECTOR_LABEL;
            else kind = SELECTOR_NAME;
            if (kind == SELECTOR_LETTER && !valid_drive_letter(value)) {
                set_error(error, index,
                    L"--letter expects A through Z, optionally followed by ':'");
                return 0;
            }
            if ((kind == SELECTOR_LABEL || kind == SELECTOR_NAME) &&
                !text_pattern_valid(value)) {
                set_error(error, index, L"pattern permits '*' only at its beginning or end");
                return 0;
            }
            if (!set_selector(command, kind, value, index, error)) return 0;
        } else if (text_iequals(argument, L"--this")) {
            if (!set_selector(command, SELECTOR_THIS, NULL, index, error)) return 0;
        } else if (text_iequals(argument, L"--card")) {
            command->card_mode = 1;
        } else if (text_iequals(argument, L"--quiet")) {
            command->quiet = 1;
        } else if (text_iequals(argument, L"--no-prompt")) {
            command->no_prompt = 1;
        } else if (text_iequals(argument, L"--yes")) {
            command->assume_yes = 1;
        } else if (text_iequals(argument, L"--kill-blocker")) {
            if (!option_value(argc, argv, &index, &value, error)) return 0;
            if (!parse_pid(value, &pid)) {
                set_error(error, index, L"blocker PID must be a positive decimal integer");
                return 0;
            }
            if (!add_pid(command, pid)) {
                set_error(error, index, L"out of memory");
                return 0;
            }
        } else if (argument[0] == L'-') {
            set_error(error, index, L"unknown option");
            return 0;
        } else {
            if (!set_selector(command, SELECTOR_IMPLICIT, argument, index, error)) return 0;
        }
    }

    if ((command->kind == COMMAND_EJECT || command->kind == COMMAND_DIAGNOSE) &&
        command->selector.kind == SELECTOR_NONE) {
        set_error(error, argc, L"command requires a target");
        return 0;
    }
    if (command->kind != COMMAND_EJECT &&
        (command->card_mode || command->quiet || command->no_prompt ||
         command->assume_yes || command->authorized_pid_count != 0)) {
        set_error(error, 2, L"ejection option used with a non-eject command");
        return 0;
    }
    if (command->assume_yes && command->authorized_pid_count == 0) {
        set_error(error, 2, L"--yes requires at least one explicit --kill-blocker PID");
        return 0;
    }
    if ((command->kind == COMMAND_HELP || command->kind == COMMAND_VERSION) && argc != 2) {
        set_error(error, 2, L"command takes no options");
        return 0;
    }
    if (command->kind == COMMAND_LIST && command->selector.kind != SELECTOR_NONE) {
        set_error(error, 2, L"list does not accept a target");
        return 0;
    }
    return 1;
}
