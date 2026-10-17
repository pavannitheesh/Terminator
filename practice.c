
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>

int main(void)
{
    int pipefd[2];

    if (pipe(pipefd) == -1){
        perror("pipe");
        return 1;
    }

    pid_t first = fork();

    if (first == -1){
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return 1;
    }

    if (first == 0){
        close(pipefd[0]);

        if (dup2(pipefd[1], STDOUT_FILENO) == -1){
            perror("dup2");
            _exit(1);
        }

        close(pipefd[1]);

        char *args[] = {"ls", NULL};
        execvp(args[0], args);

        perror("execvp ls");
        _exit(127);
    }

    pid_t second = fork();

    if (second == -1){
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        waitpid(first, NULL, 0);
        return 1;
    }

    if (second == 0){
        close(pipefd[1]);

        if (dup2(pipefd[0], STDIN_FILENO) == -1){
            perror("dup2");
            _exit(1);
        }

        close(pipefd[0]);

        char *args[] = {"wc", "-l", NULL};
        execvp(args[0], args);

        perror("execvp wc");
        _exit(127);
    }

    close(pipefd[0]);
    close(pipefd[1]);

    waitpid(first, NULL, 0);
    waitpid(second, NULL, 0);

    return 0;
}