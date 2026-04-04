// sender.c

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>
#include "multicast.h"
// shared header for shared defs?
#include "shared_structs.h"
#include <unistd.h> // for sleeping
#include <time.h> // for stats timing

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
uint32_t compute_chunk_checksum(char *givenChunk, int size) {
  init_crc32_table();
  uint32_t crc = 0xFFFFFFFF;
  for (int i = 0; i < size; i++) {
    uint8_t byte = (uint8_t)givenChunk[i];
    crc = crc32_table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFF;
}

struct send_statistics {
    uint32_t data_packets_sent;
    uint32_t meta_packets_sent;
    uint64_t bytes_sent;
    uint32_t cycles_sent;
    time_t kickoff_time;
};

static void append_sender_stats_csv(const struct send_statistics *statistics,
                                    double total_time,
                                    double packsPerSec,
                                    double bytesPerSec) {
    const char *csv_path = "sender_stats.csv";
    bool needs_header = false;

    struct stat st;
    if (stat(csv_path, &st) != 0) {
        needs_header = true;
    }

    FILE *csv = fopen(csv_path, "a");
    if (!csv) {
        perror("fopen sender_stats.csv");
        return;
    }

    if (needs_header) {
        fprintf(csv, "kickoff_epoch,cycle,total_time_sec,meta_packets_sent,data_packets_sent,bytes_sent,packets_per_sec,bytes_per_sec\n");
    }

    fprintf(csv, "%ld,%u,%.2f,%u,%u,%llu,%.2f,%.2f\n",
            (long)statistics->kickoff_time,
            statistics->cycles_sent,
            total_time,
            statistics->meta_packets_sent,
            statistics->data_packets_sent,
            (unsigned long long)statistics->bytes_sent,
            packsPerSec,
            bytesPerSec);

    fclose(csv);
}

// helper: open one file and split into chunks
// Note that this includes no error checks as of now
struct chunked_file chunk_a_file(char *filename, int file_id, int chunk_size) {
    printf("[CHUNKING] Processing file: %s (chunk_size=%d bytes)\n", filename, chunk_size);

    //initialize file checksum
    uint32_t file_checksum = 0xFFFFFFFF;

    // Open the file
    FILE *file = fopen(filename, "rb");

    // Check if file opened successfully
    if (file == NULL) {
        fprintf(stderr, "File open failed!\n");
        exit(1);
    }

    // figure out file size 
    // Navigate to end of file
    fseek(file, 0, SEEK_END);
    // ftell get's current position which is size/end of file in our case
    long file_size = ftell(file);
    //convert file size to uint32_t to be consistent with our structs
    file_size = (uint32_t)file_size;
    // Return to beginning of file for reading
    rewind(file);

    // Check if file size is zero
    if (file_size == 0) {
        fprintf(stderr, "File is empty!\n");
        fclose(file);
        exit(1);
    }

    printf("[CHUNKING] File size: %ld bytes\n", file_size);

    // Figure out total chunks needed for memory allocation
    // get total chunks from file size and chunk size, add 1 if there's a remainder
    int total_chunks = file_size / chunk_size + (file_size % chunk_size != 0);
    printf("[CHUNKING] Total chunks needed: %d\n", total_chunks);
    // Allocate memory for chunk pointers, chunk sizes, and chunk checksums
    char **chunks = malloc(total_chunks * sizeof(char*));
    int *chunk_sizes = malloc(total_chunks * sizeof(int));
    uint32_t *chunk_checksums = malloc(total_chunks * sizeof(uint32_t));

    // Read chunks into memory, get checksum, and store in arrays
    for (int i = 0; i < total_chunks; i++) {
        // Quick check for last chunk edge case
        int temp_chunk_size;
        if (i == total_chunks - 1) {
            // Last chunk probably smaller so deserves special handling
            temp_chunk_size = file_size - (i * chunk_size);
        }
        else {
            // Otherwise just normal chunk size
            temp_chunk_size = chunk_size; 
        }
        // read chunk into memory
        chunks[i] = malloc(temp_chunk_size);
        fread(chunks[i], sizeof(char), temp_chunk_size, file);
        // store chunk size for later use
        chunk_sizes[i] = temp_chunk_size;

        // Get chunk checksum and store it
        chunk_checksums[i] = compute_chunk_checksum(chunks[i], temp_chunk_size);

        // Add chunk checksum to file checksum using CRC-32
        for (int k = 0; k < chunk_sizes[i]; k++) {
            uint8_t byte = (uint8_t)chunks[i][k];
            file_checksum = crc32_table[(file_checksum ^ byte) & 0xFF] ^ (file_checksum >> 8);
        }
        printf("[CHUNKING] Chunk %d: %d bytes, CRC-32=0x%08x\n", i, chunk_sizes[i], chunk_checksums[i]);
    }

    // Close because don't need it anymore, all necessary data is stored
    fclose(file);
    
    file_checksum ^= 0xFFFFFFFF;
    printf("[CHUNKING] File CRC-32: 0x%08x\n\n", file_checksum);

    // create a chunked file to return with all the info for this file
    struct chunked_file chunkedFile;
    chunkedFile.filename = filename;
    chunkedFile.file_id = file_id;
    chunkedFile.chunks = chunks;
    chunkedFile.chunk_sizes = chunk_sizes;
    chunkedFile.total_chunks = total_chunks;
    chunkedFile.file_size = file_size;
    chunkedFile.chunk_checksums = chunk_checksums;
    chunkedFile.file_checksum = file_checksum;

    return chunkedFile;
}

static void process_pending_requests(mcast_t *data_m, mcast_t *request_m,
                                     struct chunked_file *files, int file_count,
                                     struct send_statistics *stats);

// helper for sending a chunk
void send_all_chunks(mcast_t *data_m, mcast_t *request_m,
                     struct chunked_file *files, int file_count,
                     struct send_statistics *s) {
    // send chunks from all files sequentially for now (can optimize later using round robin or something)
    for (int i = 0; i < file_count; i++) {
        for (int j = 0; j < files[i].total_chunks; j++) {
            //Create a data packet for this chunk
            int chunk_size = files[i].chunk_sizes[j];
            size_t packet_size = offsetof(struct DataPacket, data) + chunk_size;
            struct DataPacket *dPac = malloc(packet_size);
            if (!dPac) {
                fprintf(stderr, "Failed to allocate data packet\n");
                exit(1);
            }

            dPac->seq_num = j;
            dPac->file_id = files[i].file_id;
            dPac->chunk_checksum = files[i].chunk_checksums[j];
            dPac->type = DATA_TYPE;
            // Actually copy the chunk data into the packet's data field
            memcpy(dPac->data, files[i].chunks[j], chunk_size);
            // send data packet using multicast_send (need to implement this function in multicast.c)

            multicast_send(data_m, dPac, (int)packet_size);
            free(dPac);

            // Add to statistics
            s->data_packets_sent++;
            s->bytes_sent += packet_size;

            // Pick up any retransmission requests between chunk sends.
            // The sender only listens on the dedicated request socket, so this stays nonblocking.
            process_pending_requests(data_m, request_m, files, file_count, s);
        }

        // add a small delay to not overwhelm the network, but keep servicing requests
        for (int pause_ms = 0; pause_ms < 1000; pause_ms += 100) {
            usleep(100000);
            process_pending_requests(data_m, request_m, files, file_count, s);
        }
    }
}

// helper for sending a metadata packet
void send_metadata_packet(mcast_t *m, struct chunked_file file, struct send_statistics *stats) {
    struct MetadataPacket met;
    met.file_size = file.file_size;
    met.file_checksum = file.file_checksum;
    met.total_chunks = file.total_chunks;
    met.file_id = file.file_id;
    met.type = META_TYPE; // just to be explicit, not really needed since we can infer from struct but it's fine

    int packet_size = sizeof(struct MetadataPacket);

    // Get filename from chunked_file struct and put it in metadata packet
    // %s standard formating for strings, and specify size to avoid overflow
    // strrchr gets pointer to instance of last '/' in filename
    // Worked a lot with pointers in c++ 322 class so I know we can just set the base pointer to be one after the '/' to get just the filename itself
    // this is for an edge case like if we run ./sender -c 1024 /pathto/train.csv
    char *base = strrchr(file.filename, '/');
    // Note base will be null if no '/' in filename
    // base + 1 gives us pointer to start of filename since the '/' is at the *base address
    snprintf(met.filename, sizeof(met.filename), "%s", base ? base + 1 : file.filename);


    // send metadata packet using multicast_send (need to implement this function in multicast.c)
    multicast_send(m, &met, packet_size);
    // Add to statistics
    stats->meta_packets_sent++;
    stats->bytes_sent += packet_size;
}

// helper for handling retransmission requests from receivers
void handle_retransmission_request(mcast_t *m, struct chunked_file *files, int file_count, 
                                   const struct RequestPacket *req, struct send_statistics *stats) {
    if (req->file_id < 0 || req->file_id >= file_count) {
        return;
    }

    if (req->seq_num < 0 || req->seq_num >= files[req->file_id].total_chunks) {
        return;
    }

    struct chunked_file *file = &files[req->file_id];
    int chunk_size = file->chunk_sizes[req->seq_num];
    size_t packet_size = offsetof(struct DataPacket, data) + chunk_size;
    struct DataPacket *dPac = malloc(packet_size);
    
    if (!dPac) {
        fprintf(stderr, "Failed to allocate retransmission packet\n");
        return;
    }

    dPac->seq_num = req->seq_num;
    dPac->file_id = file->file_id;
    dPac->chunk_checksum = file->chunk_checksums[req->seq_num];
    dPac->type = DATA_TYPE;
    memcpy(dPac->data, file->chunks[req->seq_num], chunk_size);

    multicast_send(m, dPac, (int)packet_size);
    free(dPac);

    stats->data_packets_sent++;
    stats->bytes_sent += packet_size;
}

static void process_pending_requests(mcast_t *data_m, mcast_t *request_m,
                                     struct chunked_file *files, int file_count,
                                     struct send_statistics *stats) {
    unsigned char req_buffer[sizeof(struct RequestPacket)];

    while (poll(request_m->fds, request_m->nfds, 0) > 0) {
        int n = multicast_receive(request_m, req_buffer, sizeof(req_buffer));
        if (n < (int)sizeof(struct RequestPacket)) {
            continue;
        }

        int packet_type = *((int *)req_buffer);
        if (packet_type != REQUEST_TYPE) {
            continue;
        }

        const struct RequestPacket *req = (const struct RequestPacket *)req_buffer;
        handle_retransmission_request(data_m, files, file_count, req, stats);
    }
}


// Need to implement statistics related stuff as well. Doing after core functionality is working



int main(int argc, char *argv[]) {

    // Input parsing given assignment instructions/example
    // Simple error handling as well
    // Example:  ./sender -c 2048 train.csv descr.txt
    // Or just: ./sender train.csv descr.txt

    int chunk_size = 1024; // default chunk size
    int file_one_idx = 1; // index of first file argument in argv

    // Note argc includes the program name so this basically means no files inputted
    if (argc < 2) {
        fprintf(stderr, "Input clearly too few arguments!\n");
        return 1;
    }

    // Check for -c without a following chunk size
    if (strcmp(argv[1], "-c") == 0 && argc <= 2) {
        fprintf(stderr, "No chunk size given after -c\n");
        return 1;
    }   

    // Check for optional -c argument
    if (strcmp(argv[1], "-c") == 0) {
        if (argc < 3) {
            fprintf(stderr, "No chunk size given after -c\n");
            return 1;
        }
        chunk_size = atoi(argv[2]);
        if (chunk_size <= 0) {
            fprintf(stderr, "Chunk size can't be zero or negative!\n");
            return 1;
        }
        file_one_idx = 3;
    }

    // Checking for any file arguments after optional -c or just in general
    if (argc <= file_one_idx) {
        fprintf(stderr, "Bad input! No files!\n");
        return 1;
    }

    // Create chunked_file structs for each file argument
    int file_count = argc - file_one_idx;
    struct chunked_file *files = malloc(file_count * sizeof(struct chunked_file));

    // Create statistics struct and initialize kickoff time
    struct send_statistics statistics;
    statistics.kickoff_time = time(NULL);
    statistics.cycles_sent = 0;
    statistics.data_packets_sent = 0;
    statistics.meta_packets_sent = 0;
    statistics.bytes_sent = 0;

    printf("\n========================================\n");
    printf("[SENDER] Starting multicast file sender\n");
    printf("========================================\n");
    printf("[CHUNKING] Chunk size set to: %d bytes\n", chunk_size);
    printf("[CHUNKING] Files to send: %d\n\n", file_count);

    // initialize multicast sender
    // This is example code right here
    printf("[TRANSMISSION] Initializing multicast group 239.0.0.1:5000\n");
    printf("[TRANSMISSION] Listening for retransmission requests on 239.0.0.1:5001\n");
    mcast_t *m = multicast_init("239.0.0.1", 5000, 0);
    mcast_t *request_m = multicast_init("239.0.0.1", 5001, 5001);
    multicast_setup_recv(request_m);
    printf("[TRANSMISSION] Multicast initialized, starting transmission cycle...\n\n");

    // load all files into memory as chunks
    for (int i = 0; i < file_count; i++) {
        int current_file_id = i; // file ID is just index, can change later if we want
        int current_file_arg_idx = i + file_one_idx;
        files[i] = chunk_a_file(argv[current_file_arg_idx], current_file_id, chunk_size);
    }

    while (1) {
        // Drain any queued retransmission requests before sending the next cycle.
        process_pending_requests(m, request_m, files, file_count, &statistics);

        // Send all metadata
        for (int i = 0; i < file_count; i++) {
            send_metadata_packet(m, files[i], &statistics);
        }

        sleep(1);

        send_all_chunks(m, request_m, files, file_count, &statistics);
        // Send all chunks from all files cyclically for now (can optimize later)

        // Drain requests again so late requests are handled before the next sleep/loop.
        process_pending_requests(m, request_m, files, file_count, &statistics);

        // Update stats
        statistics.cycles_sent++;

        // Print statistics every cycle
        time_t current_time = time(NULL);
        double total_time = difftime(current_time, statistics.kickoff_time);

        double packsPerSec = 0.0;
        double bytesPerSec = 0.0;

        // Avoid zero devision
        if (total_time <= 0) total_time = 1;

        packsPerSec = (statistics.meta_packets_sent + statistics.data_packets_sent) / total_time;
        bytesPerSec = statistics.bytes_sent / total_time;
        

        printf("\n[TRANSMISSION STATISTICS - Cycle %u]\n", statistics.cycles_sent);
        printf("  Total time elapsed: %.2f seconds\n", total_time);
        printf("  Meta packets sent: %u\n", statistics.meta_packets_sent);
        printf("  Data packets sent: %u\n", statistics.data_packets_sent);
        printf("  Throughput: %.2f packets/sec\n", packsPerSec);
        printf("  Byte rate: %.2f bytes/sec\n", bytesPerSec);
        printf("\n");

        append_sender_stats_csv(&statistics, total_time, packsPerSec, bytesPerSec);
    }

    // free chunk memory
    for (int i = 0; i < file_count; i++) {
        for (int j = 0; j < files[i].total_chunks; j++) {
            free(files[i].chunks[j]);
        }
        free(files[i].chunks);
        free(files[i].chunk_sizes);
        free(files[i].chunk_checksums);
    }

    // free files array
    free(files);

    // free multicast sender resources
    multicast_destroy(m);

    // Returning :)
    return 0;
}


/*
Tasks

A: Sender Program (sender.c)

- Read files from command-line arguments
  (e.g., ./sender train.csv descr.txt)

- Chunking:
  * Split files into chunks.
    - Chunk size must be configurable via command-line argument
      (e.g., ./sender -c 1024 train.csv)
    - Default chunk size: 1024 bytes if not specified

  * Add headers with metadata:
    - sequence number
    - file ID
    - checksum
    - total chunks

- Transmission:
  * Send chunks to the multicast group

  * Implement reliability:
    - retransmit chunks cyclically OR
    - retransmit upon receiver requests

- Track statistics
*/

