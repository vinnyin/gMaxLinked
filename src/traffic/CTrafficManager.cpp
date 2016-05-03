/*
 ============================================================================
 Name        : CTrafficManager.cpp
 Author      : Rafael Gu
 Version     : 1.0
 Copyright   : GPL
 Description :
 ============================================================================
 */

#include "CTrafficManager.h"
#include "../config/Config.h"
#include "CNode.h"
#include "CNodeGroup.h"

#include <linux/socket.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>

CTrafficManager *CTrafficManager::_tm = NULL;

CTrafficManager *CTrafficManager::instance() {
	if (!_tm) {
		_tm = new CTrafficManager();
	}

	return _tm;
}

void CTrafficManager::destory() {
	delete _tm;
	_tm = NULL;
}

CTrafficManager::CTrafficManager() :
		_res(Config::App::TOTAL_SUPPORT_USER_NUM), _worker(
				Config::App::THREAD_STACK_SIZE) {
	_groups = new CNodeGroup[Config::App::NODE_GROUP_NUM];

	_listenFd = 0;
	_epollFd = 0;

	_curGroupIndex = 0;

	_running = true;
}

CTrafficManager::~CTrafficManager() {
	_running = false;

	if (_listenFd) {
		close(_listenFd);
	}

	if (_epollFd) {
		close(_epollFd);
	}

	delete[] _groups;
}

void CTrafficManager::work() {
	// create socket to listen.
	// SOCK_NONBLOCK is not supported by less than glibc 2.9
	_listenFd = socket(AF_INET, SOCK_STREAM, 0);

	if (-1 == _listenFd) {
		log_fatal("CTrafficManager::work: failed to create listening socket");

		exit(0);
	}

	setNonBlocking(_listenFd);

	int optVal = 1;

	if (setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, &optVal, sizeof(int))) {
		log_fatal(
				"CTrafficManager::work: failed to setsockopt with SO_REUSEADDR");

		exit(0);
	}

	struct sockaddr_in serverAddr;

	memset(&serverAddr, 0, sizeof(struct sockaddr_in));
	serverAddr.sin_family = AF_INET;
	inet_aton(Config::App::LISTEN_IP, &serverAddr.sin_addr);
	serverAddr.sin_port = htons(Config::App::LISTEN_PORT);

	if (-1
			== bind(_listenFd, (struct sockaddr *) &serverAddr,
					sizeof(serverAddr))) {
		log_fatal("CTrafficManager::work: failed to bind listen socket");

		exit(0);
	}

	_epollFd = epoll_create(Config::App::TOTAL_SUPPORT_USER_NUM + 1);

	if (-1 == _epollFd) {
		log_fatal("CTrafficManager::work: failed to call epoll_create");

		exit(0);
	}

	// register listening fd into epoll.
	struct epoll_event ev;

	ev.events = EPOLLIN | EPOLLET;
	ev.data.fd = _listenFd;

	if (-1 == epoll_ctl(_epollFd, EPOLL_CTL_ADD, _listenFd, &ev)) {
		switch (errno) {
		case EBADF:
			log_fatal("CTrafficManager::work: failed to call epoll_ctrl-EBADF");
			break;
		case EEXIST:
			log_fatal(
					"CTrafficManager::work: failed to call epoll_ctrl-EEXIST");
			break;
		case EINVAL:
			log_fatal(
					"CTrafficManager::work: failed to call epoll_ctrl-EINVAL");
			break;
		case ENOENT:
			log_fatal(
					"CTrafficManager::work: failed to call epoll_ctrl-ENOENT");
			break;
		case ENOMEM:
			log_fatal(
					"CTrafficManager::work: failed to call epoll_ctrl-ENOMEM");
			break;
		case ENOSPC:
			log_fatal(
					"CTrafficManager::work: failed to call epoll_ctrl-ENOSPC");
			break;
		case EPERM:
			log_fatal("CTrafficManager::work: failed to call epoll_ctrl-EPERM");
			break;
		default:
			log_fatal(
					"CTrafficManager::work: failed to call epoll_ctrl-Unknown.");
		}

		exit(0);
	}

	// start to listen.
	if (-1 == listen(_listenFd, Config::App::EPOLL_WAIT_EVENT_NUM)) {
		log_fatal("CTrafficManager::work: failed to start listening");

		return;
	}

	_worker.work(this, true, true);
}

bool CTrafficManager::working() {
	// recycle nodes at first
	_mutex.lock();

	while (!_nodeQueue.empty()) {
		delNode(_nodeQueue.front());
		_nodeQueue.pop();
	}

	_mutex.unlock();

	struct epoll_event events[Config::App::EPOLL_WAIT_EVENT_NUM];

	// waiting until timeout (1 second)
	int fds = epoll_wait(_epollFd, events, Config::App::EPOLL_WAIT_EVENT_NUM,
			1000);

	if (-1 == fds) {
		switch (errno) {
		case EBADF:
			log_fatal(
					"CTrafficManager::working: failed to call epoll_wait-EBADF");
			return false;
		case EFAULT:
			log_fatal(
					"CTrafficManager::working: failed to call epoll_wait-EFAULT");
			return false;
		case EINTR:
			return true;
		case EINVAL:
			log_fatal(
					"CTrafficManager::working: failed to call epoll_wait-EINVAL.");
			return false;
		default:
			log_fatal(
					"CTrafficManager::working: failed to call epoll_wait-Unknown.");
			assert(false);
			return false;
		}
	}

	for (int i = 0; i < fds; i++) {
		// handle accept socket
		if (events[i].data.fd == _listenFd) {
			assert(events[i].events & EPOLLIN);
			addNodes();

			continue;
		}

		CNode *node = (CNode *) events[i].data.u64;
		assert(node);

		if (events[i].events & EPOLLRDHUP) {
			log_debug("[%p %s:%u]CTrafficManager::working: node broken",
					node->getTransaction(), node->getIp(), node->getPort());
			node->getTransaction()->over(CONNECTION_BROKEN);

			continue;
		}

		if (events[i].events & EPOLLERR) {
			log_debug("[%p %s:%u]CTrafficManager::working: error happens",
					node->getTransaction(), node->getIp(), node->getPort());
			node->getTransaction()->over(CONNECTION_ERROR);

			continue;
		}

		// receive data from client
		if (events[i].events & EPOLLIN) {
			if (false == node->recv()) {
				log_debug(
						"[%p %s:%u]CTrafficManager::working: failed to receive data",
						node->getTransaction(), node->getIp(), node->getPort());
				node->getTransaction()->over(CANNOT_RECV_DATA);
			}
		}
	}

	return _running;
}

void CTrafficManager::addNodes() {
	for (;;) {
		struct sockaddr_in addr;
		socklen_t len = sizeof(struct sockaddr_in);
		int clientFd = accept(_listenFd, (struct sockaddr *) &addr, &len);

		if (-1 == clientFd) {
			if (errno != EAGAIN) {
				log_error("CTrafficManager::addNodes: failed to accept socket");
			}

			return;
		}

		// refuse any client if the total user has got the max value.
		if (0 == _res.size()) {
			log_crit("CTrafficManager::addNodes: cannot accept more nodes");
			close(clientFd);

			return;
		}

		CNode *node = _res.allocate();
		CNodeGroup *group = allocateGroup();

		group->attach(node, inet_ntoa(addr.sin_addr), ntohs(addr.sin_port),
				clientFd);
		setNonBlocking(clientFd);

		struct epoll_event ev;

		ev.data.u64 = (long unsigned int) node;
		ev.events = EPOLLIN | EPOLLRDHUP | EPOLLET;

		if (-1 == epoll_ctl(_epollFd, EPOLL_CTL_ADD, clientFd, &ev)) {
			switch (errno) {
			case EBADF:
				log_fatal(
						"CTrafficManager::addNodes: failed to call epoll_ctrl-EBADF");
				break;
			case EEXIST:
				log_fatal(
						"CTrafficManager::addNodes: failed to call epoll_ctrl-EEXIST");
				break;
			case EINVAL:
				log_fatal(
						"CTrafficManager::addNodes: failed to call epoll_ctrl-EINVAL");
				break;
			case ENOENT:
				log_fatal(
						"CTrafficManager::addNodes: failed to call epoll_ctrl-ENOENT");
				break;
			case ENOMEM:
				log_fatal(
						"CTrafficManager::addNodes: failed to call epoll_ctrl-ENOMEM");
				break;
			case ENOSPC:
				log_fatal(
						"CTrafficManager::addNodes: failed to call epoll_ctrl-ENOSPC");
				break;
			case EPERM:
				log_fatal(
						"CTrafficManager::addNodes: failed to call epoll_ctrl-EPERM");
				break;
			default:
				log_fatal(
						"CTrafficManager::addNodes: failed to call epoll_ctrl-Unknown.");
			}

			group->detach(node);

			return;
		}

		log_debug("[%p %s:%u]CTrafficManager::addNodes: accepted node",
				node->getTransaction(), node->getIp(), node->getPort());
	}
}

void CTrafficManager::delNode(CNode *node) {
	if (FREE == node->getTransaction()->getStatus() || 0 == node->getFd()) {
		return;
	}

	log_debug("[%p %s:%u]CTrafficManager::delNode: release node",
			node->getTransaction(), node->getIp(), node->getPort());

	if (-1 == epoll_ctl(_epollFd, EPOLL_CTL_DEL, node->getFd(), NULL)) {
		switch (errno) {
		case EBADF:
			log_fatal(
					"CTrafficManager::delNode: failed to call epoll_ctrl-EBADF");
			break;
		case EEXIST:
			log_fatal(
					"CTrafficManager::delNode: failed to call epoll_ctrl-EEXIST");
			break;
		case EINVAL:
			log_fatal(
					"CTrafficManager::delNode: failed to call epoll_ctrl-EINVAL");
			break;
		case ENOENT:
			log_fatal(
					"CTrafficManager::delNode: failed to call epoll_ctrl-ENOENT");
			break;
		case ENOMEM:
			log_fatal(
					"CTrafficManager::delNode: failed to call epoll_ctrl-ENOMEM");
			break;
		case ENOSPC:
			log_fatal(
					"CTrafficManager::delNode: failed to call epoll_ctrl-ENOSPC");
			break;
		case EPERM:
			log_fatal(
					"CTrafficManager::delNode: failed to call epoll_ctrl-EPERM");
			break;
		default:
			log_fatal(
					"CTrafficManager::delNode: failed to call epoll_ctrl-Unknown.");
		}
	}

	close(node->getFd());
	node->getGroup()->detach(node);
	_res.reclaim(node);
}

void CTrafficManager::recycleNode(CNode *node) {
	assert(NULL != node);
	_mutex.lock();
	_nodeQueue.push(node);
	_mutex.unlock();
}

void CTrafficManager::setNonBlocking(int socket) {
	int iOpts = fcntl(socket, F_GETFL);

	if (0 > iOpts) {
		log_error(
				"CTrafficManager::setNonBlocking: failed to call fcntl with F_GETFL");
	}

	iOpts |= O_NONBLOCK;

	if (0 > fcntl(socket, F_SETFL, iOpts)) {
		log_error(
				"CTrafficManager::setNonBlocking: failed to call fctntl with F_SETFL");
	}
}

CNodeGroup *CTrafficManager::allocateGroup() {
	if (_curGroupIndex == Config::App::NODE_GROUP_NUM) {
		_curGroupIndex = 0;
	}

	return &_groups[_curGroupIndex++];
}
