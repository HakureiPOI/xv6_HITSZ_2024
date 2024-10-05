#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

void find(const char *path, const char *target) {
    int fd;
    struct stat st;
    struct dirent de;
    char buf[512], *p;

    // 打开路径
    if ((fd = open(path, 0)) < 0) {
        printf("find: cannot open %s\n", path);
        return;
    }

    // 获取文件/目录状态
    if (fstat(fd, &st) < 0) {
        printf("find: cannot stat %s\n", path);
        close(fd);
        return;
    }

    switch (st.type) {
        case T_FILE:
            if (strcmp(path + strlen(path) - strlen(target), target) == 0) {
                printf("%s\n", path);
            }
            break;

        case T_DIR:
            // 如果当前目录名与 target 相同，则输出
            if (strcmp(path + strlen(path) - strlen(target), target) == 0) {
                printf("%s\n", path);
            }

            if (strlen(path) + 1 + DIRSIZ + 1 > sizeof(buf)) {
                printf("find: path too long\n");
                break;
            }

            strcpy(buf, path);
            p = buf + strlen(buf);
            *p++ = '/';

            while (read(fd, &de, sizeof(de)) == sizeof(de)) {
                if (de.inum == 0)
                    continue;

                // 跳过 "." 和 ".." 目录
                if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
                    continue;

                // 构造完整路径
                memmove(p, de.name, DIRSIZ);
                p[DIRSIZ] = 0;

                if (stat(buf, &st) < 0) {
                    printf("find: cannot stat %s\n", buf);
                    continue;
                }

                // 递归查找
                find(buf, target);
            }
            break;
    }

    close(fd);
}


int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: find <path> <filename>\n");
        exit(0);
    }
    
    find(argv[1], argv[2]);
    exit(0);
}
