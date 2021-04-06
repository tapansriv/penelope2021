#define _GNU_SOURCE
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <czmq.h>
#include <errno.h>
#include "power_client.h"
#include "ctx.h"

void *server_thread(void *args);

void *server_thread(void *args)
{
    printf("starting server\n");
    server_ctx_t *data = (server_ctx_t *)args;
    void *responder = zmq_socket(data->ctx->context, ZMQ_REP);
    int rc = zmq_bind(responder, "tcp://*:1600");
    assert(rc == 0);

    // zmq_pollitem_t items[1];
    // items[0].socket = responder;
    // items[0].events = ZMQ_POLLIN;

    zmq_pollitem_t items[] = {
        {responder, 0, ZMQ_POLLIN, 0}
    };

    cpu_t *cpu_list = data->ctx->cpus;
    while (!data->ctx->dying) 
    {
        zmq_poll(items, 1, 1000);
        if (items[0].revents & ZMQ_POLLIN)
        {
            zmq_msg_t req_msg, resp_msg;
            zmq_msg_init(&req_msg);

            if (zmq_msg_recv(&req_msg, responder, 0) == -1) 
            {
                exit(-1);
            }
            power_exchange_msg_t *request = (power_exchange_msg_t *)zmq_msg_data(&req_msg);
            zmq_msg_close(&req_msg);
            double power_to_give = 0.0f;
            for (int i = 0; i < 2; i++)
            {
                pthread_mutex_lock(&cpu_list[i].lock);
                if (cpu_list[i].available_power > 0)
                {
                    // power_exchanged initially represents the necessary power (proxy for urgency)
                    double max_size = max_transaction_amount(cpu_list[i].available_power, request->power_exchanged);
                    double delta_power = (cpu_list[i].available_power < max_size) ? cpu_list[i].available_power : max_size;
                    power_to_give += delta_power;
                    cpu_list[i].available_power -= delta_power;
                }
                else 
                {
                    if (request->urgency && !cpu_list[i].urgency)
                    {
                        cpu_list[i].release_power = true;
                    }
                }
                pthread_mutex_unlock(&cpu_list[i].lock);
            }
            power_exchange_msg_t response = {power_to_give, 0};
            zmq_msg_init_data(&resp_msg, (void*)(&response), sizeof(response), NULL, NULL);
            int n = zmq_msg_send(&resp_msg, responder, 0); assert(n == sizeof(response));
            #ifdef VERBOSE
            printf("POWER POOL gave %fW\n", power_to_give);
            printf("URGENCY RECEIVED: %d\n", request->urgency);
            #endif
        }
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc < 5) 
    {
        fprintf(stderr, "not enough arguments provided\n");
        exit(-1);
    }

    char *hostfilename = argv[1];
    int num_hosts = atoi(argv[2]);
    double powercap = atof(argv[3]);
    char *cur_host = argv[4]; // IP of current node. to ensure node doesn't make spurious requests to itself

    char **hosts = malloc(sizeof(char*) * (num_hosts - 1));
    int count = 0;
    FILE *hostfile = fopen(hostfilename, "r");
    if (hostfile == NULL)
    {
        fprintf(stderr, "failed to open hostfile\n");
        exit(-1);
    }

    size_t n;
    while ((count < (num_hosts - 1)) && (getline(&hosts[count], &n, hostfile) != -1))
    {
        int len = strlen(hosts[count]);
        if (hosts[count][len-1] == '\n')
            hosts[count][len-1] = '\0';
        if (strcmp(hosts[count], cur_host))
        {
            count++;
        }
    } 
    if (count != (num_hosts - 1))
    {
        fprintf(stderr, "Issue reading in hosts from file\n");
        exit(-1);
    }

    printf("hello world\n");

    power_ctx_t *power_ctx = init_power_ctx(2);
    shared_ctx_t *shared_ctx = init_shared();

    if (pthread_create(&shared_ctx->power_reader, NULL, read_power, (void*)power_ctx) != 0)
    {
        fprintf(stderr, "failed to create server thread\n");
        exit(-1);
    }

    void *wait_for_power_pool = zmq_socket(shared_ctx->context, ZMQ_REP);
    int rc = zmq_bind(wait_for_power_pool, "tcp://*:1111");
    assert(rc == 0);
    // char start[15];
    // int nbytes = zmq_recv(wait_for_power_pool, (void*)start, 15, 0);
    // if (nbytes == -1)
    // {
    //     fprintf(stderr, "error waiting on signal socket used to start power pool\n");
    //     exit(-1);
    // }
    // zmq_send(wait_for_power_pool, "ack", 3, 0);
    // start[nbytes] = '\0';
    // double powercap = atof(start);

    printf("beginning power pool\n");
    update_powercap(shared_ctx, powercap);

    server_ctx_t *server_ctx = init_server(shared_ctx);
    client_ctx_t *client_ctx = init_client_ctx(shared_ctx, power_ctx, hosts, cur_host, (num_hosts-1));

    if (pthread_create(&shared_ctx->server, NULL, server_thread, (void *)server_ctx) != 0)
    {
        fprintf(stderr, "failed to create server thread\n");
        exit(-1);
    }
    if (pthread_create(&shared_ctx->client, NULL, client_thread, (void *)client_ctx) != 0)
    {
        fprintf(stderr, "failed to create server thread\n");
        exit(-1);
    }

    char end[15];
    if (zmq_recv(wait_for_power_pool, (void*)end, 15, 0) == -1)
    {
        fprintf(stderr, "error waiting on signal socket used to kill power pool\n");
        exit(-1);
    }
    zmq_send(wait_for_power_pool, "ack", 3, 0);

    shared_ctx->dying = true;
    power_ctx->dying = true;

    pthread_join(shared_ctx->power_reader, NULL);
    pthread_join(shared_ctx->server, NULL);
    pthread_join(shared_ctx->client, NULL);

    return 0;
}
