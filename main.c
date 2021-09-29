#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <limits.h>
#include <ctype.h>
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

// Globals for sendMail() and checkMail() functions
// Used to store messages when terminal is blocked for input
// or a foreground process is currently running.
// [This was a lot more useful when I was relying
// on SIGCHLD events to reap children.]
// -----
char *mailman[20];             // Holds up to 20 messages
int sizeMailman = 20;

int slot = 0;                  // Last index filled with message

int newMail = 0;               // Counter for messages arrived
                               // since last checked
// =====

// Globals Re: storing exit status, last foreground
// exit status, and last terminating signal
// (for `status` command)
// -----
int exit_status = 0;           // exit status for entire program

int last_signal = -1;          // -1 means last foreground termination wasn't via signal
int last_exit_status = -1;     // -1 means last foreground process didn't exit normally

int hasRunForegroundProc = 0;  // Exists because program will print global exit status
                               // if no foreground processes have run yet
// =====

// Globals Re: keeping track of running child processes
// -----
int childProcesses[50] = {0};  // Array for running child processes
int childProcessCount = 0;
// =====

// Globals Re: foreground-only mode
// -----
int foregroundOnly = 0;                    // Used to flag whether foreground-only mode is on

int isForegroundProcRunning = 0;           // For knowing whether to print Ctrl+Z message
                                           // *immediately*, or after foreground process ends
// =====

// Global sigaction structs
// -----
struct sigaction SIGINT_action = {0};      // Custom signal action struct for SIGINT. Global because
                                           // child process needs to be able to access it.

struct sigaction SIGTSTP_action = {0};     // Same deal, but for SIGTSTP (i.e. Ctrl+Z signal)

/*
struct sigaction SIGCHLD_action = {0};     // SIGCHLD sigaction (goes unused in final version)
*/
// =====

void expandVar(char* input, char* buffer, pid_t pid){
    // Adapted from The Paramagnetic Croissant's answer to
    // "Replace all occurrences of a substring in a string in C"
    // via Stack Overflow:
    // https://stackoverflow.com/questions/32413667/replace-all-occurrences-of-a-substring-in-a-string-in-c/32413923
    char pid_s[10]; // pid_string

    // Place to store macro-replaced text:

    char* new_buffer = &buffer[0];

    const char *tmp = input;
    sprintf(pid_s, "%d", pid);  // Convert PID to string

    // Find first index where `$$` occurs, if any:
    char *chunk = strstr(tmp, "$$");

    while(chunk != NULL){
        // Copy up to, but not including, `$$`
        memcpy(new_buffer, tmp, chunk-tmp);
        new_buffer += chunk - tmp;

        // Copy PID where `$$` used to be
        memcpy(new_buffer, pid_s, strlen(pid_s));
        new_buffer += strlen(pid_s);

        // Update pointer past `$$`
        tmp = chunk + 2;

        // Attempt to grab next `$$`, if any
        chunk = strstr(tmp, "$$");

        if(chunk == NULL){
            // If no `$$` are left, copy the rest
            // of the string to new_buffer
            strcpy(new_buffer, tmp);
        }
    }
    if(strlen(buffer) == 0){
        strcpy(new_buffer, input);
    }
}

int itoa(int num, char *str, size_t sizeStr) {
    // Run-o'-the-mill integer to string conversion

    // Function adapted from LSerni's `ltoa` in answer to
    // "Using write syscall to print integers" via Stack Overflow:
    // https://stackoverflow.com/questions/13209632/using-write-syscall-to-print-integers
    if(num < 10){
        // If it's 0-9, conversion
        // is straightforward:
        str[0] = num + '0';
        str[1] = '\0';
        return 0;
    }
    int y = 1;
    size_t sizeNumStr;

    // Calculate space necessary for new string
    for (sizeNumStr = 0; y < num; sizeNumStr++)
        y *= 10;

    // Reject if string to be filled is too small
    if (sizeStr < sizeNumStr + 1)
        return sizeNumStr + 1;

    str[sizeNumStr--] = 0x0;  // Null

    while(sizeNumStr) {
        // '0' + number == ASCII number
        str[sizeNumStr--] = '0' + (num % 10);
        num /= 10;
    }
    str[0] = '0' + num; // Convert first digit

    return 0;
}

void sendMail(char* mail){
    // Adds 'child process terminated'
    // notes to the global `mailman` array
    // to be delivered next time control is
    // taken away from the user.
    if(slot == sizeMailman){
        // Mailbox is full, empty mailbox
        for(int i=0;i<slot;i++){
            free(mailman[i]);
        }
        slot = 0;  // Back to the beginning
    }
    mailman[slot] = (char *)malloc(strlen(mail)+1);
    strcpy(mailman[slot++], mail);
    newMail += 1;  // you've got mail
}

void checkMail(){
    // Checks for new messages. If newMail > 0,
    // pulls new messages out of the global `mailman`
    // array and prints to console.
    if(newMail == 0) {
        return;
    } else {
        int tmp = slot;
        for(int i=0;i<newMail;i++){
            tmp--;
            printf("%s", mailman[tmp]);
            fflush(stdout);
        }
        newMail = 0;  // Reset the counter
    }
}

void handleSIGTSTP(int sig){
    // Handles SIGTSTP (Ctrl+Z event signal)
    char *message;
    int msgLength;
    if(foregroundOnly == 0){  // We are *not* in foreground-only mode yet
        message = "Entering foreground-only mode (& is now ignored)\n";
        msgLength = 49;
        foregroundOnly = 1;  // Set flag for foreground-only mode to ON
    } else {
        message = "Exiting foreground-only mode\n";
        msgLength = 29;
        foregroundOnly = 0;  // Set flag for foreground-only mode to OFF
    }
    if(isForegroundProcRunning){
        sendMail(message);  // i.e. wait until foreground process is finished
                            // to display message
    } else {
        // otherwise just be out with it:
        write(STDOUT_FILENO, message, msgLength);
        fflush(stdout);
    }
}

void lookForZombies(){
    // Loops through all known active background child
    // processes to check for those that have recently
    // ended. If dead child process is discovered,
    // creates string and gives it to sendMail to be
    // delivered once control is taken back from the
    // user.
    pid_t pid;
    for(int i = 0; i < (sizeof(childProcesses)/sizeof(int)); i++){
        int status;
        if(childProcesses[i] > 0){
            if(waitpid(childProcesses[i], &status, WNOHANG)){
                // This is the same as the string-concat from
                // handleSIGCHLD, but SIGCHLD didn't work out.
                // I'm aware that signal safety isn't really
                // important here, however, I already had this
                // code handy and it works so I'm using it.
                char* message1;
                char pid_s[10] = {0};
                char* message2;
                char* message3;
                char status_s[4];
                char fullMsg[51] = {0};
                pid = childProcesses[i];
                message1 = "Background process ";
                message2 = " ended with ";
                if(WIFEXITED(status)){
                    message3 = "status ";
                    status = WEXITSTATUS(status);
                } else if(WIFSIGNALED(status)){
                    message3 = "signal ";
                    status = WTERMSIG(status);
                }

                // sprintf isn't signal-safe,
                // so we've got to convert
                // the old fashioned way:
                itoa(pid, pid_s, 10);

                if(status == 1) {
                    exit_status = 1;
                }

                if(status > 0) {
                    itoa(status, status_s, 4);
                } else {
                    // Easy to convert '0'
                    status_s[0] = status + '0';
                    status_s[1] = '\0';
                }

                // strcat is indeed signal safe
                // https://man7.org/linux/man-pages/man7/signal-safety.7.html
                strcat(fullMsg, message1);
                strcat(fullMsg, pid_s);
                strcat(fullMsg, message2);
                strcat(fullMsg, message3);
                strcat(fullMsg, status_s);
                strcat(fullMsg, "\n");

                // Pass message to global array to be delivered
                // once control is away from the user
                sendMail(fullMsg);

                // Remove dead child process from childProcesses array:
                for(int i=0;i<(sizeof(childProcesses)/sizeof(int));i++){
                    if(childProcesses[i] == pid){
                        childProcesses[i] = 0;
                        childProcessCount--;
                    }
                }
            }
        }
    }
}

void handleSIGCHLD(int sig){ // UNUSED IN FINAL VERSION

    // References asveikau's answer to "Tracking the death of a child process"
    // via Stack Overflow: https://stackoverflow.com/questions/2377811/tracking-the-death-of-a-child-process
    pid_t pid;
    int status;

    while((pid = waitpid(-1, &status, WNOHANG)) > 0){
        char* message1 = "Background process ";
        char pid_s[10] = {0};
        char* message2 = " ended with ";
        char* message3;

        // Check for exit condition and choose our
        // word accordingly
        if(WIFEXITED(status)){
            message3 = "status ";
            status = WEXITSTATUS(status);
        } else if(WIFSIGNALED(status)){
            message3 = "signal ";
            status = WTERMSIG(status);
        }

        char status_s[4];
        char fullMsg[51] = {0};

        // sprintf isn't signal-safe,
        // so we've got to convert
        // the old fashioned way:
        itoa(pid, pid_s, 10);

        if(status == 1) {
            exit_status = 1;
        }

        if(status > 0) {
            itoa(status, status_s, 4);
        } else {
            // Easy to convert '0'
            status_s[0] = status + '0';
            status_s[1] = '\0';
        }

        // strcat is indeed signal safe
        // https://man7.org/linux/man-pages/man7/signal-safety.7.html
        strcat(fullMsg, message1);
        strcat(fullMsg, pid_s);
        strcat(fullMsg, message2);
        strcat(fullMsg, message3);
        strcat(fullMsg, status_s);
        strcat(fullMsg, "\n");

        // Pass message to global array to be delivered
        // once control is away from the user
        sendMail(fullMsg);

        // Remove dead child process from childProcesses array:
        for(int i=0;i<(sizeof(childProcesses)/sizeof(int));i++){
            if(childProcesses[i] == pid){
                childProcesses[i] = 0;
                childProcessCount--;
            }
        }
    }
}

void sigIntHandler(int sig) {
    // This being set as the handler,
    // for some reason, allows SIG_INT
    // to return to default behavior
}

void runProgram(char **argv, int isBackground, char **pipes) {
    // Basic control flow Re: fork() adapted from `execute` function in
    // `shell.c` program via Michigan Tech CS 4411 course website
    // http://www.csl.mtu.edu/cs4411.ck/www/NOTES/process/fork/shell.c

    pid_t pid;              // PID == process ID
    int status;             // Exit status or terminating signal

    int isInputPiped = 0;   // If `<` was entered by user
    int isOutputPiped = 0;  // If `>` was entered by user

    if((pid = fork()) < 0) {
        // Status of -1 means fork failed
        perror("Error! Couldn't fork child process");
        fflush(stderr);
        exit(1);
    } else if(pid == 0) {
        // I am a new process and this is
        // the first moment of my life

        // Ignore SIGTSTP in all child processes:
        SIGTSTP_action.sa_handler = SIG_IGN;
        sigaction(SIGTSTP, &SIGTSTP_action, NULL);

        // Check for & handle input redirection:
        if(strcmp(pipes[0], "") != 0){
            isInputPiped = 1;
            // Open input file as read-only
            int sourceFD = open(pipes[0], O_RDONLY); // read-only mode
            if(sourceFD == -1){
                // -1 is returned if opening source file has failed
                perror("Error! Could not open source file");
                fflush(stderr);
                exit(1);  // Exit child process, return 1 for failure
            } else {
                int dupSource = dup2(sourceFD, STDIN_FILENO);
                if(dupSource == -1){
                    // -1 == failed to duplicate FD
                    perror("Error! dup2() on source file unsuccessful");
                    fflush(stderr);
                    exit(1);
                }
                // Doing everything in my power to make the
                // pipe close after exec..() is called
                fcntl(sourceFD, F_SETFD, FD_CLOEXEC | F_DUPFD_CLOEXEC);
            }


        }
        if(strcmp(pipes[1], "") != 0){
            isOutputPiped = 1;
            // open targetFD in write-only mode, creating or truncating where necessary:
            int targetFD = open(pipes[1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if(targetFD == -1){
                // -1 is returned if opening target file has failed
                perror("Error! Could not open target file");
                fflush(stderr);
                exit(1);
            } else {
                int dupTarget = dup2(targetFD, STDOUT_FILENO);
                if(dupTarget == -1){
                    // dup2 failed
                    perror("Error! dup2() on target file unsuccessful");
                    fflush(stderr);
                    exit(1);
                }
                fcntl(targetFD, F_SETFD, FD_CLOEXEC | F_DUPFD_CLOEXEC);
            }

        }
        if(isBackground == 1) {
            if(!isInputPiped){
                // close stdin and redirect input to /dev/null
                fclose(stdin);
                int devNullIn = open("/dev/null", O_RDONLY);
                dup2(devNullIn, STDIN_FILENO);    // duplicate /dev/null to stdin

                // We no longer need the devNullIn handle
                // open now that it's been copied to stdin:
                close(devNullIn);
            }
            if(!isOutputPiped){
                // close stdout and redirect output to /dev/null
                fclose(stdout);
                int devNullOut = open("/dev/null", O_WRONLY);
                dup2(devNullOut, STDOUT_FILENO);  // duplicate /dev/null to stdout
                close(devNullOut);
            }
        } else {
            SIGINT_action.sa_handler = sigIntHandler;
            sigaction(SIGINT, &SIGINT_action, NULL);
        }
        if(execvp(*argv, argv) < 0) {
            perror("Error! Execution unsuccessful");
            fflush(stderr);
            // Set exit status to 1.
            exit(1);
        }
    } else {
        // I'm the parent process
        if(isBackground == 0) {
            // be a good parent and wait for my child to die
            isForegroundProcRunning = 1;
            while(waitpid(pid, &status, 0) != pid);
            isForegroundProcRunning = 0;  // If I'm here, then the process has finished

            hasRunForegroundProc = 1;     // <- relevant to `status` command (printStatus)

            if(WIFEXITED(status)){  // Process exited normally
                last_exit_status = WEXITSTATUS(status);  // decode exit status
                if(last_exit_status == 1){
                    exit_status = 1;
                }
                last_signal = -1;  // as last process exited and was not signaled
            }

            else if(WIFSIGNALED(status)){
                // Process was terminated by a signal
                last_signal = WTERMSIG(status); // Decode terminating signal using WTERMSIG
                                                // and set last_signal for printStatus purposes
                printf("Foreground process terminated with signal %d\n", last_signal);
                last_exit_status = -1;
            }
        } else {
            // Then we're the parent of a background process
            printf("Background pid is %d\n", pid);
            fflush(stdout);
            childProcesses[childProcessCount++] = pid;
        }
    }
}

void parseArguments(char *input, char **argv, char **pipes){
    // Adapted from `parse` function in shell.c program
    // via Michigan Tech CS 4411 course website
    // http://www.csl.mtu.edu/cs4411.ck/www/NOTES/process/fork/shell.c
    int isPipe = 0;
    while(*input != '\0') {
        while(*input == ' ' || *input == '\n') {
            *input++ = '\0'; // Null-terminating spaces between words
        }
        isPipe = 0;
        // Handling input/output piping:
        if(input[0] == '<') {
            char* inputSaveptr;
            strtok_r(input, " ", &inputSaveptr);  // '<' will be tokenized first, no need to save it
            // this is the actual path:
            pipes[0] = strtok_r(NULL, " ", &inputSaveptr);

            // update `input` to match our point in the string:
            input = inputSaveptr;
            isPipe = 1;  // Set a flag so that we don't save
                         // the piping commands as args
        }
        if(input[0] == '>'){
            char *outputSaveptr;
            strtok_r(input, " ", &outputSaveptr);  // '>', which we don't need
            pipes[1] = strtok_r(NULL, " ", &outputSaveptr);
            input = outputSaveptr;
            isPipe = 1;
        }
        if(isPipe == 0){
            // If it's not a pipe, it's an argument, so save it!
            *argv++ = input;
        }
        while (*input != '\0' && *input != ' ' && *input != '\n') {
            input++;        // And working our way through the rest of the word
        }
    }
    *argv = (char *) NULL;  // End with a null pointer
}

void changeDirectory(char *path){
    // References Mic / isnullxbh's answer to "How to get the current directory in a C program?"
    // https://stackoverflow.com/questions/298510/how-to-get-the-current-directory-in-a-c-program
    char cwd[PATH_MAX];  // 4096
    if (chdir(path) == 0) {
        // Changing directory was successful
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            // Confirming the change of directory:
            printf("Working directory changed to %s\n", cwd);
            fflush(stdout);
        } else {
            // Something strange has happened, can't get current dir
            perror("Error! Could not get current working directory");
            fflush(stderr);
        }
    } else {
        // Non-zero return from chdir() indicates failure
        perror("Error! Could not change working directory");
        fflush(stderr);
    }
}

void changeDirToUserPath(char *input){
    char *saveptr;
    // Grabbing `cd` first, hopefully
    char *token = strtok_r(input, " ", &saveptr);
    if(strcmp(token, "cd") == 0) {
        // And then this ought to be the path:
        token = strtok_r(NULL, "\0", &saveptr);
        changeDirectory(token);
    }
}

void changeDirToHOME(){
    // Get `HOME` environment variable:
    char *home = getenv("HOME");
    // Change directory same as we would for "cd /user/path"
    changeDirectory(home);
}

void printStatus(){
    if(hasRunForegroundProc == 0){  // No foreground processes have been run yet
        printf("Current exit status is %d\n", exit_status);
    }
    else if(last_signal != -1){  // Last foreground process was signaled to death
        printf("Last foreground process terminated abnormally with signal %d\n", last_signal);
    } else {
        printf("Last foreground process exited normally with status %d\n", last_exit_status);
    }
}

int getInput(){
    lookForZombies();     // <- check our childProcesses array for status changes
    checkMail();          // <- get/print any messages Re: terminating processes

    printf(": ");  // summon the real hero, our command-line prompt,
    fflush(stdout);       // and flush to force all output to stdout

    char *gl_input = NULL;    // <- gl_input meaning getline_input
    char *input = NULL;       // <- copy of gl_input for manipulating
    char buffer[4096] = {0};  // <- buffer for final $$-replaced string

    int choice = -1;
    int isBackground = 0;
    // References David C. Rankin's answer to "How to read input of unknown length using fgets"
    // via Stack Overflow:
    // https://stackoverflow.com/questions/27326839/how-to-read-input-of-unknown-length-using-fgets
    // ===================================================
    size_t n = 0;
    ssize_t nchr = 0;
    // wait for user input:
    nchr = getline(&gl_input, &n, stdin);
    if(nchr == -1){  // I have SA_RESTART flags set
                     // ...but just in case, you know?
        clearerr(stdin);
    } else {
        // copy getline string to new variable:
        input = strdup(gl_input);
        free(gl_input);

        if(input[0] == '#') {
            // Then the line is a comment, so ignore:
            return 1;
        }

        // Chop off that newline:
        input[--nchr] = 0;

        while(isspace(input[nchr-1])){
            // Remove any trailing spaces:
            input[--nchr] = 0;
        }
    // ===================================================
        if(nchr == 0) {
            // If, after all,
            // the line was blank:
            return 1;
        }
        if(input[nchr-1] == '&'){
            // User wants to run process in background
            if(foregroundOnly == 0){  // But only if they're in foreground-only mode
                isBackground = 1;     // (otherwise we will ignore their request)
            }
            input[--nchr] = 0;        // Remove `&` and preceding space
            input[--nchr] = 0;        // to avoid them being read as arguments
        }

        expandVar(input, buffer, getpid());   // Replace $$ with pid

        if(strcmp(buffer, "cd") == 0) {
            // Handle `cd` by itself
            // (change working directory to HOME env. var)
            changeDirToHOME();
        } else if(strncmp("cd ", buffer, 3) == 0) {
            // Handle `cd` along with a path
            // (change working directory to what user-specified)
            changeDirToUserPath(buffer);
        } else if(strcmp(buffer, "status") == 0){
            printStatus();
        } else if(strcmp(buffer, "exit") == 0) {
            // We won't exit here, because we still have
            // to clean up remaining processes
            choice = 0;
        } else {
            char *argv[512];  // array for command + arguments

            // `pipes` holds input redirection and
            // output redirection.
            // pipes[0] == input, pipes[1] == output
            char *pipes[2];
            pipes[0] = "";
            pipes[1] = "";

            // Parse input and fill argv, pipes
            parseArguments(buffer, argv, pipes);

            // And then move into running the program
            runProgram(argv, isBackground, pipes);
        }
        free(input);  // Release input
        return choice;
    }

}

int main(int argc, char *argv[]) {
    int mode = 1;
    // Signal overrides
    // ================
    // SIGINT_action declared as global
    // at top of program
    // SIGINT == Ctrl+Z "Keyboard interrupt" signal
    SIGINT_action.sa_handler = SIG_IGN;
    sigfillset(&SIGINT_action.sa_mask);   // Block all signals while handling SIGINT
    SIGINT_action.sa_flags = SA_RESTART;  // Restart any library functions upon interruption by signal
    sigaction(SIGINT, &SIGINT_action, NULL);  // Install custom handler

    // SIGCHLD == dying child process signal
    // I ended up taking this out because handling
    // SIGCHLD ended up causing execution to hang
    // every once in a while.
    /*
    SIGCHLD_action.sa_handler = handleSIGCHLD;
    sigfillset(&SIGCHLD_action.sa_mask);
    SIGCHLD_action.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &SIGCHLD_action, NULL);
     */

    // SIGTSTP == Ctrl+Z "Stop" signal
    SIGTSTP_action.sa_handler = handleSIGTSTP;
    SIGTSTP_action.sa_flags = SA_RESTART;
    sigaction(SIGTSTP, &SIGTSTP_action, NULL);
    // ================

    // Main execution loop
    while(mode != 0) {      // mode of 0 == quit
        mode = getInput();
    }

    // Kill any remaining child processes before exiting:
    for(int i=0; i < (sizeof(childProcesses)/sizeof(int)); i++){
        if(childProcesses[i] > 0) {
            kill(childProcesses[i], SIGKILL); // send kill signal
            childProcesses[i] = 0;
            childProcessCount--;
        }
    }
    return exit_status;
}
