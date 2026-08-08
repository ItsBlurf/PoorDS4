/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ps5/payload.h>

int
main(void)
{
    mkdir("/data/poords4", 0755);
    int fd = open(
        "/data/poords4/stop-game-pad-bridge",
        O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0)
        close(fd);
    payload_exit(fd >= 0 ? 0 : 1);
    return 0;
}
