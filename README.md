# 535A4: Multicast File Sharing System

Charlie and Ash - Assignment 4: Reliable Multicast File Distribution

## Overview

A reliable multicast-based file distribution system implemented in C that allows a single sender to distribute heterogeneous files (text and binary) to multiple receivers over a local area network. The system is designed to handle late joiners, ensure file integrity through checksums, and provide statistics on transmission performance.

## Building

### Prerequisites
- GCC compiler
- Standard C libraries
- Linux/Unix environment

### Build Instructions

```bash
# Build all targets
make

# Build only sender
gcc -Wall -Wextra -g -O2 -o sender sender.c multicast.c -lm

# Build only receiver  
gcc -Wall -Wextra -g -O2 -o receiver receiver.c multicast.c -lm

# Clean build artifacts
make clean
```

## Usage

### Running the Sender

The sender loads files from command-line arguments and distributes them to receivers via multicast.

```bash
# Default chunk size (1024 bytes)
./sender file1.txt file2.csv file3.dat

# Custom chunk size (256 bytes)
./sender -c 256 file1.txt file2.csv

# With provided test files
./sender share/descr.txt
```

**Command-line Options:**
- `-c <size>`: Set chunk size in bytes (default: 1024)
- `<files...>`: Files to distribute

**Sender Features:**
- Splits files into chunks of configurable size
- Sends metadata packets first, then cycles through data packets
- Implements cyclic retransmission of all chunks
- Responds to receiver requests for missing chunks (late-joiner support)
- Tracks transmission statistics (packets/sec, bytes/sec, cycles)

### Running the Receiver

The receiver joins the multicast group and reconstructs files as they arrive.

```bash
./receiver
```

**Receiver Features:**
- Joins multicast group automatically
- Buffers chunks in RAM and handles out-of-order delivery
- Validates each chunk with 32-bit additive checksum
- Detects missing chunks and requests retransmission (after 5-second timeout)
- Verifies file integrity upon completion
- Saves reconstructed files to `received_files/` directory
- Exits automatically after all files received and verified

### Example Session

**Terminal 1 (Receiver - start first or any time):**
```bash
./receiver
```

**Terminal 2 (Sender):**
```bash
./sender -c 512 share/descr.txt
```

The receiver will:
1. Wait for metadata packets
2. Buffer incoming data chunks
3. If any chunks arrive late, request retransmission after 5 seconds
4. Write the complete file to `received_files/descr.txt` once all chunks verified

## Design Choices

### 1. Packet Structure and Protocol

**Three Packet Types:**
- **MetadataPacket (Type 0)**: Contains file metadata
  - file_id, file_size, total_chunks, file_checksum, filename
  - Sent before each transmission cycle
  
- **DataPacket (Type 1)**: Contains actual file data
  - file_id, seq_num (chunk number), chunk_checksum, data payload
  - Variable-length payload (flexible array member)
  
- **RequestPacket (Type 2)**: Retransmission request
  - file_id, seq_num - requests specific missing chunk
  - Enables late-joiner support

### 2. Reliable Delivery - Late Joiner Support

**Mechanism:**
- Sender continuously cycles through metadata + all files, retransmitting indefinitely
- Receivers track metadata receive time for each file
- After 5-second timeout from metadata arrival, receivers request any missing chunks
- Sender listens for RequestPackets and immediately sends requested chunks
- This allows receivers joining mid-transmission to eventually get all data

**Why This Approach:**
- Simple and effective for LAN scenarios
- Minimal overhead from requests (only sent if chunks missing)
- Tolerates both late joiners and temporary receiver crashes/restarts
- Request-based retransmission is more efficient than continuous flooding

### 3. Integrity Verification

**Dual Checksum Approach:**
- **Chunk-level checksums**: 32-bit additive checksum computed per chunk
  - Detected in sender for inclusion in DataPacket
  - Verified in receiver before storing chunk
  - Discards corrupted chunks (requests retransmission)
  
- **File-level checksums**: Aggregate of all chunk checksums
  - Verified in receiver after all chunks received
  - Ensures no chunks were swapped or missing
  - File written to disk only if checksums match

**Checksum Algorithm:**
Simple additive checksum - sum of all byte values modulo 2^32. Sufficient for this LAN-based system.

### 4. Chunking Strategy

**Sequential File Distribution:**
- Files processed sequentially (one file's all chunks, then next file)
- Within each file, chunks sent in order by sequence number
- Single metadata packet type handles variable-sized files

**Chunk Size Configuration:**
- Default: 1024 bytes (good balance for typical LAN)
- Configurable via `-c` flag for experimentation
- Impacts network overhead, memory usage, and transmission latency

### 5. Memory Management

**Receiver Memory Strategy:**
- Chunks buffered in RAM during reception (`char **chunks` array)
- Entire file kept in memory until all chunks received and verified
- Written to disk only after integrity check passes
- Memory freed immediately after file write
- Practical for files up to 256 MB on typical systems

**Why RAM buffering:**
- Simple implementation, easier to verify checksums
- Faster than incremental disk I/O with verification
- Acceptable for LAN scenario with reasonable file sizes

### 6. Multicast Configuration

- **Group Address**: 239.0.0.1 (reserved for this application)
- **Port**: 5000 (send and receive)
- **TTL**: 1 (LAN only, no routing)
- **Family**: IPv4 UDP multicast

## Performance Characteristics

### Metrics Tracked (Sender Statistics)
- **Cycles**: Number of complete file distribution cycles
- **Meta packets fired**: Total metadata packets sent
- **Data packets fired**: Total data packets sent
- **Throughput (packets/sec)**: Rate of packet transmission
- **Byte rate (bytes/sec)**: Data throughput in bytes

### Receiver Statistics
- **Total time**: Duration from first packet received to all files verified
- **File completion**: Per-file confirmation with chunk count and path

### Network Efficiency
- Early receivers get files in streaming fashion (1 cycle time)
- Late joiners need retransmission requests (additional 5+ seconds)
- Minimal protocol overhead: only metadata + data + selective requests
- No ACKs or handshakes required

## File Organization

```
535A4/
├── Makefile              # Build configuration
├── README.md             # This file
├── multicast.h/.c        # Multicast socket utilities
├── shared_structs.h      # Common packet definitions
├── sender.c              # Sender implementation
├── receiver.c            # Receiver implementation
├── sender                # Compiled sender binary
├── receiver              # Compiled receiver binary
├── received_files/       # Output directory for received files
└── share/                # Test files
    └── descr.txt         # Sample file for testing
```

## Testing Notes

- Sender and receiver can start in any order (receiver can join anytime)
- Multiple receivers can run simultaneously on the same machine (requires separate terminals)
- To simulate late joiner: start receiver while sender is in mid-cycle
- To simulate crash recovery: kill and restart receiver while sender running
- Check `received_files/` for successful file reception

## Known Limitations

- Single sender only (no sender redundancy)
- Files must fit in receiver's available RAM
- LAN only (TTL=1, no wide-area routing)
- Path truncation warning in snprintf (non-critical, pre-existing)
- Simple additive checksum (acceptable for LAN, not for untrusted networks)

## Team Contribution

This project was developed collaboratively. The implementation includes core sender and receiver functionality with late-joiner support, integrity verification through checksums, and statistics tracking. Both team members contributed to design, implementation, and testing throughout the development process.
