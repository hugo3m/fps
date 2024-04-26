/*
    FPS server XDP program (Userspace)

    Runs on Ubuntu 22.04 LTS 64bit with Linux Kernel 6.5+ *ONLY*
*/

#define _GNU_SOURCE

#include <memory.h>
#include <stdio.h>
#include <signal.h>
#include <errno.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <xdp/libxdp.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <inttypes.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include "shared.h"
#include "map.h"

struct bpf_t
{
    int interface_index;
    struct xdp_program * program;
    bool attached_native;
    bool attached_skb;
    int counters_fd;
    int server_stats_fd;
    int input_buffer_fd;
    int player_state_outer_fd;
    int player_state_inner_fd[MAX_CPUS];
    struct perf_buffer * input_buffer;
};

static uint64_t inputs_processed[MAX_CPUS];
static uint64_t inputs_lost[MAX_CPUS];

static struct map_t * cpu_player_map[MAX_CPUS];

void process_input( void * ctx, int cpu, void * data, unsigned int data_sz )
{
    struct bpf_t * bpf = (struct bpf_t*) ctx;

    struct input_header * header = (struct input_header*) data;

    printf( "process input for %" PRIx64 " on cpu %d\n", (uint64_t)header->session_id, cpu );

    struct input_data * input = (struct input_data*) data + sizeof(struct input_header);

    struct player_state * state = map_get( cpu_player_map[cpu], header->session_id );
    if ( !state )
    {
        printf( "first player update for session %" PRIx64 "\n", header->session_id );

        // first player update
        state = malloc( sizeof(struct player_state) );
        map_set( cpu_player_map[cpu], header->session_id, state );
    }

    // todo: handle multiple inputs

    state->t += input->dt;

    for ( int i = 0; i < PLAYER_STATE_SIZE; i++ )
    {
        state->data[i] = (uint8_t) state->t + (uint8_t) i;
    }

    int player_state_fd = bpf->player_state_inner_fd[cpu];
    int err = bpf_map_update_elem( player_state_fd, &header->session_id, state, BPF_ANY );
    if ( err != 0 )
    {
        printf( "error: failed to update player state: %s\n", strerror(errno) );
        return;
    }

    __sync_fetch_and_add( &inputs_processed[cpu], 1 );
}

void lost_input( void * ctx, int cpu, __u64 count )
{
    __sync_fetch_and_add( &inputs_lost[cpu], count );
}

static double time_start;

void platform_init()
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC_RAW, &ts );
    time_start = ts.tv_sec + ( (double) ( ts.tv_nsec ) ) / 1000000000.0;
}

double platform_time()
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC_RAW, &ts );
    double current = ts.tv_sec + ( (double) ( ts.tv_nsec ) ) / 1000000000.0;
    return current - time_start;
}

void platform_sleep( double time )
{
    usleep( (int) ( time * 1000000 ) );
}

int bpf_init( struct bpf_t * bpf, const char * interface_name )
{
    // we can only run xdp programs as root

    if ( geteuid() != 0 ) 
    {
        printf( "\nerror: this program must be run as root\n\n" );
        return 1;
    }

    // find the network interface that matches the interface name
    {
        bool found = false;

        struct ifaddrs * addrs;
        if ( getifaddrs( &addrs ) != 0 )
        {
            printf( "\nerror: getifaddrs failed\n\n" );
            return 1;
        }

        for ( struct ifaddrs * iap = addrs; iap != NULL; iap = iap->ifa_next ) 
        {
            if ( iap->ifa_addr && ( iap->ifa_flags & IFF_UP ) && iap->ifa_addr->sa_family == AF_INET )
            {
                struct sockaddr_in * sa = (struct sockaddr_in*) iap->ifa_addr;
                if ( strcmp( interface_name, iap->ifa_name ) == 0 )
                {
                    printf( "found network interface: '%s'\n", iap->ifa_name );
                    bpf->interface_index = if_nametoindex( iap->ifa_name );
                    if ( !bpf->interface_index ) 
                    {
                        printf( "\nerror: if_nametoindex failed\n\n" );
                        return 1;
                    }
                    found = true;
                    break;
                }
            }
        }

        freeifaddrs( addrs );

        if ( !found )
        {
            printf( "\nerror: could not find any network interface matching '%s'\n\n", interface_name );
            return 1;
        }
    }

    // initialize platform

    platform_init();

    // load the server_xdp program and attach it to the network interface

    printf( "loading server_xdp...\n" );

    bpf->program = xdp_program__open_file( "server_xdp.o", "server_xdp", NULL );
    if ( libxdp_get_error( bpf->program ) ) 
    {
        printf( "\nerror: could not load server_xdp program\n\n");
        return 1;
    }

    printf( "server_xdp loaded successfully.\n" );

    printf( "attaching server_xdp to network interface\n" );

    int ret = xdp_program__attach( bpf->program, bpf->interface_index, XDP_MODE_NATIVE, 0 );
    if ( ret == 0 )
    {
        bpf->attached_native = true;
    } 
    else
    {
        printf( "falling back to skb mode...\n" );
        ret = xdp_program__attach( bpf->program, bpf->interface_index, XDP_MODE_SKB, 0 );
        if ( ret == 0 )
        {
            bpf->attached_skb = true;
        }
        else
        {
            printf( "\nerror: failed to attach server_xdp program to interface\n\n" );
            return 1;
        }
    }

    // bump rlimit

    struct rlimit rlim_new = {
        .rlim_cur   = RLIM_INFINITY,
        .rlim_max   = RLIM_INFINITY,
    };

    if ( setrlimit( RLIMIT_MEMLOCK, &rlim_new ) ) 
    {
        printf( "\nerror: could not increase RLIMIT_MEMLOCK limit!\n\n" );
        return 1;
    }

    // get the file handle to counters

    bpf->counters_fd = bpf_obj_get( "/sys/fs/bpf/counters_map" );
    if ( bpf->counters_fd <= 0 )
    {
        printf( "\nerror: could not get counters: %s\n\n", strerror(errno) );
        return 1;
    }

    // get the file handle to the server stats

    bpf->server_stats_fd = bpf_obj_get( "/sys/fs/bpf/server_stats" );
    if ( bpf->server_stats_fd <= 0 )
    {
        printf( "\nerror: could not get server stats: %s\n\n", strerror(errno) );
        return 1;
    }

    // get the file handle to the outer player state map

    bpf->player_state_outer_fd = bpf_obj_get( "/sys/fs/bpf/player_state_map" );
    if ( bpf->player_state_outer_fd <= 0 )
    {
        printf( "\nerror: could not get outer player state map: %s\n\n", strerror(errno) );
        return 1;
    }

    // get the file handle to the inner player state maps

    for ( int i = 0; i < MAX_CPUS; i++ )
    {
        uint32_t key = i;
        uint32_t inner_map_id = 0;
        int result = bpf_map_lookup_elem( bpf->player_state_outer_fd, &key, &inner_map_id );
        if ( result != 0 )
        {
            printf( "\nerror: failed lookup player state inner map: %s\n\n", strerror(errno) );
            return 1;
        }
        bpf->player_state_inner_fd[i] = bpf_map_get_fd_by_id( inner_map_id );
    }

    // get the file handle to the input buffer

    bpf->input_buffer_fd = bpf_obj_get( "/sys/fs/bpf/input_buffer" );
    if ( bpf->input_buffer_fd <= 0 )
    {
        printf( "\nerror: could not get input buffer: %s\n\n", strerror(errno) );
        return 1;
    }

    // create the input perf buffer

    struct perf_buffer_opts opts;
    memset( &opts, 0, sizeof(opts) );
    opts.sz = sizeof(opts);
    opts.sample_period = 1; // 1000;
    bpf->input_buffer = perf_buffer__new( bpf->input_buffer_fd, 131072, process_input, lost_input, bpf, &opts );
    if ( libbpf_get_error( bpf->input_buffer ) ) 
    {
        printf( "\nerror: could not create input perf buffer\n\n" );
        return 1;
    }

    printf( "ready\n" );

    return 0;
}

void bpf_shutdown( struct bpf_t * bpf )
{
    assert( bpf );

    if ( bpf->program != NULL )
    {
        if ( bpf->attached_native )
        {
            xdp_program__detach( bpf->program, bpf->interface_index, XDP_MODE_NATIVE, 0 );
        }
        if ( bpf->attached_skb )
        {
            xdp_program__detach( bpf->program, bpf->interface_index, XDP_MODE_SKB, 0 );
        }
        xdp_program__close( bpf->program );
    }
}

static struct bpf_t bpf;

volatile bool quit;

void interrupt_handler( int signal )
{
    (void) signal; quit = true;
}

void clean_shutdown_handler( int signal )
{
    (void) signal;
    quit = true;
}

static void cleanup()
{
    bpf_shutdown( &bpf );
    fflush( stdout );
}

int pin_thread_to_cpu( int cpu ) 
{
    int num_cpus = sysconf(_SC_NPROCESSORS_ONLN );
    if ( cpu < 0 || cpu >= num_cpus  )
        return EINVAL;

    cpu_set_t cpuset;
    CPU_ZERO( &cpuset );
    CPU_SET( cpu, &cpuset );

    pthread_t current_thread = pthread_self();    

    return pthread_setaffinity_np( current_thread, sizeof(cpu_set_t), &cpuset );
}

int main( int argc, char *argv[] )
{
    signal( SIGINT,  interrupt_handler );
    signal( SIGTERM, clean_shutdown_handler );
    signal( SIGHUP,  clean_shutdown_handler );

    if ( argc != 2 )
    {
        printf( "\nusage: server <interface name>\n\n" );
        return 1;
    }

    for ( int i = 0; i < MAX_CPUS; i++ )
    {
        cpu_player_map[i] = map_create();
    }

    const char * interface_name = argv[1];

    if ( bpf_init( &bpf, interface_name ) != 0 )
    {
        cleanup();
        return 1;
    }

    // main loop

    pin_thread_to_cpu( MAX_CPUS );       // IMPORTANT: keep the main thread out of the way of the XDP cpus on google cloud [0,15]

    unsigned int num_cpus = libbpf_num_possible_cpus();

    double last_print_time = platform_time();

    uint64_t previous_processed_inputs = 0;
    uint64_t previous_lost_inputs = 0;

    while ( !quit )
    {
        // poll perf buffer to drive input processing

        int err = perf_buffer__poll( bpf.input_buffer, 1 );
        if ( err == -4 )
        {
            // ctrl-c
            quit = true;
            break;
        }
        if ( err < 0 ) 
        {
            printf( "\nerror: could not poll input buffer: %d\n\n", err );
            quit = true;
            break;
        }

        // print out stats every second

        double current_time = platform_time();

        if ( last_print_time + 1.0 <= current_time )
        {
            // track processed and lost inputs

            uint64_t current_processed_inputs = 0;
            uint64_t current_lost_inputs = 0;
            for ( int i = 0; i < MAX_CPUS; i++ )
            {
                current_processed_inputs += inputs_processed[i];
                current_lost_inputs += inputs_lost[i];
            }
            uint64_t input_delta = current_processed_inputs - previous_processed_inputs;
            uint64_t lost_delta = current_lost_inputs - previous_lost_inputs;
            printf( "input delta: %" PRId64 ", lost delta: %" PRId64 "\n", input_delta, lost_delta );
            previous_processed_inputs = current_processed_inputs;
            previous_lost_inputs = current_lost_inputs;
            last_print_time = current_time;

            // track player state packets sent

            struct counters values[num_cpus];

            int key = 0;
            if ( bpf_map_lookup_elem( bpf.counters_fd, &key, values ) != 0 ) 
            {
                printf( "\nerror: could not look up counters: %s\n\n", strerror( errno ) );
                quit = true;
                break;
            }

            uint64_t player_state_packets_sent = 0;

            for ( int i = 0; i < MAX_CPUS; i++ )
            {
                player_state_packets_sent += values[i].player_state_packets_sent;
            }        

            // upload stats to the xdp program

            struct server_stats stats;
            stats.inputs_processed = current_processed_inputs;
            stats.player_state_packets_sent = player_state_packets_sent;

            int err = bpf_map_update_elem( bpf.server_stats_fd, &key, &stats, BPF_ANY );
            if ( err != 0 )
            {
                printf( "\nerror: failed to update server stats: %s\n\n", strerror(errno) );
                quit = true;
                break;
            }
        }
    }

    cleanup();

    printf( "\n" );

    return 0;
}
