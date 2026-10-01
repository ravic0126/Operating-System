/*
 * workload.c - the job program that your scheduler runs.
 *
 * usage: workload <name> <cpu_ms> <io_every_ms> <io_dur_ms>
 *
 * Its behaviour is specified in the assignment handout. Do not modify this
 * file and do not submit it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>

#define MSG_FD       3
#define HB_UNITS     5
#define STOP_GAP_MS  20.0

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void sleep_ms(double ms)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000.0);
    ts.tv_nsec = (long)((ms - ts.tv_sec * 1000.0) * 1000000.0);
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: %s <name> <cpu_ms> <io_every_ms> <io_dur_ms>\n", argv[0]);
        return 1;
    }
    const char *name = argv[1];
    double cpu_ms    = atof(argv[2]);
    double io_every  = atof(argv[3]);
    double io_dur    = atof(argv[4]);

    /* Check fd 3 before opening anything else, or the open below would be
     * handed fd 3 itself and the messages would go to the wrong place. */
    if (fcntl(MSG_FD, F_GETFD) == -1) {
        fprintf(stderr, "workload: file descriptor 3 is not open. Your scheduler "
                        "must provide the write end of its pipe as fd 3.\n");
        return 1;
    }

    char path[128];
    mkdir("hb", 0755);
    snprintf(path, sizeof path, "hb/%s.hb", name);
    int hb = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    raise(SIGSTOP);

    double consumed = 0.0, since_io = 0.0;
    long units = 0;
    char buf[64];

    while (consumed < cpu_ms) {
        int64_t t0 = now_us();
        sleep_ms(1.0);
        double d = (now_us() - t0) / 1000.0;
        if (d > STOP_GAP_MS)
            d = 1.0;              /* stopped part way through: charge nominal */

        consumed += d;
        since_io += d;
        units++;

        if (hb >= 0 && units % HB_UNITS == 0) {
            int len = snprintf(buf, sizeof buf, "%lld\n",
                               (long long)(now_us() / 1000));
            ssize_t w = write(hb, buf, len);
            (void)w;
        }

        if (io_every > 0 && since_io >= io_every && consumed < cpu_ms) {
            dprintf(MSG_FD, "IO %s\n", name);
            sleep_ms(io_dur);
            dprintf(MSG_FD, "RDY %s\n", name);
            raise(SIGSTOP);
            since_io = 0.0;
        }
    }

    dprintf(MSG_FD, "DONE %s\n", name);
    if (hb >= 0) close(hb);
    return 0;
}
