#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <sys/stat.h>
#include "multicast.h"
#include "shared_structs.h"

#define MAX_FILES 256
#define RECV_BUFFER_SIZE 65536

/*
 * In-memory state for one file being reconstructed from multicast chunks.
 * The receiver indexes this array by file_id.
 */
struct received_file {
  bool has_metadata;
  bool completed;
  uint32_t file_size;
  uint32_t file_checksum;
  uint32_t total_chunks;
  char **chunks;
  uint32_t *chunk_sizes;
  bool *received;
  uint32_t received_count;
  char filename[m_file_name_max_len]; 
};

/* Simple additive checksum used by both sender and receiver. */
static uint32_t compute_chunk_checksum(const char *data, uint32_t size) {
  uint32_t sum = 0;
  for (uint32_t i = 0; i < size; i++) {
    sum += (unsigned char)data[i];
  }
  return sum;
}

/* Ensure output directory exists for reconstructed files. */
static int ensure_received_dir(void) {
  if (mkdir("received_files", 0755) == 0) {
    return 0;
  }
  if (errno == EEXIST) {
    return 0;
  }
  perror("mkdir received_files");
  return -1;
}

/* Allocate and initialize per-file buffers after metadata arrives. */
static int init_file_state(struct received_file *file, const struct MetadataPacket *meta) {
  file->file_size = meta->file_size;
  file->file_checksum = meta->file_checksum;
  file->total_chunks = meta->total_chunks;
  file->received_count = 0;
  file->completed = false;

  file->chunks = calloc(file->total_chunks, sizeof(char *));
  file->chunk_sizes = calloc(file->total_chunks, sizeof(uint32_t));
  file->received = calloc(file->total_chunks, sizeof(bool));
  if (!file->chunks || !file->chunk_sizes || !file->received) {
    fprintf(stderr, "Allocation failed for file tracking state\n");
    return -1;
  }

  snprintf(file->filename, sizeof(file->filename), "%s", meta->filename);

  file->has_metadata = true;
  return 0;
}

/*
 * Reassemble a completed file in chunk order and verify final checksum
 * before reporting success.
 */
static int write_completed_file(const struct received_file *file) {
  char path[128];
  snprintf(path, sizeof(path), "received_files/%s", file->filename);

  FILE *out = fopen(path, "wb");
  if (!out) {
    perror("fopen output file");
    return -1;
  }

  uint32_t calculated_file_checksum = 0;
  for (uint32_t i = 0; i < file->total_chunks; i++) {
    size_t written = fwrite(file->chunks[i], 1, file->chunk_sizes[i], out);
    if (written != file->chunk_sizes[i]) {
      fclose(out);
      fprintf(stderr, "Failed while writing %s\n", file->filename);
      return -1;
    }
    calculated_file_checksum += compute_chunk_checksum(file->chunks[i], file->chunk_sizes[i]);
  }

  fclose(out);

  if (calculated_file_checksum != file->file_checksum) {
    fprintf(stderr, "Final checksum mismatch for file %s (expected=%u got=%u)\n",
        file->filename, file->file_checksum, calculated_file_checksum);
    return -1;
  }

  printf("Completed file %s -> %s (%u chunks)\n", file->filename, path, file->total_chunks);
  return 0;
}

/* Handle metadata packet: allocate tracking state on first sighting. */
static void process_metadata(struct received_file files[], const struct MetadataPacket *meta) {
  if (meta->file_id < 0 || meta->file_id >= MAX_FILES) {
    return;
  }

  struct received_file *file = &files[meta->file_id];
  if (!file->has_metadata) {
    if (init_file_state(file, meta) != 0) {
      exit(1);
    }
    printf("Metadata file_id=%d chunks=%u size=%u\n", meta->file_id, meta->total_chunks, meta->file_size);
  }
}

/*
 * Handle data packet: validate bounds/checksum, store chunk once,
 * and finalize the file when all chunks are present.
 */
static void process_data(struct received_file files[], const struct DataPacket *packet, int packet_len) {
  if (packet->file_id < 0 || packet->file_id >= MAX_FILES) {
    return;
  }

  struct received_file *file = &files[packet->file_id];
  if (!file->has_metadata || file->completed) {
    return;
  }

  if (packet->seq_num < 0 || (uint32_t)packet->seq_num >= file->total_chunks) {
    return;
  }

  uint32_t payload_offset = (uint32_t)offsetof(struct DataPacket, data);
  if (packet_len <= (int)payload_offset) {
    return;
  }

  uint32_t payload_len = (uint32_t)packet_len - payload_offset;
  if (payload_len == 0 || payload_len > file->file_size) {
    return;
  }

  uint32_t calculated = compute_chunk_checksum(packet->data, payload_len);
  if (calculated != packet->chunk_checksum) {
    return;
  }

  if (file->received[packet->seq_num]) {
    return;
  }

  file->chunks[packet->seq_num] = malloc(payload_len);
  if (!file->chunks[packet->seq_num]) {
    fprintf(stderr, "malloc failed for chunk\n");
    exit(1);
  }

  memcpy(file->chunks[packet->seq_num], packet->data, payload_len);
  file->chunk_sizes[packet->seq_num] = payload_len;
  file->received[packet->seq_num] = true;
  file->received_count++;

  if (file->received_count == file->total_chunks) {
    if (write_completed_file(file) == 0) {
      file->completed = true;
    }
  }
}

int main(void) {
  /* Prepare output directory and initialize per-file state table. */
  if (ensure_received_dir() != 0) {
    return 1;
  }

  struct received_file files[MAX_FILES] = {0};

  /* Join multicast group and begin receive loop. */
  mcast_t *m = multicast_init("239.0.0.1", 5000, 5000);
  multicast_setup_recv(m);

  unsigned char buffer[RECV_BUFFER_SIZE];
  while (1) {
    /* Poll before recvfrom to avoid blocking forever when idle. */
    if (multicast_check_receive(m) <= 0) {
      continue;
    }

    /* Dispatch by packet size: metadata has fixed size, data is variable. */
    int n = multicast_receive(m, buffer, sizeof(buffer));

    // Reads first 4 bytes of buffer to check type (only works because we have type as first field in both)
    int packet_type = *((int *)buffer);

    //if (n == (int)sizeof(struct MetadataPacket)) {
    if (packet_type == META_TYPE) {
      process_metadata(files, (const struct MetadataPacket *)buffer);
    // } else if (n >= (int)offsetof(struct DataPacket, data)) {
    } else if (packet_type == DATA_TYPE) {
      process_data(files, (const struct DataPacket *)buffer, n);
    }
  }

  multicast_destroy(m);
  return 0;
}