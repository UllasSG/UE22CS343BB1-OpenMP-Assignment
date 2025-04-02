#include <assert.h>
#include <stdio.h>
#include <omp.h>
#include <stdlib.h>

#define NUM_PROCS 4
#define CACHE_SIZE 4
#define MEM_SIZE 16
#define MSG_BUFFER_SIZE 256
#define MAX_INSTR_NUM 32

typedef unsigned char byte;

typedef enum { MODIFIED, EXCLUSIVE, SHARED, INVALID } cacheLineState;
typedef enum { EM, S, U } directoryEntryState;

typedef enum { 
    READ_REQUEST,
    WRITE_REQUEST,
    REPLY_RD,
    REPLY_WR,
    REPLY_ID,
    INV,
    UPGRADE,
    WRITEBACK_INV,
    WRITEBACK_INT,
    FLUSH,
    FLUSH_INVACK,
    EVICT_SHARED,
    EVICT_MODIFIED
} transactionType;

typedef struct instruction {
    byte type;
    byte address;
    byte value;
} instruction;

typedef struct cacheLine {
    byte address;
    byte value;
    cacheLineState state;
} cacheLine;

typedef struct directoryEntry {
    byte bitVector;
    directoryEntryState state;
} directoryEntry;

typedef struct message {
    transactionType type;
    int sender;
    byte address;
    byte value;
    byte bitVector;
    int secondReceiver;
    directoryEntryState dirState;
} message;

typedef struct messageBuffer {
    message queue[ MSG_BUFFER_SIZE ];
    int head;
    int tail;
    int count;
} messageBuffer;

typedef struct processorNode {
    cacheLine cache[ CACHE_SIZE ];
    byte memory[ MEM_SIZE ];
    directoryEntry directory[ MEM_SIZE ];
    instruction instructions[ MAX_INSTR_NUM ];
    int instructionCount;
} processorNode;

void initializeProcessor( int threadId, processorNode *node, char *dirName );
void sendMessage( int receiver, message msg );
void handleCacheReplacement( int sender, cacheLine oldCacheLine );
void printProcessorState( int processorId, processorNode node );

messageBuffer messageBuffers[ NUM_PROCS ];
omp_lock_t msgBufferLocks[ NUM_PROCS ];

int main( int argc, char * argv[] ) {
    if (argc < 2) {
        fprintf( stderr, "Usage: %s <test_directory>\n", argv[0] );
        return EXIT_FAILURE;
    }
    char *dirName = argv[1];
    
    for ( int i = 0; i < NUM_PROCS; i++ ) {
        messageBuffers[ i ].count = 0;
        messageBuffers[ i ].head = 0;
        messageBuffers[ i ].tail = 0;
        omp_init_lock(&msgBufferLocks[i]);
    }
    
    processorNode nodes[NUM_PROCS];
    
    #pragma omp parallel num_threads(NUM_PROCS)
    {
        int threadId = omp_get_thread_num();
        processorNode *node = &nodes[threadId];
        initializeProcessor( threadId, node, dirName );

        message msg;
        message msgReply;
        instruction instr;
        int instructionIdx = -1;
        int printProcState = 1;
        byte waitingForReply = 0;
        
        while ( 1 ) {
            // Process messages in the buffer
            while ( 
                messageBuffers[ threadId ].count > 0 &&
                messageBuffers[ threadId ].head != messageBuffers[ threadId ].tail
            ) {
                if ( printProcState == 0 ) {
                    printProcState++;
                }
                
                omp_set_lock(&msgBufferLocks[threadId]);
                int head = messageBuffers[ threadId ].head;
                msg = messageBuffers[ threadId ].queue[ head ];
                messageBuffers[ threadId ].head = ( head + 1 ) % MSG_BUFFER_SIZE;
                messageBuffers[ threadId ].count--;
                omp_unset_lock(&msgBufferLocks[threadId]);

                byte procNodeAddr = (msg.address >> 4) & 0x0F;
                byte memBlockAddr = msg.address & 0x0F;
                byte cacheIndex = memBlockAddr % CACHE_SIZE;

                #ifdef DEBUG_MSG
                printf("Processor %d received msg from: %d, type: %d, address: 0x%02X\n",
                       threadId, msg.sender, msg.type, msg.address);
                #endif

                switch ( msg.type ) {
                    case READ_REQUEST:
                        // Handle read request from another processor
                        {
                            #ifdef DEBUG
                            printf("Processor %d handling READ_REQUEST from %d for address 0x%02X\n",
                                   threadId, msg.sender, msg.address);
                            #endif

                            // Get directory entry for the requested block
                            directoryEntry *dirEntry = &node->directory[memBlockAddr];
                            
                            if (dirEntry->state == U) {
                                // Memory block is not cached, send data directly
                                msgReply.type = REPLY_RD;
                                msgReply.sender = threadId;
                                msgReply.address = msg.address;
                                msgReply.value = node->memory[memBlockAddr];
                                msgReply.bitVector = 0;
                                
                                // Update directory state
                                dirEntry->state = S;
                                dirEntry->bitVector |= (1 << msg.sender);
                                
                                sendMessage(msg.sender, msgReply);
                            }
                            else if (dirEntry->state == S) {
                                // Block is in shared state, send data directly
                                msgReply.type = REPLY_RD;
                                msgReply.sender = threadId;
                                msgReply.address = msg.address;
                                msgReply.value = node->memory[memBlockAddr];
                                msgReply.bitVector = dirEntry->bitVector;
                                
                                // Update directory state to include new sharer
                                dirEntry->bitVector |= (1 << msg.sender);
                                
                                sendMessage(msg.sender, msgReply);
                            }
                            else if (dirEntry->state == EM) {
                                // Block is exclusively owned, need to get latest copy
                                // Find the owner
                                int owner = -1;
                                for (int i = 0; i < NUM_PROCS; i++) {
                                    if (dirEntry->bitVector & (1 << i)) {
                                        owner = i;
                                        break;
                                    }
                                }
                                
                                if (owner != -1) {
                                    // Request writeback from owner
                                    msgReply.type = WRITEBACK_INT;
                                    msgReply.sender = threadId;
                                    msgReply.address = msg.address;
                                    msgReply.secondReceiver = msg.sender;
                                    
                                    sendMessage(owner, msgReply);
                                }
                            }
                        }
                        break;

                    case REPLY_RD:
                        // Handle reply to a read request
                        {
                            #ifdef DEBUG
                            printf("Processor %d handling REPLY_RD from %d for address 0x%02X\n",
                                   threadId, msg.sender, msg.address);
                            #endif

                            // Check if we need to replace a cache line
                            cacheLine *cl = &node->cache[cacheIndex];
                            
                            if (cl->address != 0xFF && cl->address != msg.address) {
                                // Need cache replacement
                                cacheLine oldCacheLine = *cl;
                                handleCacheReplacement(threadId, oldCacheLine);
                            }
                            
                            // Update cache with received data
                            cl->address = msg.address;
                            cl->value = msg.value;
                            cl->state = SHARED;
                            
                            // Mark as no longer waiting for reply
                            waitingForReply = 0;
                        }
                        break;

                    case WRITEBACK_INT:
                        // Request to flush data (changed to SHARED)
                        {
                            #ifdef DEBUG
                            printf("Processor %d handling WRITEBACK_INT from %d for address 0x%02X\n",
                                   threadId, msg.sender, msg.address);
                            #endif
                            
                            // Find the cache line
                            cacheLine *cl = NULL;
                            for (int i = 0; i < CACHE_SIZE; i++) {
                                if (node->cache[i].address == msg.address) {
                                    cl = &node->cache[i];
                                    break;
                                }
                            }
                            
                            if (cl != NULL && (cl->state == MODIFIED || cl->state == EXCLUSIVE)) {
                                // Send data to home node and requestor
                                msgReply.type = FLUSH;
                                msgReply.sender = threadId;
                                msgReply.address = msg.address;
                                msgReply.value = cl->value;
                                
                                // Send to home node
                                sendMessage(msg.sender, msgReply);
                                
                                // Send to requestor if different
                                if (msg.secondReceiver != msg.sender) {
                                    sendMessage(msg.secondReceiver, msgReply);
                                }
                                
                                // Update local cache state
                                if (cl->state == MODIFIED) {
                                    cl->state = SHARED;
                                } else if (cl->state == EXCLUSIVE) {
                                    cl->state = SHARED;
                                }
                            }
                        }
                        break;

                    case FLUSH:
                        // Handle cache flush
                        {
                            #ifdef DEBUG
                            printf("Processor %d handling FLUSH from %d for address 0x%02X\n",
                                   threadId, msg.sender, msg.address);
                            #endif
                            
                            // If this is the home node, update memory
                            if ((msg.address >> 4) == threadId) {
                                byte memIdx = msg.address & 0x0F;
                                node->memory[memIdx] = msg.value;
                                
                                // Update directory state
                                directoryEntry *dirEntry = &node->directory[memIdx];
                                dirEntry->state = S;
                                
                                // Add sender and requestor to bit vector if not already present
                                dirEntry->bitVector |= (1 << msg.sender);
                            }
                            else {
                                // This is the requestor, update cache
                                byte cacheIdx = msg.address & 0x0F;
                                cacheIdx %= CACHE_SIZE;
                                
                                cacheLine *cl = &node->cache[cacheIdx];
                                
                                if (cl->address != 0xFF && cl->address != msg.address) {
                                    cacheLine oldCacheLine = *cl;
                                    handleCacheReplacement(threadId, oldCacheLine);
                                }
                                
                                cl->address = msg.address;
                                cl->value = msg.value;
                                cl->state = SHARED;
                                
                                waitingForReply = 0;
                            }
                        }
                        break;

                    case UPGRADE:
                        // Handle upgrade request
                        {
                            #ifdef DEBUG
                            printf("Processor %d handling UPGRADE from %d for address 0x%02X\n",
                                   threadId, msg.sender, msg.address);
                            #endif
                            
                            // Get directory entry
                            directoryEntry *dirEntry = &node->directory[memBlockAddr];
                            
                            // Send invalidation to all sharers except the requestor
                            for (int i = 0; i < NUM_PROCS; i++) {
                                if ((dirEntry->bitVector & (1 << i)) && i != msg.sender) {
                                    msgReply.type = INV;
                                    msgReply.sender = threadId;
                                    msgReply.address = msg.address;
                                    msgReply.secondReceiver = msg.sender;
                                    
                                    sendMessage(i, msgReply);
                                }
                            }
                            
                            // Update directory state
                            dirEntry->state = EM;
                            dirEntry->bitVector = (1 << msg.sender);
                            
                            // Send reply to requestor
                            msgReply.type = REPLY_WR;
                            msgReply.sender = threadId;
                            msgReply.address = msg.address;
                            
                            sendMessage(msg.sender, msgReply);
                        }
                        break;

                    case REPLY_ID:
                        // Handle reply with IDs of sharers
                        {
                            #ifdef DEBUG
                            printf("Processor %d handling REPLY_ID from %d for address 0x%02X\n",
                                   threadId, msg.sender, msg.address);
                            #endif
                            
                            // Send invalidation to all sharers
                            for (int i = 0; i < NUM_PROCS; i++) {
                                if (msg.bitVector & (1 << i)) {
                                    msgReply.type = INV;
                                    msgReply.sender = threadId;
                                    msgReply.address = msg.address;
                                    
                                    sendMessage(i, msgReply);
                                }
                            }
                            
                            // Send upgrade request to home node
                            msgReply.type = UPGRADE;
                            msgReply.sender = threadId;
                            msgReply.address = msg.address;
                            msgReply.value = node->cache[cacheIndex].value;
                            
                            sendMessage(procNodeAddr, msgReply);
                        }
                        break;

                    case INV:
                        // Handle invalidation request
                        {
                            #ifdef DEBUG
                            printf("Processor %d handling INV from %d for address 0x%02X\n",
                                   threadId, msg.sender, msg.address);
                            #endif
                            
                            // Find the cache line to invalidate
                            for (int i = 0; i < CACHE_SIZE; i++) {
                                if (node->cache[i].address == msg.address) {
                                    // If modified, need to flush first
                                    if (node->cache[i].state == MODIFIED) {
                                        msgReply.type = FLUSH_INVACK;
                                        msgReply.sender = threadId;
                                        msgReply.address = msg.address;
                                        msgReply.value = node->cache[i].value;
                                        
                                        // Send to home node
                                        byte homeNode = (msg.address >> 4) & 0x0F;
                                        sendMessage(homeNode, msgReply);
                                    }
                                    
                                    // Invalidate the cache line
                                    node->cache[i].state = INVALID;
                                    break;
                                }
                            }
                        }
                        break;

                    case WRITE_REQUEST:
                        // Handle write request
                        {
                            #ifdef DEBUG
                            printf("Processor %d handling WRITE_REQUEST from %d for address 0x%02X\n",
                                   threadId, msg.sender, msg.address);
                            #endif
                            
                            // Get directory entry
                            directoryEntry *dirEntry = &node->directory[memBlockAddr];
                            
                            if (dirEntry->state == U) {
                                // No cached copies, grant write permission
                                msgReply.type = REPLY_WR;
                                msgReply.sender = threadId;
                                msgReply.address = msg.address;
                                msgReply.value = node->memory[memBlockAddr];
                                
                                // Update directory
                                dirEntry->state = EM;
                                dirEntry->bitVector = (1 << msg.sender);
                                
                                sendMessage(msg.sender, msgReply);
                            }
                            else if (dirEntry->state == S || dirEntry->state == EM) {
                                // There are other copies, send IDs of sharers
                                msgReply.type = REPLY_ID;
                                msgReply.sender = threadId;
                                msgReply.address = msg.address;
                                msgReply.bitVector = dirEntry->bitVector;
                                
                                sendMessage(msg.sender, msgReply);
                            }
                        }
                        break;

                    case REPLY_WR:
                        // Handle reply to write request
                        {
                            #ifdef DEBUG
                            printf("Processor %d handling REPLY_WR from %d for address 0x%02X\n",
                                   threadId, msg.sender, msg.address);
                            #endif
                            
                            // Check if we need to replace a cache line
                            cacheLine *cl = &node->cache[cacheIndex];
                            
                            if (cl->address != 0xFF && cl->address != msg.address) {
                                // Need cache replacement
                                cacheLine oldCacheLine = *cl;
                                handleCacheReplacement(threadId, oldCacheLine);
                            }
                            
                            // Update cache with new value and state
                            cl->address = msg.address;
                            // For REPLY_WR, we don't update the value since we'll write our own
                            cl->state = MODIFIED;
                            
                            // Mark as no longer waiting for reply
                            waitingForReply = 0;
                        }
                        break;

                    case WRITEBACK_INV:
                        // Request to flush data and invalidate
                        {
                            #ifdef DEBUG
                            printf("Processor %d handling WRITEBACK_INV from %d for address 0x%02X\n",
                                   threadId, msg.sender, msg.address);
                            #endif
                            
                            // Find the cache line
                            cacheLine *cl = NULL;
                            for (int i = 0; i < CACHE_SIZE; i++) {
                                if (node->cache[i].address == msg.address) {
                                    cl = &node->cache[i];
                                    break;
                                }
                            }
                            
                            if (cl != NULL && (cl->state == MODIFIED || cl->state == EXCLUSIVE)) {
                                // Send data to home node
                                msgReply.type = FLUSH;
                                msgReply.sender = threadId;
                                msgReply.address = msg.address;
                                msgReply.value = cl->value;
                                
                                sendMessage(msg.sender, msgReply);
                                
                                // Update local cache state
                                cl->state = INVALID;
                            }
                        }
                        break;

                    case FLUSH_INVACK:
                        // Handle flush with invalidation acknowledgment
                        {
                            #ifdef DEBUG
                            printf("Processor %d handling FLUSH_INVACK from %d for address 0x%02X\n",
                                   threadId, msg.sender, msg.address);
                            #endif
                            
                            // Update memory with flushed data
                            byte memIdx = msg.address & 0x0F;
                            node->memory[memIdx] = msg.value;
                            
                            // Update directory
                            directoryEntry *dirEntry = &node->directory[memIdx];
                            
                            // Remove sender from bit vector
                            dirEntry->bitVector &= ~(1 << msg.sender);
                            
                            // Check if any sharers remain
                            if (dirEntry->bitVector == 0) {
                                dirEntry->state = U;
                            }
                        }
                        break;
                    
                    case EVICT_SHARED:
                        // Handle eviction of shared line
                        {
                            #ifdef DEBUG
                            printf("Processor %d handling EVICT_SHARED from %d for address 0x%02X\n",
                                   threadId, msg.sender, msg.address);
                            #endif
                            
                            // Update directory
                            directoryEntry *dirEntry = &node->directory[memBlockAddr];
                            
                            // Remove sender from bit vector
                            dirEntry->bitVector &= ~(1 << msg.sender);
                            
                            // Check if any sharers remain
                            if (dirEntry->bitVector == 0) {
                                dirEntry->state = U;
                            }
                        }
                        break;

                    case EVICT_MODIFIED:
                        // Handle eviction of modified line
                        {
                            #ifdef DEBUG
                            printf("Processor %d handling EVICT_MODIFIED from %d for address 0x%02X\n",
                                   threadId, msg.sender, msg.address);
                            #endif
                            
                            // Update memory with latest value
                            byte memIdx = msg.address & 0x0F;
                            node->memory[memIdx] = msg.value;
                            
                            // Update directory
                            directoryEntry *dirEntry = &node->directory[memIdx];
                            dirEntry->state = U;
                            dirEntry->bitVector = 0;
                        }
                        break;
                }
            }
            
            // If waiting for a reply, don't process next instruction
            if (waitingForReply > 0) {
                continue;
            }

            // Process next instruction
            if (instructionIdx < node->instructionCount - 1) {
                instructionIdx++;
                
                #ifdef DEBUG_INSTR
                printf("Processor %d executing instruction %d\n", threadId, instructionIdx);
                #endif
            } else {
                if (printProcState > 0) {
                    printProcessorState(threadId, *node);
                    printProcState--;
                }
                break; // Exit the loop when all instructions are processed
            }
            
            instr = node->instructions[instructionIdx];

            byte procNodeAddr = (instr.address >> 4) & 0x0F; // Extract processor node from the MSB 4 bits
            byte memBlockAddr = instr.address & 0x0F;       // Extract memory block from the LSB 4 bits
            byte cacheIndex = memBlockAddr % CACHE_SIZE;     // Map memory block to cache index

            #ifdef DEBUG_INSTR
            printf("Processor %d instruction: %c address: 0x%02X value: %d\n",
                   threadId, instr.type, instr.address, instr.value);
            printf("  procNodeAddr: %d, memBlockAddr: %d, cacheIndex: %d\n",
                   procNodeAddr, memBlockAddr, cacheIndex);
            #endif

            if (instr.type == 'R') {
                // Read instruction
                // Check if data is in cache and valid
                int cacheHit = 0;
                for (int i = 0; i < CACHE_SIZE; i++) {
                    if (node->cache[i].address == instr.address && 
                        node->cache[i].state != INVALID) {
                        cacheHit = 1;
                        #ifdef DEBUG_INSTR
                        printf("Processor %d cache hit for read at index %d, value: %d\n",
                               threadId, i, node->cache[i].value);
                        #endif
                        break;
                    }
                }
                
                if (!cacheHit) {
                    // Cache miss, need to fetch data
                    #ifdef DEBUG_INSTR
                    printf("Processor %d cache miss for read, sending READ_REQUEST to %d\n",
                           threadId, procNodeAddr);
                    #endif
                    
                    // Check if we need to replace a cache line
                    cacheLine *cl = &node->cache[cacheIndex];
                    if (cl->address != 0xFF && cl->address != instr.address) {
                        // Handle cache replacement
                        cacheLine oldCacheLine = *cl;
                        handleCacheReplacement(threadId, oldCacheLine);
                    }
                    
                    // Send read request to home node
                    msg.type = READ_REQUEST;
                    msg.sender = threadId;
                    msg.address = instr.address;
                    
                    sendMessage(procNodeAddr, msg);
                    waitingForReply = 1;
                }
            } else {
                // Write instruction
                // Check if data is in cache and in appropriate state
                int cacheHit = 0;
                cacheLine *cl = &node->cache[cacheIndex];
                
                for (int i = 0; i < CACHE_SIZE; i++) {
                    if (node->cache[i].address == instr.address) {
                        if (node->cache[i].state == MODIFIED || node->cache[i].state == EXCLUSIVE) {
                            // Already have exclusive access, just update
                            node->cache[i].value = instr.value;
                            node->cache[i].state = MODIFIED;
                            cacheHit = 1;
                            
                            #ifdef DEBUG_INSTR
                            printf("Processor %d cache hit for write at index %d, updated value: %d\n",
                                   threadId, i, instr.value);
                            #endif
                        } else if (node->cache[i].state == SHARED) {
                            // Need to get exclusive access
                            #ifdef DEBUG_INSTR
                            printf("Processor %d cache hit for write but in SHARED state, sending WRITE_REQUEST\n",
                                   threadId);
                            #endif
                            
                            // Send write request to home node
                            msg.type = WRITE_REQUEST;
                            msg.sender = threadId;
                            msg.address = instr.address;
                            msg.value = instr.value;
                            
                            sendMessage(procNodeAddr, msg);
                            waitingForReply = 1;
                            cacheHit = 1;
                        }
                        break;
                    }
                }
                
                if (!cacheHit) {
                    // Cache miss, need to fetch data and get exclusive access
                    #ifdef DEBUG_INSTR
                    printf("Processor %d cache miss for write, sending WRITE_REQUEST to %d\n",
                           threadId, procNodeAddr);
                    #endif
                    
                    // Check if we need to replace a cache line
                    if (cl->address != 0xFF && cl->address != instr.address) {
                        // Handle cache replacement
                        cacheLine oldCacheLine = *cl;
                        handleCacheReplacement(threadId, oldCacheLine);
                    }
                    
                    // Send write request to home node
                    msg.type = WRITE_REQUEST;
                    msg.sender = threadId;
                    msg.address = instr.address;
                    msg.value = instr.value;
                    
                    sendMessage(procNodeAddr, msg);
                    waitingForReply = 1;
                }
            }
        }
    }
    
    // Cleanup locks
    for (int i = 0; i < NUM_PROCS; i++) {
        omp_destroy_lock(&msgBufferLocks[i]);
    }
    
    return EXIT_SUCCESS;
}

void sendMessage(int receiver, message msg) {
    #ifdef DEBUG_MSG
    printf("Sending message to %d, type: %d, address: 0x%02X\n",
           receiver, msg.type, msg.address);
    #endif
    
    omp_set_lock(&msgBufferLocks[receiver]);
    int tail = messageBuffers[receiver].tail;
    messageBuffers[receiver].queue[tail] = msg;
    messageBuffers[receiver].tail = (tail + 1) % MSG_BUFFER_SIZE;
    messageBuffers[receiver].count++;
    omp_unset_lock(&msgBufferLocks[receiver]);
}

void handleCacheReplacement(int sender, cacheLine oldCacheLine) {
    if (oldCacheLine.state == INVALID || oldCacheLine.address == 0xFF) {
        return; // Nothing to do for invalid or empty lines
    }
    
    byte memBlockAddr = oldCacheLine.address & 0x0F;
    byte procNodeAddr = (oldCacheLine.address >> 4) & 0x0F;
    
    message msg;
    msg.sender = sender;
    msg.address = oldCacheLine.address;
    
    switch (oldCacheLine.state) {
        case EXCLUSIVE:
        case SHARED:
            // For EXCLUSIVE (clean) or SHARED, just inform home node to update directory
            msg.type = EVICT_SHARED;
            sendMessage(procNodeAddr, msg);
            break;
            
        case MODIFIED:
            // For MODIFIED, need to write back data
            msg.type = EVICT_MODIFIED;
            msg.value = oldCacheLine.value;
            sendMessage(procNodeAddr, msg);
            break;
            
        case INVALID:
            // Nothing to do
            break;
    }
}

void initializeProcessor(int threadId, processorNode *node, char *dirName) {
    for (int i = 0; i < MEM_SIZE; i++) {
        node->memory[i] = 20 * threadId + i;
        node->directory[i].bitVector = 0;
        node->directory[i].state = U;
    }

    for (int i = 0; i < CACHE_SIZE; i++) {
        node->cache[i].address = 0xFF;
        node->cache[i].value = 0;
        node->cache[i].state = INVALID;
    }

    char filename[128];
    snprintf(filename, sizeof(filename), "tests/%s/core_%d.txt", dirName, threadId);
    FILE *file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "Error: could not open file %s\n", filename);
        exit(EXIT_FAILURE);
    }

    char line[20];
    node->instructionCount = 0;
    while (fgets(line, sizeof(line), file) && node->instructionCount < MAX_INSTR_NUM) {
        if (line[0] == 'R' && line[1] == 'D') {
            sscanf(line, "RD %hhx", &node->instructions[node->instructionCount].address);
            node->instructions[node->instructionCount].type = 'R';
            node->instructions[node->instructionCount].value = 0;
        } else if (line[0] == 'W' && line[1] == 'R') {
            sscanf(line, "WR %hhx %hhu", 
                  &node->instructions[node->instructionCount].address,
                  &node->instructions[node->instructionCount].value);
            node->instructions[node->instructionCount].type = 'W';
        }
        node->instructionCount++;
    }

    fclose(file);
}

void printProcessorState(int processorId, processorNode node) {
    static const char *cacheStateStr[] = { "MODIFIED", "EXCLUSIVE", "SHARED", "INVALID" };
    static const char *dirStateStr[] = { "EM", "S", "U" };

    #pragma omp critical
    {
        printf("=======================================\n");
        printf(" Processor Node: %d\n", processorId);
        printf("=======================================\n\n");

        printf("-------- Memory State --------\n");
        printf("| Index | Address |   Value  |\n");
        printf("|----------------------------|\n");
        for (int i = 0; i < MEM_SIZE; i++) {
            printf("|  %3d  |  0x%02X   |  %5d   |\n", i, (processorId << 4) + i, node.memory[i]);
        }
        printf("------------------------------\n\n");

        printf("------------ Directory State ---------------\n");
        printf("| Index | Address | State |    BitVector   |\n");
        printf("|------------------------------------------|\n");
        for (int i = 0; i < MEM_SIZE; i++) {
            printf("|  %3d  |  0x%02X   |  %2s   |   0x%08B   |\n", i,
                  (processorId << 4) + i, dirStateStr[node.directory[i].state],
                  node.directory[i].bitVector);
        }
        printf("--------------------------------------------\n\n");
        
        printf("------------ Cache State ----------------\n");
        printf("| Index | Address | Value |    State    |\n");
        printf("|---------------------------------------|\n");
        for (int i = 0; i < CACHE_SIZE; i++) {
            printf("|  %3d  |  0x%02X   |  %3d  |  %8s \t|\n", 
                   i, node.cache[i].address, node.cache[i].value,
                   cacheStateStr[node.cache[i].state]);
        }
        printf("----------------------------------------\n\n");
    }
}