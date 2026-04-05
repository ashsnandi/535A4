#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include "multicast.h"
#include "shared_structs.h"
#include "time.h" // stats timing

#define MAX_FILES 256
#define RECV_BUFFER_SIZE 65536

struct recieve_statistics {
    bool done;
    time_t kickoff_time;
    time_t end_time;
  uint32_t files_completed;
  uint64_t data_packets_seen;
  uint64_t bad_data_packets;
  uint64_t duplicate_data_packets;
  uint64_t unique_chunks_received;
  uint64_t retransmission_requests_sent;
  uint64_t estimated_packets_lost;
  uint64_t recovered_packets;
};

static void append_receiver_stats_csv(const struct recieve_statistics *statistics,
                    int files_received) {
  const char *csv_path = "receiver_stats.csv";
  bool needs_header = false;

  struct stat st;
  if (stat(csv_path, &st) != 0) {
    needs_header = true;
  }

  FILE *csv = fopen(csv_path, "a");
  if (!csv) {
    perror("fopen receiver_stats.csv");
    return;
  }

  if (needs_header) {
    fprintf(csv, "kickoff_time,end_time,total_time_sec,files_received,data_packets_seen,bad_data_packets,duplicate_data_packets,unique_chunks_received,retransmission_requests_sent,estimated_packets_lost,recovered_packets,recovery_rate_pct\n");
  }

  double total_time = difftime(statistics->end_time, statistics->kickoff_time);
  double recovery_rate_pct = 0.0;
  if (statistics->estimated_packets_lost > 0) {
    recovery_rate_pct =
        ((double)statistics->recovered_packets * 100.0) /
        (double)statistics->estimated_packets_lost;
  }

  fprintf(csv, "%ld,%ld,%.2f,%d,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.2f\n",
          (long)statistics->kickoff_time,
          (long)statistics->end_time,
          total_time,
          files_received,
          (unsigned long long)statistics->data_packets_seen,
          (unsigned long long)statistics->bad_data_packets,
          (unsigned long long)statistics->duplicate_data_packets,
          (unsigned long long)statistics->unique_chunks_received,
          (unsigned long long)statistics->retransmission_requests_sent,
          (unsigned long long)statistics->estimated_packets_lost,
          (unsigned long long)statistics->recovered_packets,
          recovery_rate_pct);

  fclose(csv);
  fprintf(stderr, "[RECEIVER] Appended stats to %s\n", csv_path);
}

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
  bool *requested;
  uint32_t received_count;
  char filename[m_file_name_max_len];
  time_t metadata_received_time;
  time_t last_request_time;
};

static void free_file_state(struct received_file *file) {
  if (file->chunks) {
    for (uint32_t i = 0; i < file->total_chunks; i++) {
      free(file->chunks[i]);
    }
  }

  free(file->chunks);
  free(file->chunk_sizes);
  free(file->received);
  free(file->requested);

  memset(file, 0, sizeof(*file));
}

/* CRC-32 polynomial: standard 0x04C11DB7 */
static uint32_t crc32_table[256];
static bool crc32_table_initialized = false;

static void init_crc32_table(void) {
  if (crc32_table_initialized) return;
  
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t crc = i;
    for (int j = 0; j < 8; j++) {
      if (crc & 1) {
        crc = (crc >> 1) ^ 0xEDB88320;
      } else {
        crc = crc >> 1;
      }
    }
    crc32_table[i] = crc;
  }
  crc32_table_initialized = true;
}

/* Compute CRC-32 checksum of data buffer */
static uint32_t compute_chunk_checksum(const char *data, uint32_t size) {
  init_crc32_table();
  uint32_t crc = 0xFFFFFFFF;
  for (uint32_t i = 0; i < size; i++) {
    uint8_t byte = (uint8_t)data[i];
    crc = crc32_table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFF;
}

/* Ensure output directory exists for reconstructed files. */
static int ensure_received_dir(void) {
  printf("[RECEIVER] Ensuring received_files directory exists...\n");
  if (mkdir("received_files", 0755) == 0) {
    printf("[RECEIVER] Created received_files directory\n");
    return 0;
  }
  if (errno == EEXIST) {
    printf("[RECEIVER] received_files directory already exists\n");
    return 0;
  }
  perror("mkdir received_files");
  return -1;
}

static int send_request_packet(const struct RequestPacket *req) {
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    perror("socket");
    return -1;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr("239.0.0.1");
  addr.sin_port = htons(5001);

  int rc = sendto(sock, req, sizeof(*req), 0, (struct sockaddr *)&addr, sizeof(addr));
  if (rc < 0) {
    perror("sendto request");
  }

  close(sock);
  return rc;
}

/* Allocate and initialize per-file buffers after metadata arrives. */
static int init_file_state(struct received_file *file, const struct MetadataPacket *meta) {
  file->file_size = meta->file_size;
  file->file_checksum = meta->file_checksum;
  file->total_chunks = meta->total_chunks;
  file->received_count = 0;
  file->completed = false;
  file->metadata_received_time = time(NULL);
  file->last_request_time = 0;

  printf("[REASSEMBLY] Allocating buffers for file: total_chunks=%u, file_size=%u bytes\n", 
         file->total_chunks, file->file_size);

  file->chunks = calloc(file->total_chunks, sizeof(char *));
  file->chunk_sizes = calloc(file->total_chunks, sizeof(uint32_t));
  file->received = calloc(file->total_chunks, sizeof(bool));
  file->requested = calloc(file->total_chunks, sizeof(bool));
  if (!file->chunks || !file->chunk_sizes || !file->received || !file->requested) {
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

  printf("[FILE VALIDATION] Starting file-level validation for %s\n", file->filename);

  FILE *out = fopen(path, "wb");
  if (!out) {
    perror("fopen output file");
    return -1;
  }

  printf("[DISK WRITE] Writing %u chunks to disk...\n", file->total_chunks);
  
  uint32_t calculated_file_checksum = 0;
  uint64_t total_bytes_written = 0;
  
  for (uint32_t i = 0; i < file->total_chunks; i++) {
    size_t written = fwrite(file->chunks[i], 1, file->chunk_sizes[i], out);
    if (written != file->chunk_sizes[i]) {
      fclose(out);
      fprintf(stderr, "[ERROR] Failed while writing chunk %u of %s\n", i, file->filename);
      return -1;
    }
    total_bytes_written += written;
    calculated_file_checksum = crc32_table[(calculated_file_checksum ^ (unsigned char)file->chunks[i][0]) & 0xFF] ^ (calculated_file_checksum >> 8);
  }
  
  /* Proper aggregation of CRC-32 for file */
  calculated_file_checksum = 0xFFFFFFFF;
  for (uint32_t i = 0; i < file->total_chunks; i++) {
    for (uint32_t j = 0; j < file->chunk_sizes[i]; j++) {
      uint8_t byte = (uint8_t)file->chunks[i][j];
      calculated_file_checksum = crc32_table[(calculated_file_checksum ^ byte) & 0xFF] ^ (calculated_file_checksum >> 8);
    }
  }
  calculated_file_checksum ^= 0xFFFFFFFF;

  fclose(out);

  printf("[DISK WRITE] Written %" PRIu64 " bytes to disk\n", total_bytes_written);
  printf("[FILE VALIDATION] CRC-32 verification: expected=0x%08x, calculated=0x%08x\n", 
         file->file_checksum, calculated_file_checksum);

  if (calculated_file_checksum != file->file_checksum) {
    fprintf(stderr, "[ERROR] CRC-32 mismatch for file %s!\n", file->filename);
    return -1;
  }

  printf("[FILE VALIDATION] SUCCESS: File %s validated and saved to %s (%u chunks, %" PRIu64 " bytes)\n", 
         file->filename, path, file->total_chunks, total_bytes_written);
  return 0;
}

/* Handle metadata packet: allocate tracking state on first sighting. */
static void process_metadata(struct received_file files[], const struct MetadataPacket *meta) {
  if (meta->file_id < 0 || meta->file_id >= MAX_FILES) {
    return;
  }

  struct received_file *file = &files[meta->file_id];
  if (!file->has_metadata) {
    printf("[LISTEN] Received metadata packet for file_id=%d, filename=%s\n", meta->file_id, meta->filename);
    printf("[LISTEN] File info: chunks=%u, size=%u bytes, CRC-32=0x%08x\n", 
           meta->total_chunks, meta->file_size, meta->file_checksum);
    if (init_file_state(file, meta) != 0) {
      exit(1);
    }
    return;
  }

  bool metadata_changed =
      file->file_size != meta->file_size ||
      file->file_checksum != meta->file_checksum ||
      file->total_chunks != meta->total_chunks ||
      strncmp(file->filename, meta->filename, sizeof(file->filename)) != 0;

  if (file->completed || metadata_changed) {
    printf("[LISTEN] Reinitializing state for file_id=%d (new transfer detected: %s)\n",
           meta->file_id,
           meta->filename);
    free_file_state(file);
    if (init_file_state(file, meta) != 0) {
      exit(1);
    }
  }
}

/* Request missing chunks from sender for late-joiner support. */
static void request_missing_chunks(mcast_t *m, struct received_file files[], struct recieve_statistics *statistics) {
  #define REQUEST_TIMEOUT 5   // Wait 5 seconds after metadata before requesting missing chunks
  #define REQUEST_RETRY 2     // Re-send requests every 2 seconds until the file completes
  (void)m;
  
  time_t now = time(NULL);
  
  for (int i = 0; i < MAX_FILES; i++) {
    struct received_file *file = &files[i];
    
    // Skip if no metadata yet or already completed
    if (!file->has_metadata || file->completed) {
      continue;
    }
    
    // Only request if enough time has passed since metadata arrived
    if (difftime(now, file->metadata_received_time) < REQUEST_TIMEOUT) {
      continue;
    }

    // Throttle retries so we don't spam the network every loop iteration.
    if (file->last_request_time != 0 && difftime(now, file->last_request_time) < REQUEST_RETRY) {
      continue;
    }
    
    // Request all missing chunks
    int requests_sent = 0;
    for (uint32_t j = 0; j < file->total_chunks; j++) {
      if (!file->received[j]) {
        struct RequestPacket req;
        req.type = REQUEST_TYPE;
        req.file_id = i;
        req.seq_num = j;
        
        if (send_request_packet(&req) >= 0) {
          requests_sent++;
          statistics->retransmission_requests_sent++;
          if (!file->requested[j]) {
            file->requested[j] = true;
            statistics->estimated_packets_lost++;
          }
        }
      }
    }
    
    if (requests_sent > 0) {
      printf("Requested %d missing chunks for file_id=%d\n", requests_sent, i);
      file->last_request_time = now;
    }
  }
}

/*
 * Handle data packet: validate bounds/checksum, store chunk once,
 * and finalize the file when all chunks are present.
 */
static void process_data(struct received_file files[], const struct DataPacket *packet, int packet_len, struct recieve_statistics *s) {
  s->data_packets_seen++;

  if (packet->file_id < 0 || packet->file_id >= MAX_FILES) {
    s->bad_data_packets++;
    return;
  }

  struct received_file *file = &files[packet->file_id];
  if (!file->has_metadata || file->completed) {
    return;
  }

  if (packet->seq_num < 0 || (uint32_t)packet->seq_num >= file->total_chunks) {
    s->bad_data_packets++;
    return;
  }

  uint32_t payload_offset = (uint32_t)offsetof(struct DataPacket, data);
  if (packet_len <= (int)payload_offset) {
    s->bad_data_packets++;
    return;
  }

  uint32_t payload_len = (uint32_t)packet_len - payload_offset;
  if (payload_len == 0 || payload_len > file->file_size) {
    s->bad_data_packets++;
    return;
  }

  uint32_t calculated = compute_chunk_checksum(packet->data, payload_len);
  if (calculated != packet->chunk_checksum) {
    s->bad_data_packets++;
    return;
  }

  if (file->received[packet->seq_num]) {
    s->duplicate_data_packets++;
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
  s->unique_chunks_received++;

  if (file->requested[packet->seq_num]) {
    s->recovered_packets++;
  }
  
  printf("[OUT-OF-ORDER] Buffered chunk %d/%u in RAM (size=%u bytes, CRC-32=0x%08x)\n", 
         packet->seq_num + 1, file->total_chunks, payload_len, packet->chunk_checksum);

  if (file->received_count == file->total_chunks) {
    printf("[REASSEMBLY] All chunks received for file_id=%d, reassembling...\n", packet->file_id);
    if (write_completed_file(file) == 0) {
      file->completed = true;
      s->files_completed++;
      s->end_time = time(NULL);

      double recovery_rate_pct = 0.0;
      if (s->estimated_packets_lost > 0) {
        recovery_rate_pct = ((double)s->recovered_packets * 100.0) /
                            (double)s->estimated_packets_lost;
      }

      printf("[RECEIVER STATISTICS]\n");
      printf("  Files completed: %u\n", s->files_completed);
      printf("  Data packets seen: %" PRIu64 "\n", s->data_packets_seen);
      printf("  Estimated packets lost: %" PRIu64 "\n", s->estimated_packets_lost);
      printf("  Recovered packets: %" PRIu64 "\n", s->recovered_packets);
      printf("  Recovery rate: %.2f%%\n", recovery_rate_pct);

      append_receiver_stats_csv(s, (int)s->files_completed);
    }
  }
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);

  printf("\n========================================\n");
  printf("[RECEIVER] Starting multicast receiver\n");
  printf("========================================\n\n");
  
  /* Prepare output directory and initialize per-file state table. */
  if (ensure_received_dir() != 0) {
    return 1;
  }

  struct received_file files[MAX_FILES] = {0};

  printf("[LISTEN] Joining multicast group 239.0.0.1:5000\n");
  /* Join multicast group and begin receive loop. */
  mcast_t *m = multicast_init("239.0.0.1", 5000, 5000);
  multicast_setup_recv(m);
  printf("[LISTEN] Successfully joined multicast group, waiting for packets...\n\n");

  unsigned char buffer[RECV_BUFFER_SIZE];

  // start time for stats
  struct recieve_statistics statistics;
  statistics.kickoff_time = time(NULL);
  statistics.end_time = statistics.kickoff_time;
  statistics.done = false;
  statistics.files_completed = 0;
  statistics.data_packets_seen = 0;
  statistics.bad_data_packets = 0;
  statistics.duplicate_data_packets = 0;
  statistics.unique_chunks_received = 0;
  statistics.retransmission_requests_sent = 0;
  statistics.estimated_packets_lost = 0;
  statistics.recovered_packets = 0;
  
  // Counter to throttle retransmission requests
  int request_check_counter = 0;

  while (1) {
    /* Poll before recvfrom to avoid blocking forever when idle. */
    if (multicast_check_receive(m) <= 0) {
      // Even if no data received, check if we need to request missing chunks
      request_check_counter++;
      if (request_check_counter >= 10) {
        request_check_counter = 0;
        printf("[RETRANSMISSION] Checking for missing chunks and sending requests...\n");
        request_missing_chunks(m, files, &statistics);
      }
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
      process_data(files, (const struct DataPacket *)buffer, n, &statistics);
    }
    
    // Periodically check for missing chunks to request
    request_check_counter++;
    if (request_check_counter >= 10) {
      request_check_counter = 0;
      request_missing_chunks(m, files, &statistics);
    }

  }

  multicast_destroy(m);
  return 0;
}
