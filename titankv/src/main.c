#include "eventloop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    server_config_t cfg = {
        .port = 6379,
        .data_dir = "./data",
        .max_memory_bytes = 512ULL * 1024 * 1024,
        .fsync_policy = FSYNC_EVERYSEC,
        .wal_compact_threshold = 64ULL * 1024 * 1024
    };

    int opt;
    while ((opt = getopt(argc, argv, "p:d:m:f:")) != -1) {
        switch (opt) {
            case 'p':
                cfg.port = (uint16_t)atoi(optarg);
                break;
            case 'd':
                cfg.data_dir = optarg;
                break;
            case 'm':
                cfg.max_memory_bytes = (size_t)atoll(optarg) * 1024ULL * 1024ULL;
                break;
            case 'f':
                if (strcmp(optarg, "always") == 0) cfg.fsync_policy = FSYNC_ALWAYS;
                else if (strcmp(optarg, "everysec") == 0) cfg.fsync_policy = FSYNC_EVERYSEC;
                else if (strcmp(optarg, "no") == 0) cfg.fsync_policy = FSYNC_NO;
                else {
                    fprintf(stderr, "Invalid fsync policy: %s\n", optarg);
                    return 1;
                }
                break;
            default:
                fprintf(stderr, "Usage: %s [-p port] [-d data_dir] [-m max_memory_mb] [-f always|everysec|no]\n", argv[0]);
                return 1;
        }
    }

    return run_server(&cfg);
}