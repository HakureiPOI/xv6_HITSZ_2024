#include "kernel/types.h"
#include "user.h"

int main(int argc, char* argv[]) {
    int c2f[2];
    int f2c[2];
    pipe(c2f);
    pipe(f2c);

    if (fork() == 0) {
        /* 子进程 */
        char fpid[10];
        char cpid[10];

        itoa(getpid(), cpid);

        // 读取父进程的 PID
        close(f2c[1]);
        read(f2c[0], fpid, 10);
        close(f2c[0]);

        // printf("fpid = %s\n", fpid);
        printf("%s: received ping from pid %s\n", cpid, fpid);

        // 发送子进程的 PID
        close(c2f[0]);      
        write(c2f[1], cpid, 10);
        close(c2f[1]);
        
    } else {
        /* 父进程 */
        char fpid[10];
        char cpid[10];

        itoa(getpid(), fpid);

        // 发送父进程的 PID
        close(f2c[0]);      
        write(f2c[1], fpid, 10);
        close(f2c[1]);

        // 读取子进程的 PID
        close(c2f[1]);
        read(c2f[0], cpid, 10);
        close(c2f[0]);

        // printf("cpid = %s\n", cpid);
        printf("%s: received pong from pid %s\n", fpid, cpid);
    }

    exit(0);  // 正常退出
}
