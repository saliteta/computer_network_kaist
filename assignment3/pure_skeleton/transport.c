/*
 * transport.c 
 *
 * CS244a HW#3 (Reliable Transport)
 *
 * This file implements the STCP layer that sits between the
 * mysocket and network layers. You are required to fill in the STCP
 * functionality in this file. 
 *
 */



#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <arpa/inet.h>
#include <stddef.h>
#include "mysock.h"
#include "stcp_api.h"
#include "transport.h"


enum  { CSTATE_ESTABLISHED, CSTATE_LISTEN, CSTATE_SYN_SENT, CSTATE_SYN_RECV, 
                    CSTATE_FIN_WAIT_1, CSTATE_FIN_WAIT_2, CSTATE_TIME_WAIT, 
                    CSTATE_CLOSE_WAIT, CSTATE_LAST_ACK, CSTATE_CLOSED};    /* obviously you should have more states */

int MAX_BUFFER_LENGTH = 516;

/* this structure is global to a mysocket descriptor */
typedef struct
{
    bool_t done;    /* TRUE once connection is closed */

    int connection_state;   /* state of the connection (established, etc.) */
    tcp_seq initial_sequence_num;
    tcp_seq th_seq;
    tcp_seq th_ack;
    uint8_t th_flags;
    /* any other connection-wide global variables go here */
} context_t;


static void generate_initial_seq_num(context_t *ctx);                    // make the sequence number to be one
static void control_loop(mysocket_t sd, context_t *ctx);                 // in the main transfer and recv

void  network_recv_error_handing();
void application_recv_error_handing();
void create_tcp_header(mysocket_t sd, context_t ctx, struct tcphdr* tcpHeader);
void send_tcp_segment(mysocket_t sd, char* payload, context_t *ctx);
uint16_t calculate_checksum(const void* data, size_t length);

void transport_init(mysocket_t sd, bool_t is_active)
{
    context_t *ctx;

    ctx = (context_t *) calloc(1, sizeof(context_t));
    assert(ctx);

    generate_initial_seq_num(ctx);

    /* XXX: you should send a SYN packet here if is_active, or wait for one
     * to arrive if !is_active.  after the handshake completes, unblock the
     * application with stcp_unblock_application(sd).  you may also use
     * this to communicate an error condition back to the application, e.g.
     * if connection fails; to do so, just set errno appropriately (e.g. to
     * ECONNREFUSED, etc.) before calling the function.
     */

    // we need to do three way hand shaking, we always need a header field

    if(is_active){
        // if is active, it means this end is a sender, we need to first send SYN and ACK
        // then we need to start three way hand shaking by sending the first TCP data gram
        ctx->th_seq = ctx->initial_sequence_num;
        ctx->th_flags = TH_SYN;
        send_tcp_segment(sd, NULL, ctx);
        ctx->connection_state = CSTATE_SYN_SENT;
        ctx->th_seq++;                     // increase by one since I transfer 0 byte


        // we need to wait for last info in three way hand shaking
        unsigned int event;
        event = stcp_wait_for_event(sd, ANY_EVENT, NULL);
        if(NETWORK_DATA & event){
            char buffer[MAX_BUFFER_LENGTH];            
            ssize_t byteRecv = stcp_network_recv(sd, buffer, MAX_BUFFER_LENGTH);
            if (byteRecv<0){
                network_recv_error_handing();
            }
            const struct tcphdr* tcpHeader = (struct tcphdr*)buffer;
            ctx->th_ack = ntohl(tcpHeader->th_seq)+1; // increase the payload size, since payload is 0, payload increase by one
            ctx->th_flags = TH_ACK;
            send_tcp_segment(sd, NULL, ctx); 
            ctx->th_seq++; // increase by since transfered payload is
        }
    }
    else{
        // if is not active, we should wait for SYN here 
        unsigned int event;
        event = stcp_wait_for_event(sd, ANY_EVENT, NULL);
        if(NETWORK_DATA & event){
            char buffer[MAX_BUFFER_LENGTH];            
            ssize_t byteRecv = stcp_network_recv(sd, buffer, MAX_BUFFER_LENGTH);
            if (byteRecv<0){
                network_recv_error_handing();
            }
            struct tcphdr* tcpHeader = (struct tcphdr*)buffer;
            ctx->th_ack = ntohl(tcpHeader->th_seq)+1; // increase the payload size, since payload is 0, payload increase by one
            ctx->th_flags = TH_SYN;
            ctx->th_seq = 0; //initialize my sequence number
            send_tcp_segment(sd, NULL, ctx);
            ctx->th_seq++;
            ctx->connection_state = CSTATE_SYN_RECV;
        }
        
        unsigned int event2;
        event2 = stcp_wait_for_event(sd, ANY_EVENT, NULL);
        // then we wait for another incoming message from ACTIVE PEER
        if(NETWORK_DATA & event2){
            // another in comming message to prove client is alive
            char buffer[MAX_BUFFER_LENGTH];            
            ssize_t byteRecv = stcp_network_recv(sd, buffer, MAX_BUFFER_LENGTH);
            if (byteRecv<0){
                network_recv_error_handing();
            }
            struct tcphdr* tcpHeader = (struct tcphdr*)buffer;
            ctx->th_ack = ntohl(tcpHeader->th_seq)+1;
            ctx->th_seq ++;
            send_tcp_segment(sd, NULL, ctx);    //send tcp header with payload

        }
    }

    ctx->connection_state = CSTATE_ESTABLISHED;


    stcp_unblock_application(sd);
    control_loop(sd, ctx);

    /* do any cleanup here */
    free(ctx);
}


/* generate initial sequence number for an STCP connection */
static void generate_initial_seq_num(context_t *ctx)
{
    assert(ctx);
    ctx->initial_sequence_num = 1;
}


/* control_loop() is the main STCP loop; it repeatedly waits for one of the
 * following to happen:
 *   - incoming data from the peer
 *   - new data from the application (via mywrite())
 *   - the socket to be closed (via myclose())
 *   - a timeout
 */
static void control_loop(mysocket_t sd, context_t *ctx)
{
    assert(ctx);

    while (!ctx->done)
    {
        // we need to maintain the following argument
        // tcp_seq, tcp_ack
        // checksum
        // check sum can be calculated each time, let's ignore it for now

        unsigned int event;

        /* see stcp_api.h or stcp_api.c for details of this function */
        /* XXX: you will need to change some of these arguments! */
        event = stcp_wait_for_event(sd, ANY_EVENT, NULL);

        /* check whether it was the network, app, or a close request */
        if (event & APP_DATA)
        {
            /* the application has requested that data be sent */
            /* see stcp_app_recv() */
            char payload[MAX_BUFFER_LENGTH];  
            memset(payload, 0, sizeof(payload));          
            ssize_t byteRecv = stcp_app_recv(sd, payload, MAX_BUFFER_LENGTH);
            if (byteRecv<0){
                application_recv_error_handing();
            }
            ctx->th_flags = TH_ACK;
            payload[byteRecv] = '\0';
            send_tcp_segment(sd, payload, ctx);    //send tcp header with payload
            ctx->th_seq += byteRecv;
        }
        else if(event & NETWORK_DATA)
        {
            // if get the info from network layer
            // first recv what is inside the network layer
            char buffer[MAX_BUFFER_LENGTH+20];            
            ssize_t byteRecv = stcp_network_recv(sd, buffer, MAX_BUFFER_LENGTH+20);
            struct tcphdr tcpHeader;
            memcpy(&tcpHeader, buffer, sizeof(struct tcphdr));
            if (byteRecv<0){
                network_recv_error_handing();
            }
            memcpy(&tcpHeader, buffer, sizeof(struct tcphdr));

            if (ctx->connection_state != CSTATE_ESTABLISHED){ 
                // close handling 
                // 1. FIN_WAIT_1
                // 2. FIN-WAIT_2
                // 3. CLOSE_WAIT // close wait should not recv another data, error handel
                // 4. LAST_ACK
                // 5. Timed_wait // should be handel outside loop, error handel
                if ((ctx->connection_state == CSTATE_FIN_WAIT_1) && (ctx->th_seq == ntohl(tcpHeader.th_ack) )){
                    //active close, and it recive the confirmation from the passive close
                    ctx->connection_state = CSTATE_FIN_WAIT_2;
                    ctx->th_ack++; // since we recv nothing we only increase by one
                }
                else if((ctx->connection_state == CSTATE_FIN_WAIT_2) && (tcpHeader.th_flags & TH_FIN)){
                    stcp_fin_received(sd);
                    ctx->connection_state = CSTATE_CLOSED;
                    // now we need to send ack back
                    ctx->th_ack++;
                    ctx->th_flags = TH_ACK;
                    send_tcp_segment(sd, NULL, ctx);    //send tcp header as ACK
                    ctx->th_seq++; // no payload still increasing one
                    ctx->done = TRUE;
                    // entering Time wait stage
                }
                
                else{
                    // error state handling
                }
            }


            else{
                if ((byteRecv - sizeof(struct tcphdr)) == 0){
                    if (tcpHeader.th_flags == TH_FIN){
                        stcp_fin_received(sd);
                        ctx->connection_state = CSTATE_CLOSE_WAIT;
                        ctx->th_flags = TH_ACK;
                        ctx->th_ack++; // we have already recieved ack, we need to increment by 1
                        send_tcp_segment(sd, NULL, ctx);    //send tcp header as ACK
                        ctx->th_seq++;
                        ctx->th_flags = TH_FIN;
                        ctx->th_ack++; // we have already recieved ack, we need to increment by 1
                        send_tcp_segment(sd, NULL, ctx);    //send tcp header as ACK
                        ctx->th_seq++;
                        ctx->connection_state = CSTATE_LAST_ACK;
                        event = stcp_wait_for_event(sd, ANY_EVENT, NULL);
                        char buffer[MAX_BUFFER_LENGTH+20];            
                        stcp_network_recv(sd, buffer, MAX_BUFFER_LENGTH+20);
                        struct tcphdr tcpHeader2;
                        memcpy(&tcpHeader2, buffer, sizeof(struct tcphdr));
                        ctx->connection_state = CSTATE_CLOSED;
                        ctx->done = TRUE;
                    }
                    else{
                        //ctx->th_ack++; // we have already recieved ack, we need to increment by 1
                    }
                    // we don't need to ack ack
                }
                else{ // if segment is not just an ACK segment
                    char payload[MAX_BUFFER_LENGTH - sizeof(struct tcphdr) + 1];
                    memset(payload, 0, sizeof(payload));
                    memcpy(payload, buffer + sizeof(struct tcphdr), byteRecv - sizeof(struct tcphdr));
                    // may need do checksum here
                    // there is no limit of stcp_app_send?
                    payload[byteRecv - sizeof(struct tcphdr)] = '\0';
                    stcp_app_send(sd, payload, strlen(payload));
                    // now we need to update our ack field in ctx 
                    ctx->th_ack += strlen(payload);
                    ctx->th_flags = TH_ACK;
                    // now we need to send ack back
                    send_tcp_segment(sd, NULL, ctx);    //send tcp header as ACK
                    //ctx->th_seq++; // no payload still increasing one
                }

            }

        }
        else if(event & APP_CLOSE_REQUESTED) 
        {
            //active close
            if(ctx->connection_state == CSTATE_ESTABLISHED){
                ctx->th_flags = TH_FIN;
                send_tcp_segment(sd, NULL, ctx);    //send tcp header as ACK
                ctx->th_seq++;
                ctx->connection_state = CSTATE_FIN_WAIT_1;
            }
            
            else{
                //state_error handeling
            }
        }
    }
}




void send_tcp_segment(mysocket_t sd, char* payload, context_t *ctx){
    // input is sd and payload and tcp transport
    struct tcphdr tcpHeader;
    struct sockaddr_in localAddr, remoteAddr;
    socklen_t addrLen = sizeof(struct sockaddr_in);
    socklen_t address_length = sizeof(struct sockaddr_in);


    // Get the local address and port number
    mygetsockname(sd, (struct sockaddr*)&localAddr, &address_length);
    uint16_t localPort = htons(localAddr.sin_port);
    // Get the remote address and port number
    mygetpeername(sd, (struct sockaddr*)&remoteAddr, &addrLen);
    uint16_t remotePort = htons(remoteAddr.sin_port);



    // Populate the TCP header fields
    tcpHeader.th_sport = localPort;
    tcpHeader.th_dport = remotePort;
    tcpHeader.th_seq = ntohl(ctx->th_seq); // Set the initial sequence number
    tcpHeader.th_ack = ntohl(ctx->th_ack); // Set the initial acknowledgment number
    tcpHeader.th_off = sizeof(struct tcphdr) / 4; // Header length in 32-bit words
    tcpHeader.th_flags = ctx->th_flags; // Set the SYN flag for connection initiation
    tcpHeader.th_win = ntohs(3072); // Set the window size
    tcpHeader.th_sum = 0; // Set the checksum (to be calculated later)
    tcpHeader.th_urp = ntohs(0); // Set the urgent pointer (unused in STCP)

    if (payload == NULL){
        size_t segment_len = sizeof(tcpHeader);
        void* segment = malloc(segment_len);
        memcpy(segment, &tcpHeader, sizeof(tcpHeader));

        if (segment == NULL) {
            // Error handling for memory allocation failure
            return;
        }

        // Copy the TCP header to the beginning of the segment memory block
        memcpy(segment, &tcpHeader, sizeof(tcpHeader));

        // Send the TCP segment to the network layer
        ssize_t bytesSent = stcp_network_send(sd, segment, segment_len, NULL);
        if (bytesSent == -1) {
            // Error handling for the network send operation
        }

        // Free the allocated memory for the TCP segment
        free(segment);

    }
    else{
        size_t segment_len = sizeof(tcpHeader) + strlen(payload);
        void* segment = malloc(segment_len);
        memset(segment, 0, segment_len);

        if (segment == NULL) {
            // Error handling for memory allocation failure
            return;
        }
        // Copy the TCP header to the beginning of the segment memory block
        memcpy(segment, &tcpHeader, sizeof(tcpHeader));

        // Copy the payload data after the TCP header in the segment memory block
        memcpy(segment + sizeof(tcpHeader), payload, strlen(payload));
        
        // Send the TCP segment to the network layer
        ssize_t bytesSent = stcp_network_send(sd, segment, segment_len, NULL);
        if (bytesSent == -1) {
            // Error handling for the network send operation
        }
    
        // Free the allocated memory for the TCP segment
        free(segment);
    }
}


void  network_recv_error_handing(){

}
void application_recv_error_handing(){

}




/**********************************************************************/
/* our_dprintf
 *
 * Send a formatted message to stdout.
 * 
 * format               A printf-style format string.
 *
 * This function is equivalent to a printf, but may be
 * changed to log errors to a file if desired.
 *
 * Calls to this function are generated by the dprintf amd
 * dperror macros in transport.h
 */
void our_dprintf(const char *format,...)
{
    va_list argptr;
    char buffer[1024];

    assert(format);
    va_start(argptr, format);
    vsnprintf(buffer, sizeof(buffer), format, argptr);
    va_end(argptr);
    fputs(buffer, stdout);
    fflush(stdout);
}