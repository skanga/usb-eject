#ifndef USB_EJECT_CLI_H
#define USB_EJECT_CLI_H

#include <windows.h>
#include <stddef.h>

typedef enum {
    COMMAND_NONE = 0,
    COMMAND_HELP,
    COMMAND_VERSION,
    COMMAND_LIST,
    COMMAND_DIAGNOSE,
    COMMAND_EJECT
} CommandKind;

typedef enum {
    SELECTOR_NONE = 0,
    SELECTOR_IMPLICIT,
    SELECTOR_LETTER,
    SELECTOR_MOUNT,
    SELECTOR_LABEL,
    SELECTOR_NAME,
    SELECTOR_THIS
} SelectorKind;

typedef enum {
    OUTPUT_TEXT = 0,
    OUTPUT_TSV
} OutputFormat;

typedef struct {
    SelectorKind kind;
    wchar_t *value;
} TargetSelector;

typedef struct {
    CommandKind kind;
    CommandKind help_topic;
    TargetSelector selector;
    OutputFormat format;
    int card_mode;
    int quiet;
    int no_prompt;
    int assume_yes;
    DWORD *authorized_pids;
    size_t authorized_pid_count;
    size_t authorized_pid_capacity;
} Command;

typedef struct {
    const wchar_t *message;
    int argument_index;
} ParseError;

void command_init(Command *command);
void command_dispose(Command *command);
int cli_parse(int argc, wchar_t **argv, Command *command, ParseError *error);

#endif
