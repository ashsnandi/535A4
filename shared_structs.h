#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// Maybe a packet header struct as well?
#define m_file_name_max_len 128

#define META_TYPE 0
#define DATA_TYPE 1

struct MetadataPacket {
    int type; // 0 for meta
    uint32_t file_size;
    uint32_t file_checksum;
    uint32_t total_chunks;
    int file_id;
    char filename[m_file_name_max_len];
};

struct DataPacket {
    int type; // 1 for data
    int file_id;
    int seq_num;
    uint32_t chunk_checksum;
    // flexible array has to be last in struct. Just a thing in C
    char data[]; // flexible
};

struct chunked_file {
    char *filename;
    int file_id;
    char **chunks; //chunk pointers array
    int *chunk_sizes; //chunk sizes
    int total_chunks;
    uint32_t file_size;
    uint32_t *chunk_checksums;
    uint32_t file_checksum; //this file's checksum
    bool *received_chunks; // track which chunks received for this file
};
