#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <vector>

#include "cort_tcp_listener.h"

#define RETURN_ERROR(x) do{ \
	set_errno(x); \
	return (x); \
}while(false)

cort_tcp_listener::cort_tcp_listener(){
	backlog = 0;
	listen_port = 0;
	setsockopt_arg.data = 0;
	errnum = 0;
}

cort_tcp_listener::~cort_tcp_listener(){
	stop_listen();
}

void cort_tcp_listener::stop_listen(){
	close_cort_fd();
}

void cort_tcp_listener::pause_accept(){
	set_poll_request(EPOLLOUT); //We do not remove the listen fd because it may lead to epoll fd does not wait anything.
}

void cort_tcp_listener::resume_accept(){
	set_poll_request(EPOLLIN);
}

uint8_t cort_tcp_listener::listen_connect(){
	if(listen_port == 0){
		RETURN_ERROR(cort_socket_error_codes::SOCKET_INVALID_LISTEN_ADDRESS);
	}
	
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0){
		RETURN_ERROR(cort_socket_error_codes::SOCKET_CREATE_ERROR);
    }

    int flag = fcntl(sockfd, F_GETFL);
	if (-1 == flag || fcntl(sockfd, F_SETFL, flag | O_NONBLOCK) == -1){
		close(sockfd);
		RETURN_ERROR(cort_socket_error_codes::SOCKET_CREATE_ERROR);
	}
	flag = 1;
	if(setsockopt_arg._.disable_reuse_address == 0){
		setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
	}
	
	if(setsockopt_arg._.enable_accept_after_recv != 0){
		setsockopt(sockfd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &flag, sizeof(flag));
	}
	
	struct sockaddr_in bindaddr;
	bindaddr.sin_port = htons(listen_port);
	bindaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	bindaddr.sin_family = AF_INET;
	
	if (bind(sockfd, (struct sockaddr *) &bindaddr, sizeof(bindaddr)) < 0) {
		close(sockfd);
		RETURN_ERROR(cort_socket_error_codes::SOCKET_BIND_ERROR);
	}
	
	if(backlog == 0){
		backlog = 128;
	}
	
	int result;
	result = listen(sockfd, backlog);
	if(result < 0){
		close(sockfd);
		RETURN_ERROR(cort_socket_error_codes::SOCKET_LISTEN_ERROR);
	}
	set_cort_fd(sockfd);
	set_poll_request(EPOLLIN);
	return 0;
}

cort_proto* cort_tcp_listener::start(){
	CO_BEGIN
		if(get_cort_fd() < 0 && listen_connect() != 0){
			set_errno(cort_socket_error_codes::SOCKET_LISTEN_ERROR);
			CO_RETURN;
		}
		CO_YIELD();
		if(is_timeout_or_stopped()){
			set_errno(cort_socket_error_codes::SOCKET_LISTEN_ERROR);
			stop_listen();
			CO_RETURN;	
		}
		uint32_t poll_event = get_poll_result();
		if( (EPOLLIN & poll_event) == 0){
			set_errno(cort_socket_error_codes::SOCKET_CONNECT_REJECTED);
			stop_listen();
			CO_RETURN;	
		}
		
		int listen_fd = get_cort_fd();	
        const int max_accept_one_loop = 256;
        cort_accept_result_t accept_result[max_accept_one_loop];
        socklen_t addrlen = sizeof(accept_result->servaddr);
        int current_connection = 0;
        int thread_errno = 0;
    start_accept:
    for(; current_connection<max_accept_one_loop; ++current_connection){
        int &accept_fd = accept_result[current_connection].accept_fd;
        sockaddr_in& servaddr = accept_result[current_connection].servaddr;
		#if !defined(__linux__)
		accept_fd = accept(listen_fd, (struct sockaddr*)&servaddr, &addrlen);
		if(accept_fd > 0){
			int flag = fcntl(accept_fd, F_GETFL);
			if (-1 == flag || fcntl(accept_fd, F_SETFL, flag | O_NONBLOCK) == -1){
				close(accept_fd);
				break;
			}
			if(setsockopt_arg._.disable_no_delay == 0){
				int flag = 1;
				setsockopt(accept_fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(flag) );
			}
			if(setsockopt_arg._.enable_close_by_reset > 0){
				linger lin;
				lin.l_onoff = 1;
				uint8_t flag = setsockopt_arg._.enable_close_by_reset;
				lin.l_linger = (flag == 1? 0:flag);
				setsockopt(accept_fd, SOL_SOCKET, SO_LINGER,(&lin), sizeof(lin));  
			}			
		}
		#else
		accept_fd = accept4(listen_fd, (struct sockaddr*)&servaddr, &addrlen, SOCK_NONBLOCK);
		if(accept_fd > 0){
			if(setsockopt_arg._.disable_no_delay == 0){
				int flag = 1;
				setsockopt(accept_fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(flag) );
			}
			if(setsockopt_arg._.enable_close_by_reset > 0){
				linger lin;
				lin.l_onoff = 1;
				uint8_t flag = setsockopt_arg._.enable_close_by_reset;
				lin.l_linger = (flag == 1? 0:flag);
				setsockopt(accept_fd, SOL_SOCKET, SO_LINGER,(&lin), sizeof(lin));  
			}
		}
		#endif
        else{
            thread_errno = errno;
            break;
        }
	}
        while(current_connection != 0){
            --current_connection;
            int &accept_fd = accept_result[current_connection].accept_fd;
            sockaddr_in& servaddr = accept_result[current_connection].servaddr;
            ctrler_creator(accept_fd, servaddr.sin_addr.s_addr, servaddr.sin_port, 0, (setsockopt_arg._.enable_accept_after_recv ? EPOLLIN : 0)); //Yes you can read now!
        }
        
		if (thread_errno == EINTR) {
            thread_errno = 0;
			goto start_accept;
		}
        if(thread_errno == 0){
            goto start_accept;
        }
		if (thread_errno != EMFILE && thread_errno != ENFILE && thread_errno != ENOBUFS && thread_errno != ENOMEM ){
			CO_AGAIN;
		}
        //We can not accept so we sleep 100ms
        pause_accept();
		CO_SLEEP(1);
        resume_accept();
        CO_PREV;  
	CO_END
}

static void remove_keep_alive(cort_tcp_server_waiter* tcp_cort){
	uint32_t result = tcp_cort->get_poll_result();
	if(result == 0 || (((EPOLLRDHUP|EPOLLERR) & result) != 0)){//timeout
		tcp_cort->close_cort_fd();
		tcp_cort->release();
	}
	else{
		tcp_cort->clear();	
		tcp_cort->remove_ref();
		tcp_cort->ctrler_creator(tcp_cort->get_cort_fd(), tcp_cort->ip_v4, tcp_cort->port_v4, tcp_cort, result);
	}
}

static cort_proto* on_connection_keepalive_timeout_or_readable_server(cort_proto* arg){
	cort_tcp_server_waiter* tcp_cort = (cort_tcp_server_waiter*)arg;
	remove_keep_alive(tcp_cort);
	return 0;
}

void cort_tcp_server_waiter::keep_alive(uint32_t keep_alive_time, uint32_t /* ip_arg */, uint16_t /* port_arg */, uint16_t /* type_key_arg */){
	this->set_parent(0);
	this->set_timeout(keep_alive_time);
	this->set_callback_function(on_connection_keepalive_timeout_or_readable_server);
	this->add_ref();
	this->set_poll_request(EPOLLIN | EPOLLRDHUP);
	this->clear_poll_result();
}
