/*
 *  Simple shell functionality
 *   - execute compiled files
 *   - pipe 2 commands together
 *   - input & output redirection
 *   - execute built in commands: echo, cd, pwd, exit, fg, jobs
 *   - execute process in background
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>


/*========================
== constant definitions ==
========================*/

#define MAX_ARGS 32
#define CWD_NAME_BUFFER_SIZE 1024
#define LIST_ID_FIRST_VAL 0

#define FLAG_BG_PROCESS '&'
#define FLAG_RD_OUTPUT '>'
#define FLAG_PIPE '|'

#define SHELL_PROMPT "\n>> "
#define SHELL_JOBS_FORMAT "\n[%d] %s\t%d"
#define SHELL_JOBS_HEADER "\nCurrent running jobs:\n[#] cmd\t\tpid\n-----------------------"

#define ERROR_INPUT_CMD "Command not found"
#define ERROR_FORK "process creation failed: "
#define ERROR_SIGNAL_BIND "Could not bind signal: "
#define ERROR_OUT_OF_MEM "Could not allocate sufficient memory for process"
#define ERROR_NO_SUCH_PROC "Invalid process number"


/*======================
== struct definitions ==
======================*/

typedef struct process {    // active process node
    struct process* next;   // pointer to next node
    char* name;             // name of the binary file (command)
    pid_t process_id;       // process id
    int list_id;            // list id (rewritten when jobs is called)
} process;

process* process_list;

pid_t shell_id;


/*=======================
== function prototypes ==
=======================*/

int   execute                   (char* command, char* params[], int bg_flag);
int   execute_pipe              (char* args[], int bg_flag);
int   execute_built_ins         (char* command, char* params[]);

void  execute_echo              (char* words[]);
int   execute_cd                (char* path);
void  execute_pwd               (void);
void  execute_exit              (void);
int   execute_fg                (int list_id);
void  execute_jobs              (void);

int   process_add               (char* name, pid_t pid, int bg_flag);
int   process_remove_from_list  (pid_t pid);
pid_t process_bring_to_fg       (int id);
void  process_print_list        (void);
void  process_terminate_fg      (pid_t process_id);

int   getcmd                    (char* prompt, char* args[], int* bg_flag,
                                 int* rd_flag, int* p_flag);
void  print_error               (char* error_message);


/*===================
== signal handlers ==
===================*/

/**
 * Signal handler for SIGINT signal. Exits the shell.
 * @param sig
 */
static void signal_handler(int sig) {

    // attempts to kill the foreground process
    if (sig == SIGINT) {
        process_terminate_fg(getpid());
        return;
    }

    // attempts to remove all terminated processes from the process list (prevent zombies)
    if (sig == SIGCHLD) {
        pid_t process_id;
        while ((process_id = wait3(NULL, WNOHANG, NULL)) > 0) {
            process_remove_from_list(process_id);
        }
        return;
    }
}


/*=============
== functions ==
=============*/

/**
 * Main function, listens for commands and executes them appropriately.
 * The following signals are rewired:
 *  - SIGTSTP: ignored
 *  - SIGINT:  kills foreground process if any is found
 *  - SIGCHLD: removes terminated processes from the process list
 *
 * @return
 */
int main(void) {

    // signal handling setup
    if (signal(SIGTSTP, SIG_IGN) == SIG_ERR ||
        signal(SIGINT, signal_handler) == SIG_ERR ||
        signal(SIGCHLD, signal_handler) == SIG_ERR
        ) {
        perror(ERROR_SIGNAL_BIND);
        return -1;
    }

    // saves the shell process ID
    shell_id = getpid();

    // argument array setup
    char* args[MAX_ARGS];

    // flags, counters, and file descriptors set up
    int arg_count,
        bg_flag, rd_flag, p_flag,
        std_out, rd_out;

    // start the input - execute loop
    for (;;) {

        // reset optional flags
        bg_flag = 0;
        rd_flag = 0;
        p_flag = 0;

        // get input from user
        arg_count = getcmd(SHELL_PROMPT, args,
                           &bg_flag, &rd_flag, &p_flag);

        // check for invalid input
        if (arg_count == -1) {
            print_error(ERROR_INPUT_CMD);
            continue;
        }

        // skip to next iteration if no input was given
        if (!arg_count) {
            continue;
        }

        // makes array NULL terminated
        args[arg_count] = NULL;

        // open output redirection
        if (rd_flag) {

            // save standard out for reopening
            std_out = dup(fileno(stdout));

            // redirect output
            close(fileno(stdout));
            rd_out = open(args[arg_count - 1],
                          O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);

            // remove filename from arguments
            args[arg_count - 1] = NULL;
        }

        // check if the command is a built in
        if (execute_built_ins(*args, args)) {

            // if no built-in command was executed, creates process(es)

            // if piping is present, create 2 processes
            if (p_flag) {
                execute_pipe(args, bg_flag);
            }

            // otherwise execute normally, create 1 process
            else {
                execute(*args, args, bg_flag);
            }
        }

        // close output redirection
        if (rd_flag) {

            // redirect output
            close(rd_out);
            dup(std_out);

            // close duplicate descriptor
            close(std_out);

            // flush output
            fflush(stdout);
        }
    }
}


/*=====================
== execute functions ==
=====================*/

/**
 * Executes a command in a new process.
 * Can be output redirected.
 *
 * @param command   user command
 * @param params    argument array; params[0] == command, params[len] == NULL
 * @param bg_flag   is the process supposed to run in the background
 * @return          pid of the child process
 */
int execute(char* command, char* params[], int bg_flag) {

    // call an external command
    pid_t process_id = fork();

    // child process
    if (!process_id) {

        // dummy sleep command for testing
        if (!strcmp(command, "wait") || !strcmp(command, "sleep")) {
            sleep(strtol(params[1], NULL, 10));
            return 0;
        }

        // execute binary file
        if ((execvp(command, params)) == -1) {
            if (errno == 2) {
                perror(ERROR_INPUT_CMD);
            }
            else {
                perror(NULL);
            }
        }
    }

    // parent process; suspends until child termination (if bg_flag != 0)
    else if (process_id > 0) {

        // if process is in foreground,
        // suspend parent process until child finishes execution
        if (!bg_flag) {
            waitpid(process_id, NULL, 0);
        }

        // otherwise add a process to the active process table,
        // terminates execution on error (malloc fault)
        else {
            if (process_add(command, process_id, bg_flag)) {
                kill(process_id, SIGKILL);
                return -1;
            }
        }

        return 0;
    }

    // forking error
    else {
        perror(ERROR_FORK);
        exit(1);
    }

    return 0;
}

/**
 * Sequentially executes two commands in two new processes.
 * Can be output redirected; process 2 waits for process 1 completion.
 *
 * @param args      two commands with their arguments, separated by a pipe "|"
 * @param bg_flag   0: process runs in foreground, 1: process runs in background
 * @return          0: successful execution, -1: error
 */
int execute_pipe(char* args[], int bg_flag) {

    char** pipe_args = NULL;
    int pipe_list[2];

    // set up pipe
    if (pipe(pipe_list)) {
        perror(NULL);
        return -1;
    }

    // split args array
    pipe_args = args + 1;
    while (pipe_args) {
        if (!strcmp(*pipe_args, "|")) {
            *pipe_args++ = NULL;
            break;
        }
        pipe_args++;
    }

    // call an external command
    pid_t process_id1 = fork();

    // child process, layer 1
    if (!process_id1) {

        pid_t process_id2 = fork();

        // child process, layer 2 (runs first command)
        if (!process_id2) {

            // open pipe, write end
            close(pipe_list[0]);
            dup2(pipe_list[1], fileno(stdout));

            // execute binary file from args
            if ((execvp(*args, args)) == -1) {
                if (errno == 2) {
                    perror(ERROR_INPUT_CMD);
                }
                else {
                    perror(NULL);
                }
            }
        }

        // parent process, layer 2 (runs second command)
        else if (process_id2 > 0) {

            // open pipe, read end
            close(pipe_list[1]);
            dup2(pipe_list[0], fileno(stdin));

            // wait for first command to finish
            waitpid(process_id2, NULL, 0);

            // if it is a background process
            if (bg_flag) {

                // add a process to the active process table,
                // terminates execution on error (malloc fault)
                if (process_add(*pipe_args, process_id2, bg_flag)) {
                    kill(process_id2, SIGKILL);
                    return -1;
                }
            }

            // execute binary file from pipe_args
            if ((execvp(*pipe_args, pipe_args)) == -1) {
                if (errno == 2) {
                    perror(ERROR_INPUT_CMD);
                }
                else {
                    perror(NULL);
                }
            }
        }

        // error, layer 2
        else {
            perror(ERROR_FORK);
            exit(1);
        }

    }

    // parent process, layer 1
    else if (process_id1 > 0) {

        // clean up pipes
        close(pipe_list[0]);
        close(pipe_list[1]);

        // waits for layer 1 child to complete if process runs in foreground
        if (!bg_flag) {
            waitpid(process_id1, NULL, 0);
        }

        // otherwise add a process to the active process table,
        // terminates execution on error (malloc fault)
        else {
            if (process_add(*args, process_id1, bg_flag)) {
                kill(process_id1, SIGKILL);
                return -1;
            }
        }

        return 0;
    }

    // forking error, layer 1
    else {
        perror(ERROR_FORK);
        exit(1);
    }

    return 0;
}

/**
 * Executes a built-in command, if the command matches.
 *
 * @param list      head of list of active processes
 * @param command   possible built in command
 * @param params    parameters for the command
 * @return          0: built-in command was found and executed; 1: otherwise
 */
int execute_built_ins(char* command, char* params[]) {

    if (!strcmp(command, "echo")) {
        execute_echo(params + 1);
        return 0;
    }
    if (!strcmp(command, "cd")) {
        execute_cd(*(params + 1));
        return 0;
    }
    if (!strcmp(command, "pwd")) {
        execute_pwd();
        return 0;
    }
    if (!strcmp(command, "exit")) {
        execute_exit();
        return 0;
    }
    if (!strcmp(command, "fg")) {
        execute_fg((int) strtol(params[1], NULL, 10));
        return 0;
    }
    if (!strcmp(command, "jobs")) {
        execute_jobs();
        return 0;
    }

    return 1;
}

/**
 * Prints the arguments provided, separated by spaces.
 *
 * @param words     list of strings to be printed
 */
void execute_echo(char* words[]) {
    while (*words) {
        printf("%s ", *words++);
    }
}

/**
 * Changes directory to the specified path.
 *
 * @param path  relative or absolute target path
 * @return      0 on success, -1 on error
 */
int execute_cd(char* path) {
    if (chdir(path)) {
        perror(NULL);
        return -1;
    }
    return 0;
}

/**
 * Prints the current working directory.
 */
void execute_pwd(void) {
    char buffer[CWD_NAME_BUFFER_SIZE];
    if (getcwd(buffer, sizeof(buffer))) {
        printf("%s", buffer);
        return;
    }
    perror(NULL);
}

/**
 * Kills all processes, cleans the heap memory, and exits.
 */
void execute_exit(void) {

    // removes all process list elements from memory
    process* node = process_list;
    while (node) {
        process_remove_from_list(process_list->process_id);
        process_list = node;
        node = node->next;
    }

    // terminates all background processes
    kill(0, SIGKILL);
}

/**
 * Brings a process to foreground -- modifies bg_flag and suspends shell.
 * @param list_id
 * @return 0: success, -1: process not found
 */
int execute_fg(int list_id) {

    // attempt to bring process to foreground
    pid_t process_id = process_bring_to_fg(list_id);

    // ignore if no process was found
    if (process_id == -1) {
        return -1;
    }

    // suspend shell otherwise
    waitpid(process_id, NULL, 0);
    return 0;
}

/**
 * Prints all the background processes into the console.
 */
void execute_jobs(void) {
    printf("%s", SHELL_JOBS_HEADER);
    process_print_list();
}


/*==========================
== process list functions ==
==========================*/

/**
 * Adds a background process to the process list
 *
 * @param name  name of the command / process binary executable
 * @param pid   process id
 * @return      0: success, -1: malloc fault
 */
int process_add(char* name, pid_t pid, int bg_flag) {

    // create & initialise the new process node
    process* new = (process*) malloc(sizeof(process));
    if (!new) {
        print_error(ERROR_OUT_OF_MEM);
        return -1;
    }
    *new = (process) {
        .name = name,
        .process_id = pid,
        .list_id = -1
    };

    // prepend the process node
    new->next = process_list;
    process_list = new;

    return 0;
}

/**
 * Removes a process from the background process list by process ID.
 * @param pid   process ID of the process to be removed
 * @return      0: node was removed, -1: node not found
 */
int process_remove_from_list(pid_t pid) {
    process* node = process_list;
    process* prev = NULL;

    if (!node) {
        print_error(ERROR_NO_SUCH_PROC);
        return -1;
    }

    // attempt to find a node with pid
    while (node) {

        // if it is found
        if (node->process_id == pid) {

            // if it is the first job
            if (!prev) {
                process_list = node->next;
                free(node);
            }
            else {
                prev->next = node->next;
                free(node);
            }
            return 0;
        }

        // increment
        prev = node;
        node = node->next;
    }

    return 0;
}

/**
 * Attempts to find a process based on list id.
 * Calls process_remove_from_list().
 *
 * @param id    list ID of the process sought
 * @return      pid of process or -1 if no node matches
 */
pid_t process_bring_to_fg(int id) {
    process* node = process_list;

    if (!node) {
        print_error(ERROR_NO_SUCH_PROC);
        return -1;
    }

    // attempt to find a node with pid
    while (node) {
        if (node->list_id == id) {
            process_remove_from_list(node->process_id);
            return node->process_id;
        }
        node = node->next;
    }

    return -1;
}

/**
 * Prints all members of the process list to the console.
 * Rewrites the list_id to reflect the current display.
 */
void process_print_list(void) {
    process* node = process_list;
    int list_id = LIST_ID_FIRST_VAL;
    while (node) {
        printf(SHELL_JOBS_FORMAT, list_id, node->name, node->process_id);
        node->list_id = list_id++;
        node = node->next;
    }
}

/**
 * If a process running in the foreground is found, it is terminated.
 * The shell process is ignored.
 */
void process_terminate_fg(pid_t process_id) {
    if (shell_id == process_id) {
        return;
    }
    kill(process_id, SIGKILL);
}


/*=======================
== interface functions ==
=======================*/

/**
 * Loads parameters from the stdin into an array.
 *
 * @param prompt   prompt displayed to the user in the terminal
 * @param args     pointer to the array of parameters that will be filled
 * @param bg_flag  pointer to the boolean flag determining if process runs in background
 * @param rd_flag  pointer to the boolean flag determining if process has redirected output
 * @return         number of arguments loaded into args
 */
int getcmd(char* prompt, char* args[], int* bg_flag, int* rd_flag, int* p_flag) {

    // get user input, exit if none is provided
    char* line_buffer = NULL;       // buffer where the input line will be loaded
    size_t line_buffer_max_len = 0; // maximum buffer size

    fputs(prompt, stdout);
    if (getline(&line_buffer, &line_buffer_max_len, stdin) <= 0) {
        exit(-1);
    }

    // check for background and redirect flags
    char* pos;
    if (pos = strchr(line_buffer, FLAG_BG_PROCESS)) {
        *bg_flag = 1;
        *pos = ' ';
    }

    if (pos = strchr(line_buffer, FLAG_RD_OUTPUT)) {
        *rd_flag = 1;
        *pos = ' ';
    }

    if (strchr(line_buffer, FLAG_PIPE)) {
        *p_flag = 1;
    }

    // slice input string into args[]
    int arg_count = 0;
    char* token;
    while (token = strsep(&line_buffer, " \t\n")) {
        for (int i = 0; i < strlen(token); i++) {
            if (token[i] <= 32) {
                token[i] = '\0';
            }
        }
        if (strlen(token)) {
            args[arg_count++] = token;
        }
    }

    return arg_count;
}

/**
 * Prints an error message in the format "Error: <msg>" to stdout.
 * @param error_message
 */
void print_error(char* error_message) {
    printf("%s: %s", "Error", error_message);
}