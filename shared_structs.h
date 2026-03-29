#include <stdint.h>

#define m_chunk_size 1024
#define m_filename_size 256

// Maybe a packet header struct as well?

typedef struct {
    uint32_t file_size;
    uint32_t file_checksum;
    uint32_t chunk_size;
    char filename[m_filename_size];
} MetadataPacket;

