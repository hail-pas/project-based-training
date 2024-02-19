#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define SH_READLINE_BUFSIZE 1024;
#define SH_READARGS_BUFSIZE 1024;
#define SH_TOKEN_DELIMETER " \t\r\n\a"

/**
 * @brief 获取行输入
 *
 * @return char*
 */
char *sh_read_line() {
    int bufsize = SH_READLINE_BUFSIZE;
    int position = 0;
    char *buffer = malloc(sizeof(char) * bufsize);
    int c;

    if (!buffer) {
        fprintf(stderr, "sh_split_line: allocation error\n");
        exit(EXIT_FAILURE);
    }

    while (1) {
        c = getchar();

        if (c == EOF || c == '\n') {
            buffer[position] = '\0';
            return buffer;
        } else {
            buffer[position] = c;
        }
        position++;

        if (position >= bufsize || position >= 10 * 1024) {
            bufsize += SH_READLINE_BUFSIZE;
            buffer = realloc(buffer, bufsize);
            if (!buffer) {
                fprintf(stderr, "sh read line: allocation error\n");
                exit(EXIT_FAILURE);
            }
        }
    }
}

/**
 * @brief 行分割
 * @param line 一行
 * @return
 */
char **sh_split_line(char *line) {
    int bufsize = SH_READARGS_BUFSIZE;
    char **args = malloc(sizeof(char *) * bufsize);
    char *token;
    int position = 0;

    if (!args) {
        fprintf(stderr, "sh read args: allocation error\n");
        exit(EXIT_FAILURE);
    }

    token = strtok(line, SH_TOKEN_DELIMETER);

    while (token != NULL) {
        args[position] = token;
        position++;

        if (position >= bufsize) {
            bufsize += SH_READARGS_BUFSIZE;
            args = realloc(args, bufsize * sizeof(char *));
        }
        if (!args) {
            fprintf(stderr, "sh read args: allocation error\n");
            exit(EXIT_FAILURE);
        }
        token = strtok(NULL, SH_TOKEN_DELIMETER);
    }
    args[position] = NULL;
    return args;
}

int sh_launch(char **args) {
    pid_t pid, wpid;
    int status;

    // 复制当前进程, 返回值在不同进程有不同的值,
    // 父进程获取到的是 大于0的子进程的进程id 或 小于0的fork错误情况,
    // 生成的子进程获取到的是0
    pid = fork();

    if (pid == 0) {
        // child process
        if (execvp(args[0], args) == -1) {
            perror("sh exec error");
        }
        exit(EXIT_FAILURE);
    } else if (pid < 0) {
        perror("sh exec fork error");
    } else {
        do {
            wpid = waitpid(pid, &status, WUNTRACED);
        } while (!WIFEXITED(status) && !WIFSIGNALED(status));
    }

    return 1;
}

char *builtin_str[] = {"cd", "help", "exit"};

int sh_command_cd(char **args);
int sh_command_help(char **args);
int sh_command_exit(char **args);

int (*builtin_func[])(char **) = {&sh_command_cd, &sh_command_help,
                                  &sh_command_exit};

int sh_num_builtins() { return sizeof(builtin_str) / sizeof(char *); }

int sh_command_cd(char **args) {
    if (args[1] == NULL) {
        fprintf(stderr, "sh: expected arguments to \"cd\"\n");
    } else {
        if (chdir(args[1]) != 0) {
            perror("sh: cd");
        }
    }

    return 1;
}

int sh_command_help(char **args) {
    int i;
    printf("Phoenix's SH\n");
    printf("Type program names and arguments, and hit enter.\n");
    printf("The following are built in:\n");

    for (i = 0; i < sh_num_builtins(); i++) {
        printf("    %s", builtin_str[i]);
    }

    printf("Use the man command for information on other programs.\n");
    return 1;
}

int sh_command_exit(char **args) { return 0; }

int sh_execute(char **args) {
    int i;
    if (args[0] == NULL) {
        return 1;
    }

    for (i = 0; i < sh_num_builtins(); i++) {
        if (strcmp(args[0], builtin_str[i]) == 0) {
            return (*builtin_func[i])(args);
        }
    }

    return sh_launch(args);
}

void sh_loop() {
    char *line;
    char **args;
    int status;

    do {
        printf("> ");
        line = sh_read_line();
        args = sh_split_line(line);
        status = sh_execute(args);
        free(line);
        free(args);
    } while (status);
}

int main(int argc, char const *argv[]) {
    sh_loop();
    return 0;
}
